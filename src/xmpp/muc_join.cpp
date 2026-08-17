// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "muc_join.hh"

#include <fmt/core.h>

#include "account.hh"
#include "channel.hh"
#include "weechat/ui_port.hh"
#include "xmpp/iq_bookmarks.hh"
#include "xmpp/node.hh"
#include "xmpp/stanza.hh"
#include "xmpp/xep-0045.inl"

namespace xmpp {

std::string muc_presence_jid(const std::string_view room_bare,
                             const std::string_view bookmark_nick,
                             const std::string_view account_nick,
                             const std::string_view account_jid)
{
    const ::jid parsed(nullptr, std::string(room_bare));
    std::string effective_nick;
    if (!bookmark_nick.empty())
        effective_nick = std::string(bookmark_nick);
    else if (!account_nick.empty())
        effective_nick = std::string(account_nick);
    else
        effective_nick = ::jid(nullptr, std::string(account_jid)).local;

    return fmt::format("{}@{}/{}", parsed.local, parsed.domain, effective_nick);
}

void send_muc_join_presence(weechat::account &account,
                            const std::string_view pres_jid,
                            const std::string_view room_password)
{
    auto join_pres = stanza::presence().to(pres_jid).from(account.jid());
    static_cast<stanza::xep0045::presence &>(join_pres).muc_join(room_password);
    account.connection.send(join_pres.build(account.context).get());

    const ::jid parsed(nullptr, std::string(pres_jid));
    if (!parsed.bare.empty() && !parsed.resource.empty())
    {
        if (auto ch_it = account.channels.find(parsed.bare);
            ch_it != account.channels.end())
            ch_it->second.set_self_nick(parsed.resource);
    }
}

void rejoin_open_mucs(weechat::account &account, const std::string_view reason)
{
    const std::string account_nick(account.nickname());
    const std::string account_jid(account.jid());
    const std::string reason_str(reason.empty() ? "reconnect" : reason);

    for (auto &[room_jid, ch] : account.channels)
    {
        if (ch.type != weechat::channel::chat_type::MUC || !ch.buffer)
            continue;
        if (is_biboumi_gateway_room(room_jid))
            continue;

        std::string_view bookmark_nick;
        if (auto bm_it = account.bookmarks.find(room_jid);
            bm_it != account.bookmarks.end())
            bookmark_nick = bm_it->second.nick;

        const std::string pres_jid = muc_presence_jid(
            room_jid, bookmark_nick, account_nick, account_jid);

        // Reset join flood filter so history/occupants refresh like a real join.
        ch.joining = true;
        weechat::UiPort::for_buffer(account.buffer)->printf_network(
            fmt::format("Re-joining MUC after {}: {}", reason_str, room_jid));
        send_muc_join_presence(account, pres_jid);
    }
}

}  // namespace xmpp