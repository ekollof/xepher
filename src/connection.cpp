// This->Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <optional>
#include <charconv>
#include <thread>
#include <filesystem>
#include <array>
#include <list>
#include <ranges>
#include <time.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <curl/curl.h>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include <openssl/rand.h>
#include <fmt/core.h>
#include <fmt/chrono.h>
#include <libxml/uri.h>
#include <utility>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "xmpp/node.hh"
#include "xmpp/stanza.hh"
#include "xmpp/atom.hh"
#include "config.hh"
#include "account.hh"
#include "user.hh"
#include "channel.hh"
#include "connection.hh"
#include "omemo.hh"
#include "pgp.hh"
#include "util.hh"
#include "avatar.hh"
#include "debug.hh"
#include "xmpp/xep-0054.inl"
#include "xmpp/xep-0084.inl"
#include "xmpp/xep-0172.inl"
#include "xmpp/xep-0292.inl"

extern "C" {
#include "diff/diff.h"
}

namespace {

[[nodiscard]] auto parse_omemo_device_id(const char *value) -> std::optional<std::uint32_t>
{
    if (!value || !*value)
        return std::nullopt;

    std::uint32_t parsed = 0;
    const auto *begin = value;
    const auto *end = value + std::string_view(value).size();
    const auto [ptr, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc {} || ptr != end || parsed == 0 || parsed > 0x7fffffffU)
        return std::nullopt;

    return parsed;
}

[[nodiscard]] auto raw_xml_trace_path(weechat::account &account) -> std::string
{
    std::shared_ptr<char> eval_path(
        weechat_string_eval_expression(
            fmt::format("${{weechat_data_dir}}/xmpp/raw_xml_{}.log", account.name).c_str(),
            NULL, NULL, NULL),
        &free);
    return eval_path ? std::string(eval_path.get()) : std::string {};
}

void append_raw_xml_trace(weechat::account &account,
                          const char *direction,
                          xmpp_stanza_t *stanza)
{
    if (!stanza)
        return;

    if (!xmpp_raw_xml_log_is_on())
        return;

    const auto path = raw_xml_trace_path(account);
    if (path.empty())
        return;

    try
    {
        std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    }
    catch (...)
    {
        return;
    }

    char *xml = nullptr;
    size_t xml_len = 0;
    xmpp_stanza_to_text(stanza, &xml, &xml_len);
    if (!xml)
        return;
    struct xmpp_string_cleanup {
        xmpp_conn_t *conn; char *ptr;
        ~xmpp_string_cleanup() { if (ptr) xmpp_free(xmpp_conn_get_context(conn), ptr); }
    } xml_guard{ account.connection, xml };

    FILE *fp = fopen(path.c_str(), "a");
    if (!fp)
        return;

    time_t now = time(NULL);
    struct tm local_tm = {0};
    localtime_r(&now, &local_tm);
    std::string timestamp = fmt::format("{:%Y-%m-%d %H:%M:%S}", local_tm);

    const char *stanza_name = xmpp_stanza_get_name(stanza);
    fprintf(fp, "[%s] %s %s\n%s\n\n",
            timestamp.c_str(),
            direction ? direction : "XML",
            stanza_name ? stanza_name : "(unknown)",
            xml);
    fclose(fp);
}

}

void weechat::connection::init()
{
    srand(time(NULL));
    libstrophe::initialize();
}

void weechat::connection::send(xmpp_stanza_t *stanza)
{
    append_raw_xml_trace(account, "SEND", stanza);

    // Increment outbound counter for actual stanzas (not SM elements)
    const char *name = xmpp_stanza_get_name(stanza);
    if (name && account.sm_enabled)
    {
        std::string stanza_name(name);
        if (stanza_name == "message" || stanza_name == "presence" || stanza_name == "iq")
        {
            account.sm_h_outbound++;
            // Keep a copy in the retransmit queue (XEP-0198 §5)
            xmpp_stanza_t *copy = xmpp_stanza_copy(stanza);
            account.sm_outqueue.emplace_back(
                account.sm_h_outbound,
                std::shared_ptr<xmpp_stanza_t>(copy, xmpp_stanza_release));
        }
    }
    m_conn.send(stanza);
}

