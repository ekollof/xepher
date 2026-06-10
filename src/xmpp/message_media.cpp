// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "message_media.hh"

#include <fmt/core.h>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "weechat/runtime_port.hh"
#include "util.hh"

namespace xmpp {

namespace {

[[nodiscard]] bool element_is(StanzaView view, std::string_view name, std::string_view ns)
{
    if (view.name() != name)
        return false;
    const auto xmlns_attr = view.xmlns();
    return xmlns_attr && *xmlns_attr == ns;
}

[[nodiscard]] std::optional<std::string> first_url_from_sources(StanzaView sources)
{
    if (!sources.valid())
        return std::nullopt;

    for (StanzaView src : sources)
    {
        const auto src_ns = src.xmlns();
        const std::string_view src_name = src.name();

        if (src_name == "reference" && src_ns && *src_ns == k_reference_ns)
        {
            const std::string uri = src.attr_string("uri");
            if (!uri.empty())
                return uri;
        }
        else if (src_name == "url-data")
        {
            const std::string target = src.attr_string("target");
            if (!target.empty())
                return target;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<EncryptedMediaShare> parse_esfs_encrypted_source(
    StanzaView encrypted_elem, const FileMetadata &meta)
{
    const std::string cipher = encrypted_elem.attr_string("cipher");
    if (!is_supported_esfs_cipher(cipher))
        return std::nullopt;

    EncryptedMediaShare share;
    share.meta = meta;
    share.key_b64 = encrypted_elem.child("key").text();
    share.iv_b64 = encrypted_elem.child("iv").text();

    const StanzaView inner_sources = encrypted_elem.child("sources");
    if (auto url = first_url_from_sources(inner_sources))
        share.ciphertext_url = *url;

    if (share.key_b64.empty() || share.iv_b64.empty() || share.ciphertext_url.empty())
        return std::nullopt;
    return share;
}

}  // namespace

FileMetadata parse_file_metadata(StanzaView file_elem)
{
    FileMetadata meta;
    if (!file_elem.valid())
        return meta;

    meta.name = file_elem.child("name").text();
    meta.mime = file_elem.child("media-type").text();
    meta.size_raw = file_elem.child("size").text();

    if (auto w = parse_uint32(file_elem.child("width").text()); w)
        meta.width = *w;
    if (auto h = parse_uint32(file_elem.child("height").text()); h)
        meta.height = *h;

    for (StanzaView child : file_elem)
    {
        if (!element_is(child, "hash", k_hashes_ns))
            continue;
        FileHash hash;
        hash.algo = child.attr_string("algo");
        hash.value_b64 = child.text();
        if (!hash.algo.empty() && !hash.value_b64.empty())
            meta.hashes.push_back(std::move(hash));
    }

    return meta;
}

std::vector<PlainMediaShare> collect_sims_shares(StanzaView msg)
{
    std::vector<PlainMediaShare> shares;

    for (StanzaView child : msg)
    {
        if (!element_is(child, "reference", k_reference_ns))
            continue;
        if (child.attr_string("type") != "data")
            continue;

        const StanzaView media_sharing = child.child("media-sharing", k_sims_ns);
        if (!media_sharing.valid())
            continue;

        const StanzaView file_elem = media_sharing.child("file", k_jingle_file_ns);
        PlainMediaShare share;
        share.meta = parse_file_metadata(file_elem);

        if (auto url = first_url_from_sources(media_sharing.child("sources")))
        {
            share.url = *url;
            shares.push_back(std::move(share));
        }
    }

    return shares;
}

std::vector<SfsShare> collect_sfs_shares(StanzaView msg)
{
    std::vector<SfsShare> shares;

    for (StanzaView fs : msg)
    {
        if (!element_is(fs, "file-sharing", k_sfs_ns))
            continue;

        SfsShare share;
        share.meta = parse_file_metadata(fs.child("file", k_file_metadata_ns));

        const StanzaView sources = fs.child("sources");
        if (!sources.valid())
        {
            shares.push_back(std::move(share));
            continue;
        }

        for (StanzaView src : sources)
        {
            const auto src_ns = src.xmlns();
            const std::string_view src_name = src.name();

            if (src_name == "encrypted" && src_ns && *src_ns == k_esfs_ns)
            {
                if (auto encrypted = parse_esfs_encrypted_source(src, share.meta))
                {
                    share.encrypted = std::move(*encrypted);
                    share.plain_url.reset();
                }
                break;
            }

            if (share.plain_url)
                continue;

            if (src_name == "url-data")
            {
                const std::string target = src.attr_string("target");
                if (!target.empty())
                    share.plain_url = target;
            }
            else if (src_name == "reference" && src_ns && *src_ns == k_reference_ns)
            {
                const std::string uri = src.attr_string("uri");
                if (!uri.empty())
                    share.plain_url = uri;
            }
        }

        shares.push_back(std::move(share));
    }

    return shares;
}

bool is_supported_esfs_cipher(std::string_view cipher)
{
    return cipher == k_aes_gcm_cipher;
}

std::string format_byte_size(std::string_view size_str)
{
    if (size_str.empty())
        return {};
    if (auto sz = parse_int64(size_str); sz)
    {
        const auto n = *sz;
        if (n >= 1024 * 1024)
            return fmt::format("{:.1f} MB", n / 1048576.0);
        if (n >= 1024)
            return fmt::format("{:.1f} KB", n / 1024.0);
        return fmt::format("{} B", n);
    }
    return std::string(size_str);
}

std::string format_file_share_suffix(std::string_view name,
                                     std::string_view mime,
                                     std::string_view size_raw,
                                     std::string_view url)
{
    std::string suffix = std::string("\n") + weechat::RuntimePort::default_runtime().color("cyan") + "[File: ";
    if (!name.empty())
        suffix += name;
    else
        suffix += url;

    if (!mime.empty() || !size_raw.empty())
    {
        suffix += " (";
        if (!mime.empty())
            suffix += mime;
        if (!mime.empty() && !size_raw.empty())
            suffix += ", ";
        if (!size_raw.empty())
            suffix += format_byte_size(size_raw);
        suffix += ")";
    }
    suffix += " " + std::string(url);
    suffix += "]" + std::string(weechat::RuntimePort::default_runtime().color("resetcolor"));
    return suffix;
}

std::string format_encrypted_file_suffix(std::string_view name, std::string_view size_raw)
{
    std::string suffix = std::string("\n") + weechat::RuntimePort::default_runtime().color("cyan") + "[Encrypted file: ";
    suffix += name.empty() ? "(unnamed)" : std::string(name);
    if (!size_raw.empty())
        suffix += " (" + format_byte_size(size_raw) + ")";
    suffix += " — downloading…]" + std::string(weechat::RuntimePort::default_runtime().color("resetcolor"));
    return suffix;
}

std::string format_encrypted_file_saved_suffix(std::string_view name,
                                               std::string_view saved_path)
{
    std::string suffix = std::string("\n") + weechat::RuntimePort::default_runtime().color("cyan") + "[Encrypted file: ";
    suffix += name.empty() ? "(unnamed)" : std::string(name);
    suffix += " — already saved: " + std::string(saved_path) + "]"
        + std::string(weechat::RuntimePort::default_runtime().color("resetcolor"));
    return suffix;
}

}  // namespace xmpp