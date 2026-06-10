// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "test_export.hh"
#include "stanza_view.hh"

namespace xmpp {

inline constexpr std::string_view k_chatstates_ns = "http://jabber.org/protocol/chatstates";

enum class ChatStateKind {
    composing,
    paused,
    active,
    inactive,
    gone,
};

enum class TypingAction {
    show,
    clear,
};

struct IncomingChatState {
    std::string from;
    ChatStateKind state;
};

[[nodiscard]] XMPP_TEST_EXPORT bool stanza_has_chat_state(StanzaView msg);

// Requires a from attribute and a recognized chat-state child.
[[nodiscard]] XMPP_TEST_EXPORT std::optional<IncomingChatState>
parse_incoming_chat_state(StanzaView msg);

[[nodiscard]] XMPP_TEST_EXPORT TypingAction typing_action_for_state(ChatStateKind state);

// XEP-0085: a live message from another user implicitly clears composing.
[[nodiscard]] XMPP_TEST_EXPORT bool should_clear_typing_on_message(bool is_delayed,
                                                                    bool from_self);

}  // namespace xmpp