bool weechat::connection::version_handler(xmpp_stanza_t *stanza)
{
    const char *weechat_name = "weechat";
    std::unique_ptr<char, decltype(&free)> weechat_version(weechat_info_get("version", NULL), free);

    XDEBUG("Received version request from {}", xmpp_stanza_get_from(stanza));

    auto reply = libstrophe::stanza::reply(stanza)
        .set_type("result");

    auto query = libstrophe::stanza(account.context)
        .set_name("query");
    if (const char *ns = xmpp_stanza_get_ns(xmpp_stanza_get_children(stanza)); ns) {
        query.set_ns(ns);
    }

    query.add_child(libstrophe::stanza(account.context)
                    .set_name("name")
                    .add_child(libstrophe::stanza(account.context)
                               .set_text(weechat_name)));
    query.add_child(libstrophe::stanza(account.context)
                    .set_name("version")
                    .add_child(libstrophe::stanza(account.context)
                               .set_text(weechat_version.get())));

    reply.add_child(std::move(query));

    account.connection.send(reply);

    return true;
}

bool weechat::connection::time_handler(xmpp_stanza_t *stanza)
{
    XDEBUG("Received time request from {}", xmpp_stanza_get_from(stanza));

    auto reply = libstrophe::stanza::reply(stanza)
        .set_type("result");

    auto query = libstrophe::stanza(account.context)
        .set_name("time");
    if (const char *ns = xmpp_stanza_get_ns(xmpp_stanza_get_children(stanza)); ns) {
        query.set_ns(ns);
    }

    // Get current time
    time_t now = time(NULL);
    struct tm *tm_utc = gmtime(&now);
    struct tm *tm_local = localtime(&now);

    // Format UTC time as ISO 8601: YYYY-MM-DDTHH:MM:SSZ
    std::string utc_str = fmt::format("{:%Y-%m-%dT%H:%M:%SZ}", *tm_utc);
    
    // Calculate timezone offset
    long tz_offset = tm_local->tm_gmtoff;  // Offset in seconds
    int tz_hours = tz_offset / 3600;
    int tz_mins = abs((tz_offset % 3600) / 60);
    std::string tzo_str = fmt::format("{:+03d}:{:02d}", tz_hours, tz_mins);

    query.add_child(libstrophe::stanza(account.context)
                    .set_name("utc")
                    .add_child(libstrophe::stanza(account.context)
                               .set_text(utc_str.c_str())));
    query.add_child(libstrophe::stanza(account.context)
                    .set_name("tzo")
                    .add_child(libstrophe::stanza(account.context)
                               .set_text(tzo_str.c_str())));

    reply.add_child(std::move(query));

    account.connection.send(reply);

    return true;
}

#include "connection/presence_handler.inl"

// XEP-0466: Ephemeral Messages — tombstone timer callback.
// Fires N seconds after an ephemeral message is displayed.
// Finds the message in the buffer by id_ tag and replaces its text with a tombstone.
//
// Lifetime: ctx objects live in g_ephemeral_pending (a static list).  No raw
// new/delete — the list owns the storage; the callback erases the element.
struct ephemeral_tombstone_ctx {
    struct t_gui_buffer *buffer;
    std::string msg_id;
};

// RAII storage for pending ephemeral tombstones.  Elements are stable (list
// iterator / pointer invalidation rules) so it is safe to hand a raw pointer
// into the list to weechat_hook_timer.
static std::list<ephemeral_tombstone_ctx> g_ephemeral_pending;

