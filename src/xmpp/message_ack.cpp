// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "message_ack.hh"

namespace xmpp {

bool stanza_is_receipt_ack(StanzaView msg)
{
    return msg.child("received", k_receipts_ns).valid();
}

bool stanza_is_displayed_ack(StanzaView msg)
{
    return msg.child("displayed", k_chat_markers_ns).valid();
}

std::optional<IncomingReceiptAck> parse_incoming_receipt(StanzaView msg)
{
    const StanzaView received = msg.child("received", k_receipts_ns);
    if (!received.valid())
        return std::nullopt;

    IncomingReceiptAck ack;
    if (auto from = msg.from())
        ack.from = std::string(*from);
    ack.acked_id = received.attr_string("id");
    if (ack.from.empty() || ack.acked_id.empty())
        return std::nullopt;
    return ack;
}

std::optional<IncomingDisplayedAck> parse_incoming_displayed(StanzaView msg)
{
    const StanzaView displayed = msg.child("displayed", k_chat_markers_ns);
    if (!displayed.valid())
        return std::nullopt;

    IncomingDisplayedAck ack;
    if (auto from = msg.from())
        ack.from = std::string(*from);
    ack.acked_id = displayed.attr_string("id");
    if (ack.from.empty() || ack.acked_id.empty())
        return std::nullopt;
    return ack;
}

bool stanza_requests_receipt(StanzaView msg)
{
    return msg.child("request", k_receipts_ns).valid();
}

bool stanza_is_marker_markable(StanzaView msg)
{
    return msg.child("markable", k_chat_markers_ns).valid();
}

bool stanza_is_delayed_delivery(StanzaView msg)
{
    return msg.child("delay", "urn:xmpp:delay").valid();
}

std::optional<AckReplyResult> build_ack_reply(const AckReplyInput &input,
                                              const AckReplySuppress &suppress)
{
    if (input.message_id.empty())
        return std::nullopt;
    if (!input.receipt_requested && !input.marker_markable)
        return std::nullopt;
    if (suppress.self_outbound_copy || suppress.muc_channel
        || suppress.mam_replay || suppress.delayed_delivery)
        return std::nullopt;

    stanza::message msg;
    msg.to(input.reply_to).type(input.message_type.empty() ? "chat" : input.message_type);

    if (input.receipt_requested)
        msg.receipt_received(input.message_id);

    if (input.marker_markable)
        msg.chat_marker_displayed(input.message_id);

    if (input.thread.has_value())
        msg.thread(*input.thread);

    msg.no_store();

    PendingUnread unread;
    unread.id = input.message_id;
    unread.thread = input.thread;
    unread.stanza_id = input.stanza_id;
    unread.stanza_id_by = input.stanza_id_by;
    return AckReplyResult{std::move(msg), std::move(unread)};
}

}  // namespace xmpp