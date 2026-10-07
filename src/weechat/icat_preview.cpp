// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "weechat/icat_preview.hh"

#include <filesystem>
#include <charconv>
#include <fstream>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <ranges>
#include <thread>
#include <unistd.h>
#include <vector>

#include <curl/curl.h>
#include <fmt/core.h>
#include <weechat/weechat-plugin.h>

#include "account.hh"
#include "config.hh"
#include "plugin.hh"
#include "util.hh"
#include "weechat/ui_port.hh"
#include "weechat/buffer_port.hh"

namespace weechat {

namespace {

// Joinable background image-cache downloads — drained on plugin unload.
std::mutex g_icat_bg_mutex;
std::list<std::thread> g_icat_bg_workers;
struct t_hook *g_icat_mouse_hook = nullptr;

int open_image_done(const void *, void *, const char *, int return_code,
                    const char *, const char *)
{
    if (return_code == WEECHAT_HOOK_PROCESS_ERROR || return_code > 0)
        UiPort::for_buffer(nullptr)->printf_error(
            fmt::format("xmpp: xdg-open failed ({})", return_code));
    return WEECHAT_RC_OK;
}

int open_image_cb(const void *, void *, const char *, struct t_hashtable *event)
{
    if (!event || g_plugin_unloading)
        return WEECHAT_RC_OK;
    const auto *full_name = static_cast<const char *>(
        weechat_hashtable_get(event, "_buffer_full_name"));
    const auto *tags = static_cast<const char *>(
        weechat_hashtable_get(event, "_chat_line_tags"));
    if (!full_name || !tags || !std::string_view(full_name).starts_with("xmpp."))
        return WEECHAT_RC_OK;
    auto *buffer = BufferPort::default_port_ref().search("==", full_name);
    if (!buffer)
        return WEECHAT_RC_OK;

    // The shared live icat script uses this historical tag for all plugins.
    constexpr std::string_view prefix = "weeslack_file_";
    for (auto part : std::string_view(tags) | std::views::split(','))
    {
        const std::string_view tag(part.begin(), part.end());
        if (!tag.starts_with(prefix))
            continue;
        const auto hex = tag.substr(prefix.size());
        if (hex.empty() || hex.size() % 2 || hex.size() > 8192)
            return WEECHAT_RC_OK;
        std::string path;
        path.reserve(hex.size() / 2);
        for (auto pair : hex | std::views::chunk(2))
        {
            unsigned int byte = 0;
            const auto [end, error] = std::from_chars(pair.data(), pair.data() + 2, byte, 16);
            if (error != std::errc{} || end != pair.data() + 2 || byte == 0)
                return WEECHAT_RC_OK;
            path.push_back(static_cast<char>(byte));
        }
        std::error_code error;
        auto ui = UiPort::for_buffer(buffer);
        if (!path.starts_with('/') || !std::filesystem::is_regular_file(path, error))
        {
            ui->printf_error("xmpp: image file is unavailable");
            return WEECHAT_RC_OK;
        }
        const auto free_options = [](t_hashtable *options) { weechat_hashtable_free(options); };
        using options_ptr = std::unique_ptr<t_hashtable, decltype(free_options)>;
        options_ptr options(weechat_hashtable_new(8, WEECHAT_HASHTABLE_STRING,
                            WEECHAT_HASHTABLE_STRING, nullptr, nullptr),
                            free_options);
        if (!options)
            return WEECHAT_RC_OK;
        // Explicit argv keeps spaces and shell characters literal.
        weechat_hashtable_set(options.get(), "arg1", path.c_str());
        if (!weechat_hook_process_hashtable("xdg-open", options.get(), 30000,
                                            &open_image_done, nullptr, nullptr))
            ui->printf_error("xmpp: could not start xdg-open");
        return WEECHAT_RC_OK;
    }
    return WEECHAT_RC_OK;
}

void join_icat_background_workers()
{
    std::list<std::thread> pending;
    {
        std::lock_guard lock(g_icat_bg_mutex);
        pending.swap(g_icat_bg_workers);
    }
    for (auto &t : pending)
    {
        if (t.joinable())
            t.join();
    }
}

[[nodiscard]] bool path_is_image(std::string_view path)
{
    const auto dot = path.rfind('.');
    if (dot == std::string_view::npos)
        return false;
    const std::string_view ext = path.substr(dot);
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png"
        || ext == ".gif" || ext == ".webp";
}

[[nodiscard]] bool should_icat(const icat_preview_request &req)
{
    if (!req.mime.empty())
        return is_image_mime_type(req.mime);
    return path_is_image(req.source);
}

[[nodiscard]] std::string image_cache_dir(account &acct)
{
    std::shared_ptr<char> eval_path(
        weechat_string_eval_expression(
            fmt::format("${{weechat_data_dir}}/xmpp/image_cache/{}", acct.name).c_str(),
            nullptr, nullptr, nullptr),
        &free);
    return eval_path ? std::string(eval_path.get()) : std::string{};
}

[[nodiscard]] std::string extension_from_url(std::string_view url)
{
    auto q = url.find('?');
    const std::string_view base = (q != std::string_view::npos) ? url.substr(0, q) : url;
    const auto dot = base.rfind('.');
    if (dot == std::string_view::npos || dot + 1 >= base.size())
        return ".bin";
    const std::string_view ext = base.substr(dot);
    if (ext == ".jpg" || ext == ".jpeg" || ext == ".png"
        || ext == ".gif" || ext == ".webp")
        return std::string(ext);
    return ".bin";
}

size_t curl_write_vec(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto *buf = static_cast<std::vector<unsigned char> *>(userdata);
    buf->insert(buf->end(), reinterpret_cast<unsigned char *>(ptr),
                reinterpret_cast<unsigned char *>(ptr) + size * nmemb);
    return size * nmemb;
}

[[nodiscard]] bool icat_command_registered()
{
    struct t_infolist *infolist = weechat_infolist_get("hook", nullptr, "command,icat");
    if (!infolist)
        return false;
    const bool found = weechat_infolist_next(infolist) != 0;
    weechat_infolist_free(infolist);
    return found;
}

// Returns true when /icat is registered. Caches a positive result; re-probes until
// first success so a mid-session /script load icat.py is picked up.
[[nodiscard]] bool icat_command_available(account &acct)
{
    static bool confirmed = false;
    static bool warned = false;

    if (confirmed)
        return true;
    if (!icat_command_registered())
    {
        if (!warned && acct.buffer)
        {
            warned = true;
            if (auto ui = UiPort::for_buffer(acct.buffer))
            {
                ui->printf_error(fmt::format(
                    "{}: xmpp.look.icat is enabled but /icat is not available — "
                    "load the weechat-icat script (e.g. /script load icat.py)",
                    WEECHAT_XMPP_PLUGIN_NAME));
            }
        }
        return false;
    }

    confirmed = true;
    return true;
}

void run_icat_command(account &acct,
                      struct t_gui_buffer *buffer,
                      std::string_view local_path,
                      size_t width,
                      size_t height)
{
    if (!buffer || local_path.empty())
        return;
    if (!icat_command_available(acct))
        return;

    size_t w = width;
    size_t h = height;
    if (w == 0 || h == 0)
        std::tie(w, h) = read_image_dimensions(local_path.data());

    const std::string dim_args = icat_dimension_args(w, h);
    const std::string icat_cmd = fmt::format("/icat -print_immediately -quiet{} {}",
                                             dim_args, local_path);
    weechat_command(buffer, icat_cmd.c_str());
}

void cache_image_path_background(account *acct,
                                 std::string channel_jid,
                                 std::string stable_id,
                                 std::string url)
{
    if (!acct || url.empty() || weechat::g_plugin_unloading)
        return;

    std::thread worker([acct, channel_jid = std::move(channel_jid),
                        stable_id = std::move(stable_id), url = std::move(url)]() {
        if (weechat::g_plugin_unloading)
            return;
        if (auto path = download_image_to_cache_sync(*acct, url))
        {
            if (weechat::g_plugin_unloading)
                return;
            if (!channel_jid.empty() && !stable_id.empty())
                acct->mam_cache_store_image_preview(channel_jid, stable_id, *path);
        }
    });

    std::lock_guard lock(g_icat_bg_mutex);
    g_icat_bg_workers.push_back(std::move(worker));
}

[[nodiscard]] std::string extension_from_path(std::string_view path)
{
    const auto dot = path.rfind('.');
    if (dot == std::string_view::npos || dot + 1 >= path.size())
        return ".bin";
    const std::string_view ext = path.substr(dot);
    if (ext == ".jpg" || ext == ".jpeg" || ext == ".png"
        || ext == ".gif" || ext == ".webp")
        return std::string(ext);
    return ".bin";
}

[[nodiscard]] bool is_upload_temp_snapshot(std::string_view path)
{
    return path.rfind("/tmp/xepher-upload-", 0) == 0;
}

[[nodiscard]] std::optional<std::string>
stage_upload_snapshot_for_icat(account &acct, std::string_view source_path)
{
    if (source_path.empty() || !std::filesystem::is_regular_file(source_path))
        return std::nullopt;

    if (!is_upload_temp_snapshot(source_path))
        return std::string(source_path);

    const std::string cache_dir = image_cache_dir(acct);
    if (cache_dir.empty())
        return std::nullopt;

    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);

