// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "message_invite.hh"

#include "xmpp/node.hh"

namespace xmpp {

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

    const std::string password = invite.attr_string("password");
    if (!password.empty())
        parsed.password = password;

    const std::string reason = invite.attr_string("reason");
    if (!reason.empty())
        parsed.reason = reason;

    if (const auto from = msg.from(); from && !from->empty())
        parsed.inviter_bare = ::jid(nullptr, std::string(*from).c_str()).bare;
    else
        parsed.inviter_bare = "unknown";

    return parsed;
}

}  // namespace xmpp