static int ephemeral_tombstone_cb(const void *pointer, void *data, int remaining_calls)
{
    (void) data;
    (void) remaining_calls;

    if (!pointer)
        return WEECHAT_RC_OK;

    const auto *ctx = static_cast<const ephemeral_tombstone_ctx *>(pointer);
    struct t_gui_buffer *buf = ctx->buffer;
    const std::string &msg_id = ctx->msg_id;

    if (buf)
    {
        // Search buffer lines for the one tagged id_<msg_id>
        std::string id_tag = "id_" + msg_id;
        void *lines = weechat_hdata_pointer(weechat_hdata_get("buffer"), buf, "lines");
        if (lines)
        {
            void *last_line = weechat_hdata_pointer(weechat_hdata_get("lines"), lines, "last_line");
            while (last_line)
            {
                void *line_data = weechat_hdata_pointer(weechat_hdata_get("line"),
                                                        last_line, "data");
                if (line_data)
                {
                    int tags_count = weechat_hdata_integer(weechat_hdata_get("line_data"),
                                                           line_data, "tags_count");
                    bool found = false;
                    for (int n = 0; n < tags_count && !found; ++n)
                    {
                        std::string tag_key = fmt::format("{}|tags_array", n);
                        const char *tag = weechat_hdata_string(weechat_hdata_get("line_data"),
                                                               line_data, tag_key.c_str());
                        if (tag && id_tag == tag)
                            found = true;
                    }
                    if (found)
                    {
                        // Replace message text with tombstone
                        struct t_hashtable *props = weechat_hashtable_new(
                            4, WEECHAT_HASHTABLE_STRING, WEECHAT_HASHTABLE_STRING,
                            nullptr, nullptr);
                        if (props)
                        {
                            weechat_hashtable_set(props, "message",
                                "\x1b[9m[This message has disappeared]\x1b[0m");
                            weechat_hdata_update(weechat_hdata_get("line_data"), line_data, props);
                            weechat_hashtable_free(props);
                        }
                        break;
                    }
                }
                last_line = weechat_hdata_pointer(weechat_hdata_get("line"),
                                                  last_line, "prev_line");
            }
        }
    }

    // Erase from the owning list — no delete needed.
    g_ephemeral_pending.remove_if([ctx](const ephemeral_tombstone_ctx &e) {
        return &e == ctx;
    });

    return WEECHAT_RC_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// XEP-0448: Encrypted File Sharing — receive-side download + decrypt.
//
// When an incoming <file-sharing> contains <encrypted xmlns='urn:xmpp:esfs:0'>
// we: (1) download the ciphertext in a worker thread via libcurl, (2) AES-256-GCM
// decrypt it, (3) save the plaintext to ~/Downloads/<filename>, (4) print the
// local path to the channel buffer.
//
// Lifetime: esfs_download_ctx objects live in g_esfs_downloads (a static list).
// No raw new/delete — the list owns the storage.
// ─────────────────────────────────────────────────────────────────────────────
struct esfs_download_ctx {
    // Inputs (set before thread starts)
    std::string cipher_url;    // HTTPS URL to the ciphertext blob
    std::string filename;      // original filename for the saved file
    std::string key_b64;       // Base64(AES-256 key, 32 bytes)
    std::string iv_b64;        // Base64(GCM IV, 12 bytes)
    struct t_gui_buffer *buffer = nullptr;

    // Communication pipe (read-end watched by weechat_hook_fd)
    int pipe_read_fd  = -1;
    int pipe_write_fd = -1;
    struct t_hook *hook = nullptr;

    // Result (written by thread, read by callback)
    bool success = false;
    std::string saved_path;    // local file path on success
    std::string error_msg;     // human-readable error on failure

    std::thread worker;
};

static std::list<esfs_download_ctx> g_esfs_downloads;

// Base64-decode helper using OpenSSL BIO (returns decoded bytes or empty on error).
static std::vector<unsigned char> esfs_b64_decode(const std::string &b64)
{
    if (b64.empty()) return {};
    std::unique_ptr<BIO, decltype(&BIO_free_all)>
        chain(BIO_push(BIO_new(BIO_f_base64()), BIO_new_mem_buf(b64.data(), static_cast<int>(b64.size()))),
              BIO_free_all);
    BIO_set_flags(chain.get(), BIO_FLAGS_BASE64_NO_NL);
    std::vector<unsigned char> out(b64.size()); // upper bound
    int n = BIO_read(chain.get(), out.data(), static_cast<int>(out.size()));
    if (n <= 0) return {};
    out.resize(static_cast<size_t>(n));
    return out;
}

// curl write callback: appends received data to a std::vector<char>*.
static size_t esfs_curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *buf = static_cast<std::vector<unsigned char> *>(userdata);
    buf->insert(buf->end(), reinterpret_cast<unsigned char *>(ptr),
                reinterpret_cast<unsigned char *>(ptr) + size * nmemb);
    return size * nmemb;
}

