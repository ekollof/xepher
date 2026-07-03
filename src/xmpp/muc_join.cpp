// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "muc_join.hh"

#include <fmt/core.h>

#include "account.hh"
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
}

}  // namespace xmpp