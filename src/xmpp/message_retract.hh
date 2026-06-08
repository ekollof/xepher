// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "../test_export.hh"
#include "stanza_view.hh"

namespace xmpp {

inline constexpr std::string_view k_message_retract_ns = "urn:xmpp:message-retract:1";
inline constexpr std::string_view k_message_moderate_ns = "urn:xmpp:message-moderate:1";
inline constexpr std::string_view k_occupant_id_ns = "urn:xmpp:occupant-id:0";

struct MessageRetraction {
    std::string target_id;
};

struct ModeratedRetraction {
    std::string target_id;
    std::optional<std::string> reason;
};

[[nodiscard]] XMPP_TEST_EXPORT std::optional<MessageRetraction>
parse_message_retraction(StanzaView msg);

[[nodiscard]] XMPP_TEST_EXPORT std::optional<ModeratedRetraction>
parse_moderated_retraction(StanzaView msg);

[[nodiscard]] XMPP_TEST_EXPORT bool should_accept_moderation_from_sender(
    std::string_view from_bare,
    std::string_view channel_id);

[[nodiscard]] XMPP_TEST_EXPORT std::optional<std::string>
parse_retraction_occupant_id(StanzaView msg);

[[nodiscard]] XMPP_TEST_EXPORT std::string retraction_sender_key(
    std::string_view from_full,
    std::string_view from_bare,
    bool is_muc_channel);

[[nodiscard]] XMPP_TEST_EXPORT std::string format_retraction_tombstone();
[[nodiscard]] XMPP_TEST_EXPORT std::string
format_moderation_tombstone(std::optional<std::string_view> reason);

}  // namespace xmpp