// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <string_view>

#include "test_export.hh"

namespace xmpp {

inline constexpr std::string_view k_pubsub_event_ns =
    "http://jabber.org/protocol/pubsub#event";

inline constexpr std::string_view k_pep_avatar_metadata = "urn:xmpp:avatar:metadata";
inline constexpr std::string_view k_pep_avatar_data = "urn:xmpp:avatar:data";
inline constexpr std::string_view k_pep_bookmarks = "urn:xmpp:bookmarks:1";
inline constexpr std::string_view k_pep_mds_displayed = "urn:xmpp:mds:displayed:0";
inline constexpr std::string_view k_pep_omemo2_devices = "urn:xmpp:omemo:2:devices";
inline constexpr std::string_view k_pep_microblog = "urn:xmpp:microblog:0";

struct PubsubFeedGate {
    bool is_generic_feed = false;
    bool drop_legacy_omemo = false;
};

[[nodiscard]] XMPP_TEST_EXPORT bool pep_node_is_microblog(std::string_view node);

[[nodiscard]] XMPP_TEST_EXPORT bool pep_node_is_protocol_uri(std::string_view node);

[[nodiscard]] XMPP_TEST_EXPORT bool pep_from_is_self(
    std::string_view from_full, std::string_view own_jid);

[[nodiscard]] XMPP_TEST_EXPORT bool pep_node_is_legacy_omemo(std::string_view node);

[[nodiscard]] XMPP_TEST_EXPORT bool pep_node_is_known_protocol_node(std::string_view node);

[[nodiscard]] XMPP_TEST_EXPORT PubsubFeedGate classify_generic_pubsub_feed(
    std::string_view node,
    std::string_view from_full,
    std::string_view own_jid);

}  // namespace xmpp