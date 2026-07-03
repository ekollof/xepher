// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <string>
#include <string_view>

#include "test_export.hh"

namespace weechat {
class account;
}

namespace xmpp {

// Full MUC presence JID (room@conf/nick) for join/self-ping.
// bookmark_nick wins when non-empty; else account_nick; else localpart of account_jid.
[[nodiscard]] XMPP_TEST_EXPORT std::string muc_presence_jid(
    std::string_view room_bare,
    std::string_view bookmark_nick,
    std::string_view account_nick,
    std::string_view account_jid);

// XEP-0045 §7.1: send directed join presence to pres_jid (room@conf/nick).
void send_muc_join_presence(weechat::account &account,
                            std::string_view pres_jid,
                            std::string_view room_password = {});

}  // namespace xmpp