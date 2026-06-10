// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "test_export.hh"
#include "xmpp/stanza_view.hh"

namespace xmpp {

inline constexpr std::string_view k_muc_invite_ns = "jabber:x:conference";
inline constexpr std::string_view k_muc_user_ns = "http://jabber.org/protocol/muc#user";

[[nodiscard]] XMPP_TEST_EXPORT bool stanza_is_error_message(StanzaView msg);

struct DirectMucInvite {
    std::string inviter_bare;
    std::string room_jid;
    std::optional<std::string> password;
    std::optional<std::string> reason;
};

[[nodiscard]] XMPP_TEST_EXPORT std::optional<DirectMucInvite>
parse_direct_muc_invite(StanzaView msg);

struct MediatedMucInvite {
    std::string room_jid;
    std::optional<std::string> password;
    std::optional<std::string> reason;
    std::optional<std::string> inviter_bare;
};

[[nodiscard]] XMPP_TEST_EXPORT std::optional<MediatedMucInvite>
parse_mediated_muc_invite(StanzaView msg);

struct MediatedMucDecline {
    std::string room_jid;
    std::optional<std::string> reason;
    std::optional<std::string> decliner_bare;
};

[[nodiscard]] XMPP_TEST_EXPORT std::optional<MediatedMucDecline>
parse_mediated_muc_decline(StanzaView msg);

struct MucInviteNotification {
    std::vector<std::string> network_lines;
};

[[nodiscard]] XMPP_TEST_EXPORT MucInviteNotification
render_direct_muc_invite_notification(const DirectMucInvite& invite);

[[nodiscard]] XMPP_TEST_EXPORT MucInviteNotification
render_mediated_muc_invite_notification(const MediatedMucInvite& invite);

[[nodiscard]] XMPP_TEST_EXPORT std::string
render_mediated_muc_decline_notification(const MediatedMucDecline& decline);

struct MucAdminListItem {
    std::string jid;
    std::string nick;
    std::string affiliation;
};

[[nodiscard]] XMPP_TEST_EXPORT std::vector<MucAdminListItem>
parse_muc_admin_list_items(StanzaView admin_query);

struct MucRegisterFormField {
    std::string var;
    std::string label;
    std::string type;
    std::string value;
};

[[nodiscard]] XMPP_TEST_EXPORT std::vector<MucRegisterFormField>
parse_muc_register_form_fields(StanzaView xdata_form);

}  // namespace xmpp