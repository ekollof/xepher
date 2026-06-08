// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "chat_state.hh"

namespace xmpp {

namespace {

[[nodiscard]] bool has_state_element(StanzaView msg, std::string_view name)
{
    return msg.child(name, k_chatstates_ns).valid();
}

[[nodiscard]] std::optional<ChatStateKind> detect_chat_state_kind(StanzaView msg)
{
    if (has_state_element(msg, "composing"))
        return ChatStateKind::composing;
    if (has_state_element(msg, "paused"))
        return ChatStateKind::paused;
    if (has_state_element(msg, "active"))
        return ChatStateKind::active;
    if (has_state_element(msg, "inactive"))
        return ChatStateKind::inactive;
    if (has_state_element(msg, "gone"))
        return ChatStateKind::gone;
    return std::nullopt;
}

}  // namespace

bool stanza_has_chat_state(StanzaView msg)
{
    return detect_chat_state_kind(msg).has_value();
}

std::optional<IncomingChatState> parse_incoming_chat_state(StanzaView msg)
{
    const auto kind = detect_chat_state_kind(msg);
    if (!kind)
        return std::nullopt;

    const auto from = msg.from();
    if (!from || from->empty())
        return std::nullopt;

    return IncomingChatState{
        .from = std::string(*from),
        .state = *kind,
    };
}

TypingAction typing_action_for_state(ChatStateKind state)
{
    if (state == ChatStateKind::composing)
        return TypingAction::show;
    return TypingAction::clear;
}

bool should_clear_typing_on_message(bool is_delayed, bool from_self)
{
    return !is_delayed && !from_self;
}

}  // namespace xmpp