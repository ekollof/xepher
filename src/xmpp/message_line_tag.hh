// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "../test_export.hh"

namespace xmpp {

// Buffer lines are tagged id_<stable_id>, stanza_id_<sid>, or origin_id_<oid>.
[[nodiscard]] XMPP_TEST_EXPORT bool line_tag_matches_message_id(
    std::string_view tag, std::string_view target_id);

[[nodiscard]] XMPP_TEST_EXPORT bool line_tag_matches_nick_sender(
    std::string_view tag, std::string_view sender_key);

[[nodiscard]] XMPP_TEST_EXPORT bool line_tag_matches_occupant_sender(
    std::string_view tag, std::string_view occupant_id);

[[nodiscard]] XMPP_TEST_EXPORT std::string occupant_id_tag_needle(std::string_view occupant_id);

struct LineSenderVerify {
    std::string sender_key;
    std::optional<std::string> occupant_id;
    bool prefer_occupant_id = false;
};

// Returns true when any tag matches the sender policy.
[[nodiscard]] XMPP_TEST_EXPORT bool line_tags_verify_sender(
    std::span<const std::string_view> tags,
    const LineSenderVerify &verify);

}  // namespace xmpp