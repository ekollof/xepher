// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "message_bob.hh"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ranges>
#include <unordered_set>

#include <fmt/core.h>
#include <weechat/weechat-plugin.h>

#include "account.hh"
#include "util.hh"
#include "xmpp/node.hh"
#include "weechat/icat_preview.hh"
#include "xmpp/xep-0231.inl"

namespace xmpp {

namespace {

[[nodiscard]] bool element_is(StanzaView view, std::string_view name, std::string_view ns)
{
    if (view.name() != name)
        return false;
    const auto xmlns_attr = view.xmlns();
    return xmlns_attr && *xmlns_attr == ns;
}

[[nodiscard]] std::string extension_from_mime(std::string_view mime)
{
    if (mime == "image/png")
        return ".png";
    if (mime == "image/jpeg" || mime == "image/jpg")
        return ".jpg";
    if (mime == "image/gif")
        return ".gif";
    if (mime == "image/webp")
        return ".webp";
    return ".bin";
}

[[nodiscard]] std::string bob_cache_dir(weechat::account &acct)
{
    std::shared_ptr<char> eval_path(
        weechat_string_eval_expression(
            fmt::format("${{weechat_data_dir}}/xmpp/bob_cache/{}", acct.name).c_str(),
            nullptr, nullptr, nullptr),
        &free);
    return eval_path ? std::string(eval_path.get()) : std::string{};
}

[[nodiscard]] std::string bob_cache_filename(std::string_view cid)
{
    std::string stem;
    stem.reserve(cid.size());
    for (char c : cid)
    {
        if (std::isalnum(static_cast<unsigned char>(c))
            || c == '-' || c == '_' || c == '.')
            stem += c;
        else
            stem += '_';
    }
    return stem;
}

[[nodiscard]] std::vector<std::uint8_t> base64_decode_bytes(std::string_view encoded)
{
    if (encoded.empty())
        return {};

    const std::size_t max_decoded = (encoded.size() / 4 + 1) * 3 + 3;
    std::vector<char> buf(max_decoded, '\0');
    const std::string encoded_str(encoded);
    const int written = weechat_string_base_decode("64", encoded_str.c_str(), buf.data());
    if (written <= 0)
        return {};

    const auto *ubuf = reinterpret_cast<const std::uint8_t *>(buf.data());
    return {ubuf, ubuf + static_cast<std::size_t>(written)};
}

void collect_img_bob_cids(StanzaView node, std::vector<BobImageRef> &out)
{
    for (StanzaView child : node)
    {
        if (child.name() == "img")
        {
            const std::string src = child.attr_string("src");
            if (auto cid = bob_cid_from_url(src))
            {
                BobImageRef ref;
                ref.cid = std::move(*cid);
                out.push_back(std::move(ref));
            }
            continue;
        }
        collect_img_bob_cids(child, out);
    }
}

void emit_bob_icat(weechat::account &acct,
                   struct t_gui_buffer *buffer,
                   std::string_view local_path,
                   std::string_view mime,
                   std::string_view channel_jid,
                   std::string_view stable_id,
                   bool mam_replay)
{
    if (!buffer || local_path.empty())
        return;

    weechat::icat_preview_request req;
    req.buffer = buffer;
    req.source = std::string(local_path);
    req.mime = std::string(mime);
    req.mam_replay = mam_replay;
    req.channel_jid = std::string(channel_jid);
    req.stable_id = std::string(stable_id);
    invoke_icat_preview(req, acct);
}

void deliver_bob_icat_waiters(weechat::account &acct,
                              std::string_view cid,
                              std::string_view mime,
                              std::string_view local_path,
                              const weechat::account::bob_icat_context &primary)
{
    emit_bob_icat(acct, primary.buffer, local_path,
                  primary.mime.empty() ? mime : primary.mime,
                  primary.channel_jid, primary.stable_id, primary.mam_replay);
    if (!primary.channel_jid.empty() && !primary.stable_id.empty())
        acct.mam_cache_store_image_preview(
            primary.channel_jid, primary.stable_id, std::string(local_path));

    if (auto waiters = acct.bob_deferred_icat.find(std::string(cid));
        waiters != acct.bob_deferred_icat.end())
    {
        for (const auto &ctx : waiters->second)
        {
            emit_bob_icat(acct, ctx.buffer, local_path,
                          ctx.mime.empty() ? mime : ctx.mime,
                          ctx.channel_jid, ctx.stable_id, ctx.mam_replay);
            if (!ctx.channel_jid.empty() && !ctx.stable_id.empty())
                acct.mam_cache_store_image_preview(
                    ctx.channel_jid, ctx.stable_id, std::string(local_path));
        }
        acct.bob_deferred_icat.erase(waiters);
    }

    acct.bob_inflight_cids.erase(std::string(cid));
}

}  // namespace

std::vector<std::uint8_t> bob_decode_base64(std::string_view encoded)
{
    return base64_decode_bytes(encoded);
}

bool is_bob_cid_url(std::string_view url)
{
    return url.starts_with("cid:") && url.ends_with("@bob.xmpp.org");
}

std::optional<std::string> bob_cid_from_url(std::string_view url)
{
    if (!is_bob_cid_url(url))
        return std::nullopt;
    return std::string(url.substr(4));
}

std::vector<BobImageRef> collect_bob_image_refs(StanzaView msg)
{
    std::vector<BobImageRef> refs;
    std::unordered_set<std::string> seen;

    for (StanzaView child : msg)
    {
        if (!element_is(child, "data", k_bob_ns))
            continue;

        const std::string cid = child.attr_string("cid");
        if (cid.empty() || !seen.insert(cid).second)
            continue;

        BobImageRef ref;
        ref.cid = cid;
        ref.mime = child.attr_string("type");
        ref.inline_b64 = child.text();
        refs.push_back(std::move(ref));
    }

    if (const StanzaView html = msg.child("html", k_xhtml_im_ns); html.valid())
    {
        if (const StanzaView body = html.child("body", k_xhtml_ns); body.valid())
        {
            std::vector<BobImageRef> xhtml_refs;
            collect_img_bob_cids(body, xhtml_refs);
            for (auto &ref : xhtml_refs)
            {
                if (!seen.insert(ref.cid).second)
                    continue;
                refs.push_back(std::move(ref));
            }
        }
    }

    return refs;
}

bool message_has_xhtml_bob_images(StanzaView msg)
{
    if (const StanzaView html = msg.child("html", k_xhtml_im_ns); html.valid())
    {
        if (const StanzaView body = html.child("body", k_xhtml_ns); body.valid())
        {
            std::vector<BobImageRef> refs;
            collect_img_bob_cids(body, refs);
            return !refs.empty();
        }
    }
    return false;
}

std::optional<std::string> bob_cache_lookup(weechat::account &acct, std::string_view cid)
{
    const std::string cache_dir = bob_cache_dir(acct);
    if (cache_dir.empty())
        return std::nullopt;

    const std::string stem = bob_cache_filename(cid);
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(cache_dir, ec))
    {
        if (!entry.is_regular_file())
            continue;
        const std::string filename = entry.path().filename().string();
        if (filename.starts_with(stem))
            return entry.path().string();
    }
    return std::nullopt;
}

