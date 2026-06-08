// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "message_line_tag.hh"

#include <algorithm>
#include <cctype>
#include <fmt/core.h>
#include <ranges>

namespace xmpp {

namespace {

[[nodiscard]] bool cstrcasecmp(std::string_view a, std::string_view b)
{
    return a.size() == b.size()
        && std::ranges::equal(a, b, [](char ca, char cb) {
               return std::tolower(static_cast<unsigned char>(ca))
                   == std::tolower(static_cast<unsigned char>(cb));
           });
}

}  // namespace

bool line_tag_matches_message_id(std::string_view tag, std::string_view target_id)
{
    if (tag.empty() || target_id.empty())
        return false;

    if (tag.starts_with("id_"))
        return cstrcasecmp(tag.substr(3), target_id);
    if (tag.starts_with("stanza_id_"))
        return cstrcasecmp(tag.substr(10), target_id);
    if (tag.starts_with("origin_id_"))
        return cstrcasecmp(tag.substr(10), target_id);
    return false;
}

bool line_tag_matches_nick_sender(std::string_view tag, std::string_view sender_key)
{
    return tag.starts_with("nick_") && cstrcasecmp(tag.substr(5), sender_key);
}

bool line_tag_matches_occupant_sender(std::string_view tag, std::string_view occupant_id)
{
    return cstrcasecmp(tag, occupant_id_tag_needle(occupant_id));
}

std::string occupant_id_tag_needle(std::string_view occupant_id)
{
    return fmt::format("occupant_id_{}", occupant_id);
}

bool line_tags_verify_sender(std::span<const std::string_view> tags,
                             const LineSenderVerify &verify)
{
    if (verify.prefer_occupant_id && verify.occupant_id)
    {
        return std::ranges::any_of(tags, [&](std::string_view tag) {
            return line_tag_matches_occupant_sender(tag, *verify.occupant_id);
        });
    }

    return std::ranges::any_of(tags, [&](std::string_view tag) {
        return line_tag_matches_nick_sender(tag, verify.sender_key);
    });
}

}  // namespace xmpp