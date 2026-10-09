// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
#pragma once

#include <expected>
#include <optional>
#include <string>
#include <unordered_set>
#include "xmpp/node.hh"
#include "xmpp/stanza_view.hh"
#include "test_export.hh"

namespace xmpp {
struct PubsubMamQuery {
    std::string service;
    std::string node;
    std::string cursor;
    int max_items = 20;
    bool initial_tail = true;
    bool stable = true;
    bool stale_retried = false;
    bool order_creation = false;
    std::unordered_set<std::string> visited_cursors;
};

struct PubsubMamPage {
    std::optional<PubsubMamQuery> next;
    // No value: do not alter the checkpoint; empty value: archive is empty.
    std::optional<std::string> checkpoint;
};

[[nodiscard]] XMPP_TEST_EXPORT auto make_pubsub_mam_iq(
    std::string_view id, std::string_view from, const PubsubMamQuery &query) -> stanza::iq;

[[nodiscard]] XMPP_TEST_EXPORT auto advance_pubsub_mam(PubsubMamQuery query, StanzaView fin)
    -> std::expected<PubsubMamPage, std::string>;

[[nodiscard]] XMPP_TEST_EXPORT auto pubsub_mam_publisher(
    StanzaView message, StanzaView item, std::string_view service) -> std::string;

[[nodiscard]] XMPP_TEST_EXPORT auto recover_pubsub_mam_cursor(
    const PubsubMamQuery &query, StanzaView iq) -> std::optional<PubsubMamQuery>;
}
