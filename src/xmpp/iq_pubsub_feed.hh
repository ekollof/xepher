// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <span>
#include <string>
#include <string_view>

#include "test_export.hh"

namespace xmpp {

inline constexpr std::string_view k_microblog_comments_prefix =
    "urn:xmpp:microblog:0:comments/";

[[nodiscard]] XMPP_TEST_EXPORT bool is_skipped_non_atom_feed_item_id(std::string_view item_id);

[[nodiscard]] XMPP_TEST_EXPORT bool is_microblog_comments_node(std::string_view node);

// True when jid looks like a PubSub service component (not a user PEP target).
[[nodiscard]] XMPP_TEST_EXPORT bool is_pubsub_component_jid(
    std::string_view jid,
    std::span<const std::string> known_pubsub_services = {});

// /feed <jid>: default to urn:xmpp:microblog:0 only for user JIDs, not services.
[[nodiscard]] XMPP_TEST_EXPORT bool should_default_pep_microblog_node(
    std::string_view service_jid,
    std::span<const std::string> known_pubsub_services = {});

}  // namespace xmpp