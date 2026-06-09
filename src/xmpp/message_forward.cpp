// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "message_forward.hh"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <ranges>

#include "xmpp/node.hh"

namespace xmpp {

namespace {

[[nodiscard]] StanzaView carbon_wrapper(StanzaView envelope)
{
    const StanzaView sent = envelope.child("sent", k_carbons_ns);
    if (sent.valid())
        return sent;
    return envelope.child("received", k_carbons_ns);
}

[[nodiscard]] bool bare_jid_matches(std::string_view full_jid, std::string_view bare_jid)
{
    if (full_jid.empty() || bare_jid.empty())
        return false;
    return bare_jid_iequals(::jid(nullptr, std::string(full_jid).c_str()).bare, bare_jid);
}

}  // namespace

bool bare_jid_iequals(std::string_view a, std::string_view b)
{
    return a.size() == b.size()
        && std::ranges::equal(a, b, [](unsigned char x, unsigned char y) {
            return std::tolower(x) == std::tolower(y);
        });
}

bool stanza_has_user_message_payload(StanzaView msg)
{
    const StanzaView body = msg.child("body");
    if (body.valid() && !body.text().empty())
        return true;

    const StanzaView encrypted = msg.child("encrypted", "eu.siacs.conversations.axolotl");
    if (encrypted.valid())
    {
        const StanzaView payload = encrypted.child("payload");
        if (payload.valid() && !payload.text().empty())
            return true;
    }

    const StanzaView pgp = msg.child("x", "jabber:x:encrypted");
    if (pgp.valid() && !pgp.text().empty())
        return true;

    return false;
}

bool stanza_is_carbon(StanzaView msg)
{
    const StanzaView wrapper = carbon_wrapper(msg);
    return wrapper.valid() && wrapper.child("forwarded", k_forward_ns).valid();
}

bool carbon_sender_is_account(StanzaView envelope, std::string_view account_bare_jid)
{
    const auto from = envelope.from();
    if (!from || from->empty())
        return false;
    return bare_jid_matches(*from, account_bare_jid);
}

std::optional<xmpp_stanza_t *> parse_carbon_inner_message(StanzaView envelope,
                                                          std::string_view account_bare_jid)
{
    if (!stanza_is_carbon(envelope))
        return std::nullopt;
    if (!carbon_sender_is_account(envelope, account_bare_jid))
        return std::nullopt;

    const StanzaView wrapper = carbon_wrapper(envelope);
    const StanzaView forwarded = wrapper.child("forwarded", k_forward_ns);
    const StanzaView inner = forwarded.child("message");
    if (!inner.valid())
        return std::nullopt;
    return inner.raw();
}

bool stanza_is_mam_result(StanzaView msg)
{
    return msg.child("result", k_mam_ns).valid();
}

std::optional<std::string> mam_pubsub_query_id(StanzaView msg)
{
    const StanzaView result = msg.child("result", k_mam_ns);
    if (!result.valid())
        return std::nullopt;
    const std::string queryid = result.attr_string("queryid");
    if (queryid.empty())
        return std::nullopt;
    return queryid;
}

std::optional<MamForwardedDispatch> parse_mam_forwarded_dispatch(StanzaView envelope)
{
    const StanzaView result = envelope.child("result", k_mam_ns);
    if (!result.valid())
        return std::nullopt;

    const StanzaView forwarded = result.child("forwarded", k_forward_ns);
    if (!forwarded.valid())
        return std::nullopt;

    const StanzaView message = forwarded.child("message");
    if (!message.valid())
        return std::nullopt;

    MamForwardedDispatch dispatch;
    dispatch.message = message.raw();
    dispatch.archive_id = result.attr_string("id");

    const StanzaView delay = forwarded.child("delay", k_delay_ns);
    if (delay.valid())
        dispatch.delay_stamp = delay.attr_string("stamp");

    return dispatch;
}

std::optional<std::string> mam_conversation_partner_jid(std::string_view from_bare,
                                                        std::string_view to_bare,
                                                        std::string_view account_bare_jid)
{
    if (!from_bare.empty() && !bare_jid_iequals(from_bare, account_bare_jid))
        return std::string(from_bare);
    if (!to_bare.empty() && !bare_jid_iequals(to_bare, account_bare_jid))
        return std::string(to_bare);
    return std::nullopt;
}

std::optional<std::string> conversation_channel_jid(std::string_view from_bare,
                                                    std::string_view to_bare,
                                                    std::string_view account_bare_jid)
{
    if (!from_bare.empty() && bare_jid_iequals(from_bare, account_bare_jid))
        return to_bare.empty() ? std::nullopt : std::optional(std::string(to_bare));
    if (!from_bare.empty())
        return std::string(from_bare);
    if (!to_bare.empty() && !bare_jid_iequals(to_bare, account_bare_jid))
        return std::string(to_bare);
    return std::nullopt;
}

std::optional<std::string> conversation_channel_jid_from_message(
    std::string_view from_full,
    std::string_view to_full,
    std::string_view account_bare_jid)
{
    std::string from_bare;
    if (!from_full.empty())
        from_bare = ::jid(nullptr, std::string(from_full).c_str()).bare;

    const std::string to_bare = to_full.empty()
        ? std::string{}
        : ::jid(nullptr, std::string(to_full).c_str()).bare;

    if (from_bare.empty() && !to_bare.empty()
        && !bare_jid_iequals(to_bare, account_bare_jid))
        from_bare = std::string(account_bare_jid);

    return conversation_channel_jid(from_bare, to_bare, account_bare_jid);
}

std::optional<std::string> mam_channel_jid_for_addresses(std::string_view from_bare,
                                                           std::string_view to_bare,
                                                           std::string_view account_bare_jid)
{
    return mam_conversation_partner_jid(from_bare, to_bare, account_bare_jid);
}

bool should_discover_pm_channel_from_mam(const MamPmDiscoveryPolicy &policy)
{
    return policy.has_partner_jid
        && !policy.is_groupchat
        && !policy.channel_already_exists
        && !policy.deliberately_closed
        && policy.has_user_payload;
}

MamDedupNeedles mam_dedup_needles(std::string_view archive_id, std::string_view message_id)
{
    MamDedupNeedles needles;
    if (!archive_id.empty())
        needles.stanza_id_needle = std::string("stanza_id_") + std::string(archive_id);
    if (!message_id.empty())
        needles.message_id_needle = std::string("id_") + std::string(message_id);
    return needles;
}

std::string mam_cache_from_label(std::string_view message_type,
                                 std::string_view msg_from_full,
                                 std::string_view msg_from_bare)
{
    if (message_type == "groupchat")
    {
        const std::string nick = ::jid(nullptr, std::string(msg_from_full).c_str()).resource;
        return nick.empty() ? std::string(msg_from_bare) : nick;
    }
    return std::string(msg_from_bare);
}

time_t parse_forward_delay_stamp(std::string_view stamp)
{
    if (stamp.empty())
        return 0;

    struct tm time {};
    if (!strptime(std::string(stamp).c_str(), "%FT%T", &time))
        return 0;
    return timegm(&time);
}

}  // namespace xmpp