    const std::string stem =
        fmt::format("upload_{:x}", std::hash<std::string>{}(std::string(source_path)));
    const std::string ext = extension_from_path(source_path);
    std::string dest = fmt::format("{}/{}{}", cache_dir, stem, ext);
    for (int suffix = 1; std::filesystem::exists(dest); ++suffix)
        dest = fmt::format("{}/{}_{}{}", cache_dir, stem, suffix, ext);

    std::filesystem::copy_file(source_path, dest,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec)
        return std::nullopt;
    return dest;
}

[[nodiscard]] std::optional<std::string>
resolve_local_icat_path(const icat_preview_request &req, account &acct)
{
    if (req.source.empty())
        return std::nullopt;

    const std::string_view src = req.source;
    if (!src.starts_with("http://") && !src.starts_with("https://"))
    {
        if (std::filesystem::is_regular_file(src))
            return std::string(src);
        return std::nullopt;
    }

    if (!req.channel_jid.empty() && !req.stable_id.empty())
    {
        if (auto cached = acct.mam_cache_lookup_image_preview(req.channel_jid, req.stable_id))
            return *cached;
        if (auto esfs = acct.mam_cache_lookup_esfs_download(req.channel_jid, req.stable_id))
            return *esfs;
    }

    if (!req.mam_replay)
        return std::nullopt;

    if (auto path = download_image_to_cache_sync(acct, src))
    {
        if (!req.channel_jid.empty() && !req.stable_id.empty())
            acct.mam_cache_store_image_preview(req.channel_jid, req.stable_id, *path);
        return *path;
    }

    return std::nullopt;
}

}  // namespace

