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
#include <openssl/evp.h>
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

    const std::size_t max_decoded = encoded.size() * 3 / 4 + 4;
    std::vector<std::uint8_t> buf(max_decoded, 0);
    const int written = EVP_DecodeBlock(
        buf.data(),
        reinterpret_cast<const unsigned char *>(encoded.data()),
        static_cast<int>(encoded.size()));
    if (written <= 0)
        return {};

    std::size_t out_len = static_cast<std::size_t>(written);
    if (!encoded.empty() && encoded.back() == '=')
    {
        --out_len;
        if (encoded.size() >= 2 && encoded[encoded.size() - 2] == '=')
            --out_len;
    }
    buf.resize(out_len);
    return buf;
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

std::string bob_encode_base64(std::span<const std::uint8_t> data)
{
    if (data.empty())
        return {};

    const int encoded_size = 4 * static_cast<int>((data.size() + 2) / 3);
    std::string encoded(static_cast<std::size_t>(encoded_size), '\0');
    const int written = EVP_EncodeBlock(
        reinterpret_cast<unsigned char *>(encoded.data()),
        data.data(),
        static_cast<int>(data.size()));
    if (written <= 0)
        return {};
    encoded.resize(static_cast<std::size_t>(written));
    return encoded;
}

std::string bob_make_cid(std::span<const std::uint8_t> data)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    EVP_Digest(data.data(), data.size(), digest, &digest_len, EVP_sha1(), nullptr);

    std::string hex;
    hex.reserve(digest_len * 2);
    std::ranges::for_each(std::span<const unsigned char>(digest, digest_len),
                          [&](unsigned char b) {
                              hex += fmt::format("{:02x}", b);
                          });
    return fmt::format("sha1+{}@bob.xmpp.org", hex);
}

bool bob_payload_size_ok(std::size_t nbytes)
{
    return nbytes > 0 && nbytes <= k_bob_max_payload_bytes;
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

void bob_host_store(weechat::account &acct,
                    std::string_view cid,
                    std::string_view mime,
                    std::span<const std::uint8_t> data)
{
    if (cid.empty() || data.empty())
        return;

    acct.bob_hosted[std::string(cid)] = BobHostedPayload{
        std::string(mime),
        {data.begin(), data.end()},
    };
    (void)bob_cache_store_bytes(acct, cid, mime, data);
}

std::optional<BobHostedPayload> bob_host_lookup(weechat::account &acct,
                                                std::string_view cid)
{
    if (auto it = acct.bob_hosted.find(std::string(cid)); it != acct.bob_hosted.end())
        return it->second;
    return std::nullopt;
}

stanza::message build_bob_image_message(std::string_view to,
                                        std::string_view msg_type,
                                        std::string_view msg_id,
                                        std::string_view cid,
                                        std::string_view mime,
                                        std::span<const std::uint8_t> data,
                                        std::string_view alt)
{
    const std::string body_fallback = alt.empty() ? "[image]" : std::string(alt);
    const std::string cid_url = fmt::format("cid:{}", cid);

    stanza::message msg;
    msg.type(msg_type)
       .to(std::string(to))
       .id(std::string(msg_id))
       .origin_id(std::string(msg_id))
       .body(body_fallback);

    if (data.size() <= k_bob_inline_payload_bytes)
    {
        const std::string b64 = bob_encode_base64(data);
        if (!b64.empty())
            msg.child(xep0231::data_elem(cid, mime, b64));
    }

    xep0231::xhtml_img img(cid_url, alt);
    xep0231::xhtml_p paragraph;
    paragraph.img(img);
    xep0231::xhtml_body xbody;
    xbody.paragraph(paragraph);
    xep0231::xhtml_im xhtml;
    xhtml.body(xbody);
    msg.child(xhtml);

    msg.chatstate("active");
    if (msg_type != "groupchat")
    {
        msg.receipt_request().chat_marker_markable();
    }
    msg.store();

    return msg;
}

bool is_bob_iq_get(StanzaView iq)
{
    if (iq.type() != "get")
        return false;

    const StanzaView data = iq.child("data", k_bob_ns);
    if (!data.valid())
        return false;

    return data.attr_string("cid").size() > 0 && data.text().empty();
}

std::optional<stanza::iq> handle_bob_iq_get(StanzaView request,
                                            std::string_view local_jid,
                                            const BobHostedPayload *hosted)
{
    if (!is_bob_iq_get(request))
        return std::nullopt;

    const std::string cid = request.child("data", k_bob_ns).attr_string("cid");
    const std::string from = request.attr_string("from");
    const std::string id = request.attr_string("id");

    if (!hosted || hosted->data.empty())
    {
        struct item_not_found : virtual public stanza::spec {
            item_not_found() : spec("item-not-found")
            {
                attr("xmlns", "urn:ietf:params:xml:ns:xmpp-stanzas");
            }
        };
        struct error_elem : virtual public stanza::spec {
            explicit error_elem(stanza::spec &child_spec) : spec("error")
            {
                attr("type", "cancel");
                child(child_spec);
            }
        };
        item_not_found inf;
        error_elem err(inf);
        stanza::iq reply;
        reply.type("error")
            .id(id)
            .to(from)
            .from(std::string(local_jid));
        reply.child(err);
        return reply;
    }

    const std::string b64 = bob_encode_base64(hosted->data);
    if (b64.empty())
        return std::nullopt;

    xep0231::data_elem payload(
        cid,
        hosted->mime,
        b64,
        xep0231::k_default_max_age);
    stanza::iq reply;
    reply.type("result")
        .id(id)
        .to(from)
        .from(std::string(local_jid));
    reply.child(payload);
    return reply;
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