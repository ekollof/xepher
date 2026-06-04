// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// Shared helper implementations used by connection handler translation units.
// Definitions of everything declared in connection/internal.hh.

#include <algorithm>
#include <expected>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <list>
#include <ranges>
#include <span>
#include <string>
#include <thread>
#include <vector>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include <curl/curl.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include <fmt/core.h>
#include <fmt/chrono.h>
#include <weechat/weechat-plugin.h>
#include <strophe.h>

#include "account.hh"
#include "debug.hh"
#include "util.hh"
#include "connection/internal.hh"

// ── parse_omemo_device_id ─────────────────────────────────────────────────────

[[nodiscard]] std::optional<std::uint32_t> parse_omemo_device_id(const char *value)
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

// ── raw XML trace helpers ─────────────────────────────────────────────────────

[[nodiscard]] std::string raw_xml_trace_path(weechat::account &account)
{
    std::shared_ptr<char> eval_path(
        weechat_string_eval_expression(
            fmt::format("${{weechat_data_dir}}/xmpp/raw_xml_{}.log", account.name).c_str(),
            nullptr, nullptr, nullptr),
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

    time_t now = time(nullptr);
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

// ── Ephemeral-message tombstone timer ─────────────────────────────────────────

std::list<ephemeral_tombstone_ctx> g_ephemeral_pending;

int ephemeral_tombstone_cb(const void *pointer, void *data, int remaining_calls)
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

    g_ephemeral_pending.remove_if([ctx](const ephemeral_tombstone_ctx &e) {
        return &e == ctx;
    });

    return WEECHAT_RC_OK;
}

// ── XEP-0448 Encrypted File Sharing ──────────────────────────────────────────

std::list<esfs_download_ctx> g_esfs_downloads;

// Base64-decode helper using OpenSSL BIO.
// Returns unexpected on decode error, or the decoded bytes on success
// (empty input produces empty output — that's valid).
[[nodiscard]] static auto esfs_b64_decode(std::string_view b64)
    -> std::expected<std::vector<unsigned char>, std::string>
{
    if (b64.empty()) return std::vector<unsigned char>{};
    std::unique_ptr<BIO, decltype(&BIO_free_all)>
        chain(BIO_push(BIO_new(BIO_f_base64()), BIO_new_mem_buf(b64.data(), static_cast<int>(b64.size()))),
              BIO_free_all);
    if (!chain) return std::unexpected("esfs: BIO allocation failed");
    BIO_set_flags(chain.get(), BIO_FLAGS_BASE64_NO_NL);
    std::vector<unsigned char> out(b64.size());
    std::span<unsigned char> out_view{out};
    int n = BIO_read(chain.get(), out_view.data(), static_cast<int>(out_view.size()));
    if (n <= 0) return std::unexpected("esfs: base64 decode failed");
    out.resize(static_cast<size_t>(n));
    return out;
}

// curl write callback: appends received data to a std::vector<unsigned char>*.
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

    char sig[1];
    (void)::read(fd, sig, sizeof(sig));

    auto it = std::ranges::find_if(g_esfs_downloads,
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
    {
        weechat_printf(ctx.buffer, "%s[ESFS] Saved decrypted file: %s",
                       weechat_prefix("network"), ctx.saved_path.c_str());
        // Persist the download record to LMDB so MAM replay skips re-downloading.
        if (ctx.account_ptr && !ctx.channel_jid.empty() && !ctx.stable_id.empty())
            ctx.account_ptr->mam_cache_store_esfs_download(
                ctx.channel_jid, ctx.stable_id, ctx.saved_path);

        // weechat-icat: display decrypted image inline via Kitty graphics protocol.
        if (ctx.account_ptr && weechat::config::instance &&
            weechat_config_boolean(weechat::config::instance->look.icat))
        {
            std::string_view fname = ctx.filename;
            if (fname.ends_with(".jpg") || fname.ends_with(".jpeg") || fname.ends_with(".png")
                || fname.ends_with(".gif") || fname.ends_with(".webp"))
            {
                std::string icat_cmd = fmt::format("/icat {}", ctx.saved_path);
                weechat_command(ctx.buffer, icat_cmd.c_str());
            }
        }
    }
    else
        weechat_printf(ctx.buffer, "%s[ESFS] Download/decrypt failed: %s",
                       weechat_prefix("error"), ctx.error_msg.c_str());

    g_esfs_downloads.erase(it);
    return WEECHAT_RC_OK;
}

void esfs_start_download(std::string_view cipher_url,
                         std::string_view filename,
                         std::string_view key_b64,
                         std::string_view iv_b64,
                         struct t_gui_buffer *buf,
                         weechat::account *account_ptr,
                         std::string_view channel_jid,
                         std::string_view stable_id)
{
    // Layer 1: in-flight dedup — same cipher_url already being downloaded this session.
    bool already_inflight = std::ranges::any_of(g_esfs_downloads,
                                        [&](const esfs_download_ctx &c) {
                                            return c.cipher_url == cipher_url;
                                        });
    if (already_inflight)
        return;
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0)
    {
        weechat_printf(buf, "%s[ESFS] Failed to create pipe for download",
                       weechat_prefix("error"));
        return;
    }

    g_esfs_downloads.push_back({});
    esfs_download_ctx &ctx = g_esfs_downloads.back();
    ctx.cipher_url    = std::string(cipher_url);
    ctx.filename      = std::string(filename);
    ctx.key_b64       = std::string(key_b64);
    ctx.iv_b64        = std::string(iv_b64);
    ctx.buffer        = buf;
    ctx.account_ptr   = account_ptr;
    ctx.channel_jid   = std::string(channel_jid);
    ctx.stable_id     = std::string(stable_id);
    ctx.pipe_read_fd  = pipe_fds[0];
    ctx.pipe_write_fd = pipe_fds[1];

    ctx.hook = weechat_hook_fd(pipe_fds[0], 1, 0, 0,
                               esfs_download_cb, nullptr, nullptr);

    std::string downloads_dir;
    {
        // Determine base downloads directory from XDG or HOME.
        std::string base;
        const char *xdg = getenv("XDG_DOWNLOAD_DIR");
        if (xdg && *xdg)
        {
            base = xdg;
        }
        else
        {
            const char *home = getenv("HOME");
            base = home ? std::string(home) + "/Downloads" : "/tmp";
        }

        // Build date subfolder (local wall-clock time at download).
        time_t now = time(nullptr);
        struct tm tm_now{};
        localtime_r(&now, &tm_now);
        char date_buf[16];
        strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &tm_now);

        // Sanitize JID for use as a directory name: replace '/' with '_'
        // (bare JIDs don't contain '/' but be defensive; '\0' can't appear).
        std::string safe_jid = ctx.channel_jid;
        std::ranges::for_each(safe_jid, [](char &c){ if (c == '/') c = '_'; });

        downloads_dir = base + "/xmpp/" + safe_jid + "/" + date_buf;
    }

    ctx.worker = std::thread([&ctx, downloads_dir]()
    {
        auto key_bytes = esfs_b64_decode(ctx.key_b64);
        auto iv_bytes  = esfs_b64_decode(ctx.iv_b64);

        if (!key_bytes || !iv_bytes || key_bytes->size() != 32 || iv_bytes->size() != 12)
        {
            ctx.success   = false;
            ctx.error_msg = !key_bytes ? key_bytes.error()
                          : !iv_bytes  ? iv_bytes.error()
                          : fmt::format("esfs: invalid key/IV length (key={} IV={})",
                                        key_bytes->size(), iv_bytes->size());
            ::write(ctx.pipe_write_fd, "x", 1);
            return;
        }

        std::vector<unsigned char> ciphertext;
        {
            CURL *curl = curl_easy_init();
            if (!curl)
            {
                ctx.error_msg = "esfs: curl_easy_init failed";
                ::write(ctx.pipe_write_fd, "x", 1);
                return;
            }

            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
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

        if (ciphertext.size() < 16)
        {
            ctx.success   = false;
            ctx.error_msg = "esfs: ciphertext too short to contain auth tag";
            ::write(ctx.pipe_write_fd, "x", 1);
            return;
        }

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
                                  nullptr, key_bytes->data(), iv_bytes->data()) != 1)
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

        std::string safe_name = ctx.filename;
        std::ranges::for_each(safe_name, [](char &c){ if (c == '/' || c == '\0') c = '_'; });
        if (safe_name.empty()) safe_name = "esfs_file";

        std::string out_path = downloads_dir + "/" + safe_name;

        {
            std::string candidate = out_path;
            int suffix = 1;
            struct stat st{};
            while (::stat(candidate.c_str(), &st) == 0)
                candidate = out_path + "." + std::to_string(suffix++);
            out_path = candidate;
        }

        std::filesystem::create_directories(downloads_dir);

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

// ── OG / HTML-title async URL preview fetch ───────────────────────────────────

std::list<og_fetch_ctx>    g_og_fetches;
std::list<og_pending_entry> g_og_pending;

// Case-insensitive ASCII substring search.
static size_t ifinds(std::string_view hay, std::string_view needle, size_t from = 0)
{
    if (needle.empty()) return from;
    auto sub = hay.substr(from);
    auto res = std::ranges::search(sub, needle,
        [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        });
    if (res.empty()) return std::string::npos;
    return from + static_cast<size_t>(res.begin() - sub.begin());
}

// Extract the value of an HTML attribute from a tag string.
// e.g. attr_value("<meta property=\"og:title\" content=\"Hello\">", "content") → "Hello"
static std::string attr_value(std::string_view tag, std::string_view attr)
{
    std::string needle = std::string(attr) + "=";
    size_t pos = ifinds(tag, needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    if (pos >= tag.size()) return {};
    char q = tag[pos];
    if (q == '"' || q == '\'')
    {
        ++pos;
        size_t end = tag.find(q, pos);
        if (end == std::string::npos) end = tag.size();
        return std::string(tag.substr(pos, end - pos));
    }
    // Unquoted attribute value: scan to next whitespace or >
    size_t end = pos;
    while (end < tag.size() && tag[end] != '>' && !std::isspace((unsigned char)tag[end]))
        ++end;
    return std::string(tag.substr(pos, end - pos));
}

// Decode basic HTML entities: &amp; &lt; &gt; &quot; &apos; &#NNN; &#xNNN;
static std::string decode_html_entities(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); )
    {
        if (s[i] != '&')
        {
            out += s[i++];
            continue;
        }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10)
        {
            out += s[i++];
            continue;
        }
        std::string ent = std::string(s.substr(i + 1, semi - i - 1));
        if (ent == "amp")       out += '&';
        else if (ent == "lt")   out += '<';
        else if (ent == "gt")   out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (!ent.empty() && ent[0] == '#')
        {
            unsigned long cp = 0;
            if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                cp = std::strtoul(ent.c_str() + 2, nullptr, 16);
            else
                cp = std::strtoul(ent.c_str() + 1, nullptr, 10);
            // Encode as UTF-8 (only BMP for simplicity)
            if (cp < 0x80)        out += static_cast<char>(cp);
            else if (cp < 0x800)  { out += static_cast<char>(0xC0 | (cp >> 6));   out += static_cast<char>(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000){ out += static_cast<char>(0xE0 | (cp >> 12));  out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
            else                   out += '?';
        }
        else out += std::string(s.substr(i, semi - i + 1)); // unknown entity: pass through
        i = semi + 1;
    }
    return out;
}

// Parse OG meta tags and <title> from an HTML string.
// Scans only the <head> section (up to </head> or <body>) for speed.
static og_fetch_ctx::preview_t parse_og_from_html(std::string_view html)
{
    og_fetch_ctx::preview_t p;

    // Limit scan to <head> region
    size_t head_end = html.size();
    {
        size_t he = ifinds(html, "</head>");
        if (he != std::string::npos) head_end = he;
        size_t be = ifinds(html, "<body");
        if (be != std::string::npos && be < head_end) head_end = be;
    }
    std::string_view head = html.substr(0, head_end);

    // Scan <meta ...> tags
    size_t pos = 0;
    while (pos < head.size())
    {
        size_t tag_start = ifinds(head, "<meta", pos);
        if (tag_start == std::string::npos) break;
        size_t tag_end = head.find('>', tag_start);
        if (tag_end == std::string::npos) break;
        std::string_view tag = head.substr(tag_start, tag_end - tag_start + 1);
        pos = tag_end + 1;

        // OpenGraph: <meta property="og:X" content="...">
        std::string prop = attr_value(tag, "property");
        if (!prop.empty())
        {
            std::string content = decode_html_entities(attr_value(tag, "content"));
            if (prop == "og:title" && p.title.empty())            p.title       = content;
            else if (prop == "og:description" && p.description.empty()) p.description = content;
            else if (prop == "og:url" && p.url.empty())           p.url         = content;
            else if (prop == "og:image" && p.image.empty()
                     && std::string_view(content).starts_with("http")) p.image = content;
            continue;
        }
        // Twitter Card: <meta name="twitter:title" content="..."> etc.
        std::string name = attr_value(tag, "name");
        if (!name.empty())
        {
            std::string content = decode_html_entities(attr_value(tag, "content"));
            if (name == "twitter:title" && p.title.empty())            p.title       = content;
            else if (name == "twitter:description" && p.description.empty()) p.description = content;
            else if (name == "twitter:image" && p.image.empty()
                     && std::string_view(content).starts_with("http"))  p.image = content;
            // Also handle plain <meta name="description">
            else if (name == "description" && p.description.empty())    p.description = content;
        }
    }

    // Fallback: <title>...</title>
    if (p.title.empty())
    {
        size_t ts = ifinds(head, "<title");
        if (ts != std::string::npos)
        {
            size_t te = head.find('>', ts);
            if (te != std::string::npos)
            {
                size_t content_start = te + 1;
                size_t content_end   = ifinds(head, "</title>", content_start);
                if (content_end == std::string::npos) content_end = head.size();
                std::string_view raw = head.substr(content_start, content_end - content_start);
                // Trim whitespace
                size_t first = raw.find_first_not_of(" \t\r\n");
                size_t last  = raw.find_last_not_of(" \t\r\n");
                if (first != std::string::npos)
                    p.title = decode_html_entities(raw.substr(first, last - first + 1));
            }
        }
    }

    return p;
}

// Forward declaration — defined after og_fetch_cb (mutual recursion via drain).
static void og_launch_one(og_pending_entry entry);

static int og_fetch_cb(const void * /*pointer*/, void * /*data*/, int fd)
{
    if (weechat::g_plugin_unloading || !weechat::plugin::instance)
        return WEECHAT_RC_OK;

    char sig[1];
    (void)::read(fd, sig, sizeof(sig));

    auto it = std::ranges::find_if(g_og_fetches,
                           [fd](const og_fetch_ctx &c) { return c.pipe_read_fd == fd; });
    if (it == g_og_fetches.end())
        return WEECHAT_RC_ERROR;

    og_fetch_ctx &ctx = *it;

    if (ctx.worker.joinable())
        ctx.worker.join();

    if (ctx.hook)
        weechat_unhook(ctx.hook);
    close(ctx.pipe_read_fd);
    if (ctx.pipe_write_fd >= 0)
        close(ctx.pipe_write_fd);

    XDEBUG("og_fetch_cb: url={} http={} curl={} success={} title='{}'",
           ctx.url, ctx.http_code, ctx.curl_res, (int)ctx.success,
           ctx.preview.title.empty() ? "(none)" : ctx.preview.title);

    if (ctx.success && ctx.account_ptr
        && (!ctx.preview.title.empty() || !ctx.preview.description.empty()))
    {
        auto &p = ctx.preview;
        const std::string &display_url = p.url.empty() ? ctx.url : p.url;

        weechat::account::og_preview cached;
        cached.title       = p.title;
        cached.description = p.description;
        cached.url         = display_url;
        cached.image       = p.image;
        ctx.account_ptr->og_cache_store(ctx.url, cached);
        auto verify = ctx.account_ptr->og_cache_lookup(ctx.url);
        XDEBUG("og_cache_store: url={} verify={}",
               ctx.url, verify ? "ok" : "FAILED");

        if (!ctx.silent
            && weechat::config::instance
            && weechat::config::instance->look.incoming_link_preview.boolean())
        {
            std::string line = format_og_preview_card(
                p.title, p.description, p.url, p.image, ctx.url);

            weechat_printf_date_tags(ctx.buffer, ctx.date,
                "notify_none,no_log,xmpp_og_preview",
                "%s\t%s", ctx.display_prefix.c_str(), line.c_str());
        }
        else
        {
            // Silent fetch: print a one-line progress note to the account buffer.
            struct t_gui_buffer *acct_buf = ctx.account_ptr->buffer;
            const std::string &title_str = p.title.empty() ? display_url : p.title;
            weechat_printf(acct_buf,
                "%sxmpp/links: fetched %s \u2014 %s",
                weechat_prefix("network"),
                ctx.url.c_str(),
                title_str.c_str());
        }
    }

    g_og_fetches.erase(it);

    // Drain one entry from the pending queue now that a slot is free.
    if (!g_og_pending.empty())
    {
        og_pending_entry next = std::move(g_og_pending.front());
        g_og_pending.pop_front();
        og_launch_one(std::move(next));
    }

    return WEECHAT_RC_OK;
}

void strip_url_trailing_punct(std::string &url)
{
    // Characters that should not appear at the end of a URL when extracted
    // from natural-language text.  We strip them iteratively because a URL
    // could be wrapped in multiple layers, e.g. ("https://x.com/foo]").
    static constexpr std::string_view kTrailing = "\"'.,;:!?)>]}\u00bb";
    while (!url.empty())
    {
        // Check for single-byte punctuation
        unsigned char last = static_cast<unsigned char>(url.back());
        if (last < 0x80)
        {
            if (kTrailing.contains(static_cast<char>(last)))
            {
                url.pop_back();
                continue;
            }
        }
        else
        {
            // Multi-byte: check for UTF-8 encoded » (U+00BB = 0xC2 0xBB)
            if (url.size() >= 2
                && static_cast<unsigned char>(url[url.size()-2]) == 0xC2
                && static_cast<unsigned char>(url[url.size()-1]) == 0xBB)
            {
                url.resize(url.size() - 2);
                continue;
            }
        }
        break;
    }
}

// ── format_og_preview_card ────────────────────────────────────────────────────
// Build a simple WeeChat preview string without box-drawing characters.
// Uses darkgray for the whole block so it is visually distinct from chat text.

std::string format_og_preview_card(std::string_view title,
                                     std::string_view description,
                                     std::string_view url,
                                     std::string_view image,
                                     std::string_view fallback_url)
{
    std::string_view display_url = url.empty() ? fallback_url : url;
    std::string_view display_title = title.empty()
                                         ? (display_url.empty() ? "Link" : display_url)
                                         : title;

    std::string line;
    line += weechat_color("darkgray");
    line += weechat_color("bold");
    line += std::string(display_title);
    line += weechat_color("-bold");

    // Sanitize description: collapse any internal newlines / tabs / CRs to spaces
    std::string desc = std::string(description);
    std::ranges::for_each(desc, [](char &c){ if (c == '\n' || c == '\r' || c == '\t') c = ' '; });

    if (!desc.empty())
    {
        line += "  ";
        if (desc.size() > 120)
        {
            line += desc.substr(0, 117);
            line += "...";
        }
        else
        {
            line += desc;
        }
    }

    if (!display_url.empty() && display_url != display_title)
    {
        line += "  ";
        line += weechat_color("blue");
        line += std::string(display_url);
        line += weechat_color("darkgray");
    }

    if (!image.empty())
    {
        line += " [img]";
    }

    line += weechat_color("resetcolor");
    return line;
}

void og_start_fetch(std::string_view url,
                    struct t_gui_buffer *buf,
                    weechat::account *account_ptr,
                    std::string_view display_prefix,
                    time_t date,
                    bool silent)
{
    if (url.empty() || !account_ptr)
        return;

    // Skip non-HTTP URLs
    if (!url.starts_with("http://") && !url.starts_with("https://"))
        return;

    // Dedup: already in-flight?
    if (std::ranges::any_of(g_og_fetches,
                    [&](const og_fetch_ctx &c) { return c.url == url; }))
        return;

    // Dedup: already pending?
    if (std::ranges::any_of(g_og_pending,
                    [&](const og_pending_entry &p) { return p.url == url; }))
        return;

    og_pending_entry entry;
    entry.url            = std::string(url);
    entry.buffer         = buf;
    entry.account_ptr    = account_ptr;
    entry.display_prefix = std::string(display_prefix);
    entry.date           = date;
    entry.silent         = silent;

    if (static_cast<int>(g_og_fetches.size()) < OG_MAX_CONCURRENT)
    {
        XDEBUG("og: launching fetch silent={} url={}", (int)silent, url);
        og_launch_one(std::move(entry));
    }
    else
    {
        XDEBUG("og: queuing fetch (slots full) url={}", url);
        g_og_pending.push_back(std::move(entry));
    }
}

// ── og_launch_one ─────────────────────────────────────────────────────────────
// Extract one pending entry and launch its fetch thread.
// Called by og_start_fetch (when a slot is free) and by og_fetch_cb (drain).

static void og_launch_one(og_pending_entry entry)
{
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) return;

    g_og_fetches.push_back({});
    og_fetch_ctx &ctx = g_og_fetches.back();
    ctx.url            = std::move(entry.url);
    ctx.buffer         = entry.buffer;
    ctx.account_ptr    = entry.account_ptr;
    ctx.display_prefix = std::move(entry.display_prefix);
    ctx.date           = entry.date;
    ctx.silent         = entry.silent;
    ctx.pipe_read_fd   = pipe_fds[0];
    ctx.pipe_write_fd  = pipe_fds[1];

    ctx.hook = weechat_hook_fd(pipe_fds[0], 1, 0, 0,
                               og_fetch_cb, nullptr, nullptr);

    // Capture a raw pointer to the list node — std::list iterators/references
    // are stable across insertions and erasures of other elements, so this
    // pointer remains valid for the entire lifetime of the thread.
    og_fetch_ctx *ctx_ptr = &ctx;
    ctx.worker = std::thread([ctx_ptr]()
    {
        og_fetch_ctx &ctx = *ctx_ptr;
        std::string body;
        {
            CURL *curl = curl_easy_init();
            if (!curl)
            {
                ::write(ctx.pipe_write_fd, "x", 1);
                return;
            }

            // Collect response bytes.
            // Stop as soon as we've buffered past </head> — OG tags are always
            // in the <head> section.  Some sites (e.g. YouTube) put 600+ KB of
            // inline JS before the OG meta tags, so we must read that far.
            // Hard safety cap: 2 MB.  Return CURL_WRITEFUNC_ERROR to abort the
            // transfer early once </head> is seen; we treat CURLE_WRITE_ERROR
            // as a successful partial read in that case.
            struct write_ctx {
                std::string data;
                bool head_done = false;
            };
            static constexpr size_t kWriteHardLimit = 2 * 1024 * 1024; // 2 MB
            write_ctx wctx;
            // Case-insensitive needle search reused from parse_og_from_html.
            auto write_cb = [](char *ptr, size_t size, size_t nmemb, void *ud) -> size_t {
                auto *wc = static_cast<write_ctx *>(ud);
                size_t n = size * nmemb;
                if (wc->head_done)
                    return 0; // abort: already found </head>
                if (wc->data.size() + n > kWriteHardLimit)
                    n = kWriteHardLimit - wc->data.size();
                wc->data.append(ptr, n);
                // Check if </head> or <body appeared in the accumulated data
                // (only scan the new tail to avoid O(n²) behaviour).
                size_t scan_start = wc->data.size() > n + 8
                                    ? wc->data.size() - n - 8 : 0;
                std::string_view tail(wc->data.c_str() + scan_start,
                                      wc->data.size() - scan_start);
                // Reuse established case-insensitive search (std::ranges::search + tolower pred).
                if (ifinds(tail, "</head>") != std::string::npos ||
                    ifinds(tail, "<body") != std::string::npos)
                {
                    wc->head_done = true;
                    return 0; // signal early-abort to curl
                }
                if (wc->data.size() >= kWriteHardLimit)
                    return 0; // hard cap reached, abort
                return size * nmemb;
            };

            curl_easy_setopt(curl, CURLOPT_NOSIGNAL,       1L);
            curl_easy_setopt(curl, CURLOPT_URL,            ctx.url.c_str());
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_MAXREDIRS,      5L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT,        10L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
            curl_easy_setopt(curl, CURLOPT_USERAGENT,      "xepher-og-preview/1.0");
            // Only accept text/html
            struct curl_slist *headers = nullptr;
            headers = curl_slist_append(headers,
                "Accept: text/html,application/xhtml+xml;q=0.9,*/*;q=0.1");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                static_cast<size_t(*)(char*,size_t,size_t,void*)>(write_cb));
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wctx);

            CURLcode res = curl_easy_perform(curl);
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            ctx.http_code = http_code;
            ctx.curl_res  = static_cast<int>(res);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);

            if ((res != CURLE_OK && res != CURLE_WRITE_ERROR) || http_code != 200)
            {
                ::write(ctx.pipe_write_fd, "x", 1);
                return;
            }
            body = std::move(wctx.data);
        }

        ctx.preview = parse_og_from_html(body);
        ctx.success = !ctx.preview.title.empty() || !ctx.preview.description.empty();
        ::write(ctx.pipe_write_fd, "x", 1);
    });
}
