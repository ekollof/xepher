// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "test_export.hh"
#include "xmpp/server_capabilities.hh"
#include "xmpp/stanza_view.hh"

namespace xmpp {

enum class capability_id {
    stream_management,
    client_state_indication,
    message_carbons,
    global_mam,
    bookmarks,
    mds,
    http_upload,
    pubsub_feeds,
    pubsub_mam,
};

[[nodiscard]] XMPP_TEST_EXPORT std::vector<disco_identity>
disco_identities_from_query(StanzaView query);

[[nodiscard]] XMPP_TEST_EXPORT bool capability_enabled(
    const server_capabilities &caps,
    capability_id id) noexcept;

[[nodiscard]] XMPP_TEST_EXPORT std::string capability_status_label(
    capability_id id,
    const server_capabilities &caps);

[[nodiscard]] XMPP_TEST_EXPORT std::vector<std::string>
format_disco_summary(const server_capabilities &caps);

[[nodiscard]] XMPP_TEST_EXPORT bool features_contain(
    std::span<const std::string> features,
    std::string_view var) noexcept;

}  // namespace xmpp