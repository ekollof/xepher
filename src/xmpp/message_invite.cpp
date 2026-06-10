// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "message_invite.hh"

#include <string_view>

#include "xmpp/node.hh"

namespace xmpp {

namespace {

[[nodiscard]] auto bare_jid_from(std::string_view full) -> std::string
{
    return ::jid(nullptr, std::string(full).c_str()).bare;
}

} // namespace

bool stanza_is_error_message(StanzaView msg)
{
    const auto type = msg.type();
    return type && *type == "error";
}

std::optional<DirectMucInvite> parse_direct_muc_invite(StanzaView msg)
{
    const StanzaView invite = msg.child("x", k_muc_invite_ns);
    if (!invite.valid())
        return std::nullopt;

    const std::string room_jid = invite.attr_string("jid");
    if (room_jid.empty())
        return std::nullopt;

    DirectMucInvite parsed;
    parsed.room_jid = room_jid;

    if (const std::string password = invite.attr_string("password"); !password.empty())
        parsed.password = password;

    if (const std::string reason = invite.attr_string("reason"); !reason.empty())
        parsed.reason = reason;

    if (const auto from = msg.from(); from && !from->empty())
        parsed.inviter_bare = bare_jid_from(*from);
    else
        parsed.inviter_bare = "unknown";

    return parsed;
}

std::optional<MediatedMucInvite> parse_mediated_muc_invite(StanzaView msg)
{
    const StanzaView muc_user = msg.child("x", k_muc_user_ns);
    if (!muc_user.valid())
        return std::nullopt;

    const StanzaView invite = muc_user.child("invite");
    if (!invite.valid())
        return std::nullopt;

    const auto from = msg.from();
    if (!from || from->empty())
        return std::nullopt;

    MediatedMucInvite parsed;
    parsed.room_jid = bare_jid_from(*from);
    if (parsed.room_jid.empty())
        return std::nullopt;

    if (const std::string inviter = invite.attr_string("from"); !inviter.empty())
        parsed.inviter_bare = bare_jid_from(inviter);

    if (const StanzaView reason_el = invite.child("reason"); reason_el.valid())
    {
        if (const std::string reason = reason_el.text(); !reason.empty())
            parsed.reason = reason;
    }

    if (const StanzaView password_el = muc_user.child("password"); password_el.valid())
    {
        if (const std::string password = password_el.text(); !password.empty())
            parsed.password = password;
    }

    return parsed;
}

std::optional<MediatedMucDecline> parse_mediated_muc_decline(StanzaView msg)
{
    const StanzaView muc_user = msg.child("x", k_muc_user_ns);
    if (!muc_user.valid())
        return std::nullopt;

    const StanzaView decline = muc_user.child("decline");
    if (!decline.valid())
        return std::nullopt;

    const auto from = msg.from();
    if (!from || from->empty())
        return std::nullopt;

    MediatedMucDecline parsed;
    parsed.room_jid = bare_jid_from(*from);
    if (parsed.room_jid.empty())
        return std::nullopt;

    if (const std::string decliner = decline.attr_string("from"); !decliner.empty())
        parsed.decliner_bare = bare_jid_from(decliner);

    if (const StanzaView reason_el = decline.child("reason"); reason_el.valid())
    {
        if (const std::string reason = reason_el.text(); !reason.empty())
            parsed.reason = reason;
    }

    return parsed;
}

}  // namespace xmpp