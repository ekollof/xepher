// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "test_export.hh"
#include "xmpp/stanza_view.hh"

namespace xmpp {

struct MessageReactions {
    std::string target_id;
    // Space-separated emoji set; empty means remove all reactions (XEP-0444 §3.2).
    std::string emojis;
};

[[nodiscard]] XMPP_TEST_EXPORT std::optional<MessageReactions>
parse_message_reactions(StanzaView msg);

// Marker prefix of our appended reaction suffix, for stripping a prior set.
[[nodiscard]] XMPP_TEST_EXPORT std::string reaction_suffix_marker();

// Replace any prior reaction suffix on original_message with emojis (or strip when empty).
[[nodiscard]] XMPP_TEST_EXPORT std::string format_message_with_reactions(
    std::string_view original_message, std::string_view emojis);

}  // namespace xmpp