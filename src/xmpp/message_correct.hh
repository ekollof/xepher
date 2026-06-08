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

inline constexpr std::string_view k_message_correct_ns = "urn:xmpp:message-correct:0";

struct MessageCorrection {
    std::string target_id;
};

[[nodiscard]] XMPP_TEST_EXPORT bool stanza_has_message_correction(StanzaView msg);

[[nodiscard]] XMPP_TEST_EXPORT std::optional<MessageCorrection>
parse_message_correction(StanzaView msg);

// MUC: resource nick; PM: bare JID.
[[nodiscard]] XMPP_TEST_EXPORT std::string message_correction_sender_key(
    std::string_view from_full,
    std::string_view from_bare,
    bool is_muc_channel);

[[nodiscard]] XMPP_TEST_EXPORT std::string
format_message_correction_text(std::string_view corrected_body);

}  // namespace xmpp