// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "test_export.hh"
#include "stanza_view.hh"

namespace xmpp {

inline constexpr std::string_view k_legacy_devicelist_node =
    "eu.siacs.conversations.axolotl.devicelist";
inline constexpr std::string_view k_legacy_bundle_node_prefix =
    "eu.siacs.conversations.axolotl.bundles:";

[[nodiscard]] XMPP_TEST_EXPORT bool is_legacy_devicelist_pubsub_node(std::string_view node);

[[nodiscard]] XMPP_TEST_EXPORT bool is_legacy_bundle_pubsub_node(std::string_view node);

[[nodiscard]] XMPP_TEST_EXPORT bool iq_has_legacy_devicelist_pubsub_error(StanzaView iq);

[[nodiscard]] XMPP_TEST_EXPORT bool iq_error_has_item_not_found(StanzaView iq);

[[nodiscard]] XMPP_TEST_EXPORT std::string
omemo_precondition_retry_node_from_publish_id(std::string_view iq_id, std::uint32_t device_id);

}  // namespace xmpp