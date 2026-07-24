// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "test_export.hh"
#include "stanza_view.hh"

namespace xmpp {

[[nodiscard]] XMPP_TEST_EXPORT std::string build_caps_verification_string(
    StanzaView query,
    const std::vector<std::string> &features);

[[nodiscard]] XMPP_TEST_EXPORT std::string caps_sha1_base64(std::string_view verification_string);

// Human-readable multi-line summary of a disco#info caps result for XDEBUG.
// Includes identities, sorted feature list, and advertised vs computed ver hash.
[[nodiscard]] XMPP_TEST_EXPORT std::string format_caps_debug_summary(
    std::string_view jid,
    StanzaView query,
    const std::vector<std::string> &features,
    std::string_view advertised_ver,
    std::string_view computed_ver);

[[nodiscard]] XMPP_TEST_EXPORT bool caps_requested_node_ok(
    std::string_view requested_node,
    std::string_view computed_hash);

}  // namespace xmpp