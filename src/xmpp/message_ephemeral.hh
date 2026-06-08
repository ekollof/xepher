// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../test_export.hh"
#include "stanza_view.hh"

namespace xmpp {

inline constexpr std::string_view k_ephemeral_ns = "urn:xmpp:ephemeral:0";

[[nodiscard]] XMPP_TEST_EXPORT std::optional<std::int64_t>
parse_ephemeral_timer(StanzaView msg);

[[nodiscard]] XMPP_TEST_EXPORT std::string
format_ephemeral_display_prefix(std::int64_t timer_secs);

[[nodiscard]] XMPP_TEST_EXPORT bool should_schedule_ephemeral_tombstone(
    std::int64_t timer_secs, std::string_view stable_id);

}  // namespace xmpp