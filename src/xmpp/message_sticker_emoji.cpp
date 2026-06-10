// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "message_sticker_emoji.hh"

#include <algorithm>
#include <optional>
#include <ranges>

#include <fmt/core.h>

#include "message_body.hh"
#include "message_media.hh"
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

struct ResolvedMedia {
    FileMetadata meta;
    std::string url;
};

[[nodiscard]] std::vector<ResolvedMedia> collect_hashed_media(StanzaView msg)
{
    std::vector<ResolvedMedia> out;

    for (const auto &share : collect_sims_shares(msg))
    {
        if (share.url.empty())
            continue;
        ResolvedMedia item;
        item.meta = share.meta;
        item.url = share.url;
        out.push_back(std::move(item));
    }

    for (const auto &sfs : collect_sfs_shares(msg))
    {
        if (sfs.encrypted || !sfs.plain_url)
            continue;
        ResolvedMedia item;
        item.meta = sfs.meta;
        item.url = *sfs.plain_url;
        out.push_back(std::move(item));
    }

    return out;
}

[[nodiscard]] std::optional<ResolvedMedia>
resolve_media_by_hash(std::string_view algo,
                      std::string_view value_b64,
                      const std::vector<ResolvedMedia> &media)
{
    if (algo.empty() || value_b64.empty())
        return std::nullopt;

    const std::string key = file_hash_key(algo, value_b64);
    for (const auto &item : media)
    {
        const bool matched = std::ranges::any_of(
            item.meta.hashes,
            [&](const FileHash &hash) {
                return file_hash_key(hash.algo, hash.value_b64) == key;
            });
        if (matched)
            return item;
    }
    return std::nullopt;
}

void collect_emoji_hashes_from_span(StanzaView span,
                                    const std::vector<ResolvedMedia> &media,
                                    std::vector<CustomEmojiPreview> &previews,
                                    std::unordered_set<std::string> &hash_keys)
{
    for (StanzaView child : span)
    {
        if (!element_is(child, "emoji", k_emoji_markup_ns))
            continue;

        const std::string name = child.attr_string("name");
        for (StanzaView hash_el : child)
        {
            if (!element_is(hash_el, "hash", k_hashes_ns))
                continue;

            const std::string algo = hash_el.attr_string("algo");
            const std::string value_b64 = hash_el.text();
            if (algo.empty() || value_b64.empty())
                continue;

            hash_keys.insert(file_hash_key(algo, value_b64));

            if (auto resolved = resolve_media_by_hash(algo, value_b64, media))
            {
                CustomEmojiPreview preview;
                preview.url = resolved->url;
                preview.mime = resolved->meta.mime;
                preview.width = resolved->meta.width;
                preview.height = resolved->meta.height;
                preview.name = name.empty() ? resolved->meta.name : name;
                previews.push_back(std::move(preview));
            }
        }
    }
}

}  // namespace

std::string file_hash_key(std::string_view algo, std::string_view value_b64)
{
    return fmt::format("{}:{}", algo, value_b64);
}

bool stanza_has_sticker(StanzaView msg)
{
    for (StanzaView child : msg)
    {
        if (element_is(child, "sticker", k_stickers_ns))
            return true;
    }
    return false;
}

std::vector<CustomEmojiPreview> collect_custom_emoji_previews(StanzaView msg)
{
    const StanzaView markup = msg.child("markup", k_markup_ns);
    if (!markup.valid())
        return {};

    const auto media = collect_hashed_media(msg);
    if (media.empty())
        return {};

    std::vector<CustomEmojiPreview> previews;
    std::unordered_set<std::string> hash_keys;

    for (StanzaView child : markup)
    {
        if (child.name() != "span")
            continue;
        collect_emoji_hashes_from_span(child, media, previews, hash_keys);
    }

    return previews;
}

std::unordered_set<std::string> collect_emoji_markup_hash_keys(StanzaView msg)
{
    const StanzaView markup = msg.child("markup", k_markup_ns);
    if (!markup.valid())
        return {};

    const auto media = collect_hashed_media(msg);
    std::vector<CustomEmojiPreview> unused;
    std::unordered_set<std::string> hash_keys;

    for (StanzaView child : markup)
    {
        if (child.name() != "span")
            continue;
        collect_emoji_hashes_from_span(child, media, unused, hash_keys);
    }

    return hash_keys;
}

}  // namespace xmpp