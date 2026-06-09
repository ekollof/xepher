// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "iq_bookmarks.hh"

#include <algorithm>
#include <cctype>
#include <fmt/core.h>
#include <ranges>

namespace xmpp {

namespace {

[[nodiscard]] bool attr_iequals(std::string_view a, std::string_view b)
{
    return a.size() == b.size()
        && std::ranges::equal(a, b, [](unsigned char x, unsigned char y) {
            return std::tolower(x) == std::tolower(y);
        });
}

}  // namespace

bool is_bookmark_autojoin_true(const std::string_view autojoin_attr)
{
    return attr_iequals(autojoin_attr, "true") || autojoin_attr == "1";
}

bool is_biboumi_gateway_room(const std::string_view jid)
{
    return jid.contains('%') || jid.contains("biboumi") || jid.contains("@irc.");
}

std::string bookmark_enter_command(const std::string_view jid, const std::string_view nick)
{
    // --no-switch: bookmark autojoin must not steal focus from the account buffer.
    if (nick.empty())
        return fmt::format("/enter {} --no-switch", jid);
    return fmt::format("/enter {}/{} --no-switch", jid, nick);
}

}  // namespace xmpp