void init_icat_mouse()
{
    g_icat_mouse_hook = weechat_hook_hsignal("xmpp_open_image", &open_image_cb,
                                            nullptr, nullptr);
    auto *keys = weechat_hashtable_new(8, WEECHAT_HASHTABLE_STRING,
                                      WEECHAT_HASHTABLE_STRING, nullptr, nullptr);
    if (!keys)
        return;
    weechat_hashtable_set(keys, "__quiet", "1");
    weechat_hashtable_set(keys, "@chat(xmpp.*):button1", "hsignal:xmpp_open_image");
    weechat_key_bind("mouse", keys);
    weechat_hashtable_free(keys);
}

void shutdown_icat_mouse()
{
    if (g_icat_mouse_hook)
    {
        weechat_unhook(g_icat_mouse_hook);
        g_icat_mouse_hook = nullptr;
    }
    weechat_key_unbind("mouse", "@chat(xmpp.*):button1");
}

void shutdown_icat_background_workers()
{
    join_icat_background_workers();
}

void emit_upload_local_icat_preview(const icat_preview_request &req,
                                    account &acct,
                                    const std::string_view local_path)
{
    if (!req.buffer || local_path.empty())
        return;

    const auto staged = stage_upload_snapshot_for_icat(acct, local_path);
    if (!staged)
        return;

    icat_preview_request staged_req = req;
    staged_req.source = *staged;
    invoke_icat_preview(staged_req, acct);

    if (is_upload_temp_snapshot(local_path))
        ::unlink(std::string(local_path).c_str());
}

std::expected<std::string, std::string>
download_image_to_cache_sync(account &acct, const std::string_view url)
{
    if (!url.starts_with("http://") && !url.starts_with("https://"))
        return std::unexpected("not an http(s) url");

    const std::string cache_dir = image_cache_dir(acct);
    if (cache_dir.empty())
        return std::unexpected("image cache dir unavailable");

    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);

    std::vector<unsigned char> body;
    CURL *curl = curl_easy_init();
    if (!curl)
        return std::unexpected("curl_easy_init failed");

    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_URL, std::string(url).c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_vec);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "xepher-icat-cache/1.0");

    const CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK)
        return std::unexpected(fmt::format("curl: {}", curl_easy_strerror(res)));
    if (http_code != 200 && http_code != 206)
        return std::unexpected(fmt::format("http {}", http_code));
    if (body.empty())
        return std::unexpected("empty response");

    const std::string stem =
        fmt::format("{:x}", std::hash<std::string>{}(std::string(url)));
    const std::string ext = extension_from_url(url);
    std::string out_path = fmt::format("{}/{}{}", cache_dir, stem, ext);
    for (int suffix = 1; std::filesystem::exists(out_path); ++suffix)
        out_path = fmt::format("{}/{}_{}{}", cache_dir, stem, suffix, ext);

    std::ofstream out(out_path, std::ios::binary);
    if (!out)
        return std::unexpected("failed to open cache file");
    out.write(reinterpret_cast<const char *>(body.data()),
              static_cast<std::streamsize>(body.size()));
    if (!out)
        return std::unexpected("failed to write cache file");

    return out_path;
}

void invoke_icat_preview(const icat_preview_request &req, account &acct)
{
    if (!req.buffer || req.source.empty())
        return;
    if (!config::instance
        || !weechat_config_boolean(config::instance->look.icat))
        return;
    if (!should_icat(req))
        return;

    if (req.mam_replay)
    {
        if (auto local = resolve_local_icat_path(req, acct))
            run_icat_command(acct, req.buffer, *local, req.width, req.height);
        return;
    }

    const std::string_view src = req.source;
    if (src.starts_with("http://") || src.starts_with("https://"))
    {
        run_icat_command(acct, req.buffer, src, req.width, req.height);
        if (!req.channel_jid.empty() && !req.stable_id.empty())
            cache_image_path_background(&acct, req.channel_jid, req.stable_id, std::string(src));
        return;
    }

    if (std::filesystem::is_regular_file(src))
    {
        run_icat_command(acct, req.buffer, src, req.width, req.height);
        if (!req.channel_jid.empty() && !req.stable_id.empty())
            acct.mam_cache_store_image_preview(req.channel_jid, req.stable_id, std::string(src));
    }
}

}  // namespace weechat
