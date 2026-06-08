// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "message_spoiler.hh"

#include <weechat/weechat-plugin.h>

#include "color.hh"
#include "plugin.hh"

namespace xmpp {

std::optional<std::string> parse_spoiler_hint(StanzaView msg)
{
    const StanzaView spoiler = msg.child("spoiler", k_spoiler_ns);
    if (!spoiler.valid())
        return std::nullopt;

    const std::string hint = spoiler.text();
    if (hint.empty())
        return std::string{};
    return hint;
}

std::string format_spoiler_display_prefix(std::optional<std::string_view> hint)
{
    std::string prefix = std::string(weechat::xmpp_color("yellow")) + "[Spoiler";
    if (hint && !hint->empty())
        prefix += ": " + std::string(*hint);
    prefix += "] " + std::string(weechat::xmpp_color("resetcolor"));
    return prefix;
}

}  // namespace xmpp