// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "test_export.hh"
#include "xmpp/stanza_view.hh"

namespace xmpp {

inline constexpr std::string_view k_muc_invite_ns = "jabber:x:conference";

[[nodiscard]] XMPP_TEST_EXPORT bool stanza_is_error_message(StanzaView msg);

struct DirectMucInvite {
    std::string inviter_bare;
    std::string room_jid;
    std::optional<std::string> password;
    std::optional<std::string> reason;
};

[[nodiscard]] XMPP_TEST_EXPORT std::optional<DirectMucInvite>
parse_direct_muc_invite(StanzaView msg);

}  // namespace xmpp