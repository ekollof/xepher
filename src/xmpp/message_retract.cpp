// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "message_retract.hh"

#include <fmt/core.h>
#include <weechat/weechat-plugin.h>

#include "../plugin.hh"
#include "message_correct.hh"
#include "../xmpp/node.hh"

namespace xmpp {

std::optional<MessageRetraction> parse_message_retraction(StanzaView msg)
{
    const StanzaView retract = msg.child("retract", k_message_retract_ns);
    if (!retract.valid())
        return std::nullopt;

    if (retract.child("moderated", k_message_moderate_ns).valid())
        return std::nullopt;

    const std::string target_id = retract.attr_string("id");
    if (target_id.empty())
        return std::nullopt;

    MessageRetraction retraction;
    retraction.target_id = target_id;
    return retraction;
}

std::optional<ModeratedRetraction> parse_moderated_retraction(StanzaView msg)
{
    const StanzaView retract = msg.child("retract", k_message_retract_ns);
    if (!retract.valid())
        return std::nullopt;

    const StanzaView moderated = retract.child("moderated", k_message_moderate_ns);
    if (!moderated.valid())
        return std::nullopt;

    const std::string target_id = retract.attr_string("id");
    if (target_id.empty())
        return std::nullopt;

    ModeratedRetraction moderation;
    moderation.target_id = target_id;

    const std::string reason = moderated.child("reason").text();
    if (!reason.empty())
        moderation.reason = reason;

    return moderation;
}

bool should_accept_moderation_from_sender(
    std::string_view from_bare,
    std::string_view channel_id)
{
    return !from_bare.empty() && from_bare == channel_id;
}

std::optional<std::string> parse_retraction_occupant_id(StanzaView msg)
{
    const StanzaView occupant = msg.child("occupant-id", k_occupant_id_ns);
    if (!occupant.valid())
        return std::nullopt;

    const std::string id = occupant.attr_string("id");
    if (id.empty())
        return std::nullopt;
    return id;
}

std::string retraction_sender_key(
    std::string_view from_full,
    std::string_view from_bare,
    bool is_muc_channel)
{
    return message_correction_sender_key(from_full, from_bare, is_muc_channel);
}

std::string format_retraction_tombstone()
{
    return fmt::format("{}[Message deleted]{}",
                       weechat_color("darkgray"),
                       weechat_color("resetcolor"));
}

std::string format_moderation_tombstone(std::optional<std::string_view> reason)
{
    if (reason && !reason->empty())
    {
        return fmt::format("{}[Message moderated: {}]{}",
                           weechat_color("darkgray"),
                           *reason,
                           weechat_color("resetcolor"));
    }
    return fmt::format("{}[Message moderated by room moderator]{}",
                       weechat_color("darkgray"),
                       weechat_color("resetcolor"));
}

}  // namespace xmpp