static int esfs_download_cb(const void *pointer, void *data, int fd)
{
    (void) pointer;
    (void) data;

    if (weechat::g_plugin_unloading || !weechat::plugin::instance)
        return WEECHAT_RC_OK;

    // Drain pipe
    char sig[1];
    (void)::read(fd, sig, sizeof(sig));

    // Find the ctx in the list by pipe fd
    auto it = std::find_if(g_esfs_downloads.begin(), g_esfs_downloads.end(),
                           [fd](const esfs_download_ctx &c) { return c.pipe_read_fd == fd; });
    if (it == g_esfs_downloads.end())
        return WEECHAT_RC_ERROR;

    esfs_download_ctx &ctx = *it;

    if (ctx.worker.joinable())
        ctx.worker.join();

    if (ctx.hook)
        weechat_unhook(ctx.hook);
    close(ctx.pipe_read_fd);
    if (ctx.pipe_write_fd >= 0)
        close(ctx.pipe_write_fd);

    if (ctx.success)
        weechat_printf(ctx.buffer, "%s[ESFS] Saved decrypted file: %s",
                       weechat_prefix("network"), ctx.saved_path.c_str());
    else
        weechat_printf(ctx.buffer, "%s[ESFS] Download/decrypt failed: %s",
                       weechat_prefix("error"), ctx.error_msg.c_str());

    g_esfs_downloads.erase(it);
    return WEECHAT_RC_OK;
}

