// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "test_export.hh"
#include "node.hh"
#include "stanza_view.hh"

namespace xmpp {

inline constexpr std::string_view k_receipts_ns = "urn:xmpp:receipts";
inline constexpr std::string_view k_chat_markers_ns = "urn:xmpp:chat-markers:0";

struct IncomingReceiptAck {
    std::string from;
    std::string acked_id;
};

struct IncomingDisplayedAck {
    std::string from;
    std::string acked_id;
};

struct AckReplyInput {
    std::string message_id;
    std::string reply_to;
    std::string message_type;
    bool receipt_requested = false;
    bool marker_markable = false;
    std::optional<std::string> thread;
    std::optional<std::string> stanza_id;
    std::optional<std::string> stanza_id_by;
};

struct AckReplySuppress {
    bool self_outbound_copy = false;
    bool muc_channel = false;
    bool mam_replay = false;
    bool delayed_delivery = false;
};

struct PendingUnread {
    std::string id;
    std::optional<std::string> thread;
    std::optional<std::string> stanza_id;
    std::optional<std::string> stanza_id_by;
};

struct AckReplyResult {
    // Immediate XEP-0184 delivery receipt — safe to send on receipt
    // (delivery is not display).
    std::optional<stanza::message> reply;
    // Deferred XEP-0333 displayed marker payload. Queued in
    // channel::unreads and sent via channel::send_reads() once the user
    // actually views the message — never sent immediately here.
    std::optional<PendingUnread> unread;
};

[[nodiscard]] XMPP_TEST_EXPORT bool stanza_is_receipt_ack(StanzaView msg);
[[nodiscard]] XMPP_TEST_EXPORT bool stanza_is_displayed_ack(StanzaView msg);

// Populated only when from + acked id are both present on an ack stanza.
[[nodiscard]] XMPP_TEST_EXPORT std::optional<IncomingReceiptAck>
parse_incoming_receipt(StanzaView msg);

[[nodiscard]] XMPP_TEST_EXPORT std::optional<IncomingDisplayedAck>
parse_incoming_displayed(StanzaView msg);

[[nodiscard]] XMPP_TEST_EXPORT bool stanza_requests_receipt(StanzaView msg);
[[nodiscard]] XMPP_TEST_EXPORT bool stanza_is_marker_markable(StanzaView msg);
[[nodiscard]] XMPP_TEST_EXPORT bool stanza_is_delayed_delivery(StanzaView msg);

// Build an outgoing receipt/marker reply when policy allows; nullopt when suppressed.
[[nodiscard]] XMPP_TEST_EXPORT std::optional<AckReplyResult>
build_ack_reply(const AckReplyInput &input, const AckReplySuppress &suppress);

}  // namespace xmpp