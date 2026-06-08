// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <ctime>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "test_export.hh"
#include "stanza_view.hh"

namespace xmpp {

inline constexpr std::string_view k_ping_ns = "urn:xmpp:ping";
inline constexpr std::string_view k_ietf_stanza_ns =
    "urn:ietf:params:xml:ns:xmpp-stanzas";

[[nodiscard]] XMPP_TEST_EXPORT bool is_ping_get_iq(StanzaView iq);

[[nodiscard]] XMPP_TEST_EXPORT long compute_ping_rtt_ms(std::time_t start, std::time_t now);

struct MucPingFrom {
    std::string room_jid;
    std::string resource;
};

[[nodiscard]] XMPP_TEST_EXPORT std::optional<MucPingFrom>
parse_muc_ping_from(std::string_view from_full);

[[nodiscard]] XMPP_TEST_EXPORT bool is_muc_self_ping(
    const MucPingFrom &from,
    std::string_view account_nick,
    const std::function<bool(std::string_view room_jid)> &has_room_channel);

enum class MucSelfPingErrorOutcome {
    still_joined,
    ambiguous,
    not_joined,
};

[[nodiscard]] XMPP_TEST_EXPORT MucSelfPingErrorOutcome
classify_muc_self_ping_error(StanzaView error_elem);

}  // namespace xmpp