std::optional<std::string> bob_cache_store_bytes(weechat::account &acct,
                                                 std::string_view cid,
                                                 std::string_view mime,
                                                 std::span<const std::uint8_t> data)
{
    if (cid.empty() || data.empty())
        return std::nullopt;

    const std::string cache_dir = bob_cache_dir(acct);
    if (cache_dir.empty())
        return std::nullopt;

    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);

    const std::string path = fmt::format("{}/{}{}",
                                         cache_dir,
                                         bob_cache_filename(cid),
                                         extension_from_mime(mime));
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return std::nullopt;
    out.write(reinterpret_cast<const char *>(data.data()),
              static_cast<std::streamsize>(data.size()));
    if (!out)
        return std::nullopt;
    return path;
}

void bob_start_fetch(weechat::account &acct,
                     std::string_view to_jid,
                     std::string_view cid,
                     std::string_view mime,
                     struct t_gui_buffer *buffer,
                     std::string_view channel_jid,
                     std::string_view stable_id,
                     bool mam_replay)
{
    if (to_jid.empty() || cid.empty() || !buffer)
        return;

    if (auto cached = bob_cache_lookup(acct, cid))
    {
        emit_bob_icat(acct, buffer, *cached, mime, channel_jid, stable_id, mam_replay);
        if (!channel_jid.empty() && !stable_id.empty())
            acct.mam_cache_store_image_preview(channel_jid, stable_id, *cached);
        return;
    }

    weechat::account::bob_icat_context ctx;
    ctx.cid = std::string(cid);
    ctx.buffer = buffer;
    ctx.channel_jid = std::string(channel_jid);
    ctx.stable_id = std::string(stable_id);
    ctx.mime = std::string(mime);
    ctx.mam_replay = mam_replay;

    if (acct.bob_inflight_cids.contains(ctx.cid))
    {
        acct.bob_deferred_icat[ctx.cid].push_back(std::move(ctx));
        return;
    }

    const std::string iq_id = stanza::uuid(acct.context);
    acct.bob_inflight_cids.insert(ctx.cid);
    acct.bob_fetch_queries[iq_id] = std::move(ctx);

    xmpp_stanza_t *iq = xep0231::request_bob_data(
        acct.context, to_jid, cid, iq_id);
    acct.connection.send(iq);
    xmpp_stanza_release(iq);
}

void bob_complete_fetch_iq(weechat::account &acct,
                           std::string_view iq_id,
                           std::string_view mime,
                           std::span<const std::uint8_t> data)
{
    auto it = acct.bob_fetch_queries.find(std::string(iq_id));
    if (it == acct.bob_fetch_queries.end())
        return;

    weechat::account::bob_icat_context ctx = std::move(it->second);
    acct.bob_fetch_queries.erase(it);

    if (ctx.cid.empty() || data.empty())
    {
        acct.bob_inflight_cids.erase(ctx.cid);
        acct.bob_deferred_icat.erase(ctx.cid);
        return;
    }

    if (auto path = bob_cache_store_bytes(acct, ctx.cid, mime, data))
        deliver_bob_icat_waiters(acct, ctx.cid, mime, *path, ctx);
    else
    {
        acct.bob_inflight_cids.erase(ctx.cid);
        acct.bob_deferred_icat.erase(ctx.cid);
    }
}

}  // namespace xmpp