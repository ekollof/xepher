// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "message_reactions.hh"

#include <algorithm>
#include <fmt/core.h>
#include <fmt/ranges.h>
#include <ranges>
#include <vector>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "weechat/runtime_port.hh"
#include "xmpp/message_fallback.hh"

namespace xmpp {

std::optional<MessageReactions> parse_message_reactions(StanzaView msg)
{
    const StanzaView reactions = msg.child("reactions", k_reactions_ns);
    if (!reactions.valid())
        return std::nullopt;

    const std::string target_id = reactions.attr_string("id");
    if (target_id.empty())
        return std::nullopt;

    MessageReactions parsed;
    parsed.target_id = target_id;
    std::vector<std::string> emojis;
    std::ranges::copy(
        reactions
            | std::views::filter([](const StanzaView &child) { return child.name() == "reaction"; })
            | std::views::transform([](const StanzaView &child) { return child.text(); })
            | std::views::filter([](const std::string &emoji) { return !emoji.empty(); }),
        std::back_inserter(emojis));
    parsed.emojis = fmt::format("{}", fmt::join(emojis, " "));
    return parsed;
}

std::string reaction_suffix_marker()
{
    return fmt::format(" {}[", weechat::RuntimePort::default_runtime().color("blue"));
}

std::string format_message_with_reactions(std::string_view original_message,
                                          std::string_view emojis)
{
    std::string base(original_message);
    const std::string marker = reaction_suffix_marker();
    if (const auto rxn_pos = base.find(marker); rxn_pos != std::string::npos)
        base.resize(rxn_pos);

    if (emojis.empty())
        return base;

    return fmt::format("{} {}[{}]{}",
                       base,
                       weechat::RuntimePort::default_runtime().color("blue"),
                       emojis,
                       weechat::RuntimePort::default_runtime().color("resetcolor"));
}

}  // namespace xmpp