// Called from message_handler.inl when an encrypted SFS source is detected.
// Captures everything needed, allocates a ctx in g_esfs_downloads, launches thread.
static void esfs_start_download(const std::string &cipher_url,
                                const std::string &filename,
                                const std::string &key_b64,
                                const std::string &iv_b64,
                                struct t_gui_buffer *buf)
{
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0)
    {
        weechat_printf(buf, "%s[ESFS] Failed to create pipe for download",
                       weechat_prefix("error"));
        return;
    }

    g_esfs_downloads.push_back({});
    esfs_download_ctx &ctx = g_esfs_downloads.back();
    ctx.cipher_url    = cipher_url;
    ctx.filename      = filename;
    ctx.key_b64       = key_b64;
    ctx.iv_b64        = iv_b64;
    ctx.buffer        = buf;
    ctx.pipe_read_fd  = pipe_fds[0];
    ctx.pipe_write_fd = pipe_fds[1];

    ctx.hook = weechat_hook_fd(pipe_fds[0], 1, 0, 0,
                               esfs_download_cb, nullptr, nullptr);

    // Determine downloads directory: $XDG_DOWNLOAD_DIR or ~/Downloads
    std::string downloads_dir;
    {
        const char *xdg = getenv("XDG_DOWNLOAD_DIR");
        if (xdg && *xdg)
        {
            downloads_dir = xdg;
        }
        else
        {
            const char *home = getenv("HOME");
            downloads_dir = home ? std::string(home) + "/Downloads" : "/tmp";
        }
    }

    ctx.worker = std::thread([&ctx, downloads_dir]()
    {
        // ── 1. Decode key and IV ──
        auto key_bytes = esfs_b64_decode(ctx.key_b64);
        auto iv_bytes  = esfs_b64_decode(ctx.iv_b64);

        if (key_bytes.size() != 32 || iv_bytes.size() != 12)
        {
            ctx.success   = false;
            ctx.error_msg = "esfs: invalid key or IV length after base64 decode";
            ::write(ctx.pipe_write_fd, "x", 1);
            return;
        }

        // ── 2. Download ciphertext via libcurl ──
        std::vector<unsigned char> ciphertext;
        {
            CURL *curl = curl_easy_init();
            if (!curl)
            {
                ctx.success   = false;
                ctx.error_msg = "esfs: curl_easy_init failed";
                ::write(ctx.pipe_write_fd, "x", 1);
                return;
            }
            curl_easy_setopt(curl, CURLOPT_URL, ctx.cipher_url.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, esfs_curl_write_cb);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ciphertext);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            CURLcode res = curl_easy_perform(curl);
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            curl_easy_cleanup(curl);

            if (res != CURLE_OK || (http_code != 200 && http_code != 206))
            {
                ctx.success   = false;
                ctx.error_msg = fmt::format("esfs: download failed (curl={} http={})",
                                            curl_easy_strerror(res), http_code);
                ::write(ctx.pipe_write_fd, "x", 1);
                return;
            }
        }

        // Ciphertext must be at least 16 bytes (GCM auth tag).
        if (ciphertext.size() < 16)
        {
            ctx.success   = false;
            ctx.error_msg = "esfs: ciphertext too short to contain auth tag";
            ::write(ctx.pipe_write_fd, "x", 1);
            return;
        }

        // ── 3. AES-256-GCM decrypt ──
        // Layout: [ciphertext bytes…][16-byte GCM tag]
        size_t ct_len  = ciphertext.size() - 16;
        unsigned char *ct_data  = ciphertext.data();
        unsigned char *tag_data = ciphertext.data() + ct_len;

        std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>
            dec_ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);

        if (!dec_ctx
            || EVP_DecryptInit_ex(dec_ctx.get(), EVP_aes_256_gcm(),
                                  nullptr, nullptr, nullptr) != 1
            || EVP_CIPHER_CTX_ctrl(dec_ctx.get(), EVP_CTRL_GCM_SET_IVLEN,
                                   12, nullptr) != 1
            || EVP_DecryptInit_ex(dec_ctx.get(), nullptr,
                                  nullptr, key_bytes.data(), iv_bytes.data()) != 1)
        {
            ctx.success   = false;
            ctx.error_msg = "esfs: EVP decrypt init failed";
            ::write(ctx.pipe_write_fd, "x", 1);
            return;
        }

        std::vector<unsigned char> plaintext(ct_len + 16);
        int out_len = 0;
        if (ct_len > 0
            && EVP_DecryptUpdate(dec_ctx.get(), plaintext.data(), &out_len,
                                 ct_data, static_cast<int>(ct_len)) != 1)
        {
            ctx.success   = false;
            ctx.error_msg = "esfs: EVP_DecryptUpdate failed";
            ::write(ctx.pipe_write_fd, "x", 1);
            return;
        }
        size_t pt_len = static_cast<size_t>(out_len);

        // Set the expected GCM tag before finalising.
        EVP_CIPHER_CTX_ctrl(dec_ctx.get(), EVP_CTRL_GCM_SET_TAG, 16, tag_data);

        int final_len = 0;
        int final_rc = EVP_DecryptFinal_ex(dec_ctx.get(),
                                           plaintext.data() + pt_len, &final_len);
        if (final_rc != 1)
        {
            ctx.success   = false;
            ctx.error_msg = "esfs: GCM tag verification failed (authentication error)";
            ::write(ctx.pipe_write_fd, "x", 1);
            return;
        }
        pt_len += static_cast<size_t>(final_len);
        plaintext.resize(pt_len);

        // ── 4. Save to Downloads directory ──
        // Sanitize filename: replace path separators.
        std::string safe_name = ctx.filename;
        for (char &c : safe_name)
            if (c == '/' || c == '\0') c = '_';
        if (safe_name.empty()) safe_name = "esfs_file";

        std::string out_path = downloads_dir + "/" + safe_name;

        // If file already exists, add a numeric suffix.
        {
            std::string candidate = out_path;
            int suffix = 1;
            struct stat st{};
            while (::stat(candidate.c_str(), &st) == 0)
                candidate = out_path + "." + std::to_string(suffix++);
            out_path = candidate;
        }

        // Ensure the downloads directory exists.
        mkdir(downloads_dir.c_str(), 0755);

        {
            auto file_deleter = [](FILE *f) { if (f) fclose(f); };
            std::unique_ptr<FILE, decltype(file_deleter)>
                out_file(fopen(out_path.c_str(), "wb"), file_deleter);
            if (!out_file)
            {
                ctx.success   = false;
                ctx.error_msg = fmt::format("esfs: failed to open output file: {}", out_path);
                ::write(ctx.pipe_write_fd, "x", 1);
                return;
            }
            if (pt_len > 0)
                fwrite(plaintext.data(), 1, pt_len, out_file.get());
        }

        ctx.saved_path = out_path;
        ctx.success    = true;
        ::write(ctx.pipe_write_fd, "x", 1);
    });
}

#include "connection/message_handler.inl"

#include "connection/iq_handler.inl"

#include "connection/session_lifecycle.inl"
