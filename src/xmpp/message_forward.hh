// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <ctime>
#include <optional>
#include <string>
#include <string_view>

#include <strophe.h>

#include "test_export.hh"
#include "xmpp/stanza_view.hh"

namespace xmpp {

inline constexpr std::string_view k_carbons_ns = "urn:xmpp:carbons:2";
inline constexpr std::string_view k_forward_ns = "urn:xmpp:forward:0";
inline constexpr std::string_view k_mam_ns = "urn:xmpp:mam:2";
inline constexpr std::string_view k_delay_ns = "urn:xmpp:delay";

struct MamForwardedDispatch {
    xmpp_stanza_t *message = nullptr;
    std::string archive_id;
    std::string delay_stamp;
};

struct MamDedupNeedles {
    std::string stanza_id_needle;
    std::string message_id_needle;
};

struct MamPmDiscoveryPolicy {
    bool is_groupchat = false;
    bool channel_already_exists = false;
    bool deliberately_closed = false;
    bool has_user_payload = false;
    bool has_partner_jid = false;
};

[[nodiscard]] XMPP_TEST_EXPORT bool stanza_has_user_message_payload(StanzaView msg);

[[nodiscard]] XMPP_TEST_EXPORT bool bare_jid_iequals(std::string_view a, std::string_view b);

// Case-folded bare JID for case-insensitive map keys (ASCII JID localpart@domain).
[[nodiscard]] XMPP_TEST_EXPORT std::string bare_jid_fold_key(std::string_view bare_jid);

[[nodiscard]] XMPP_TEST_EXPORT bool stanza_is_carbon(StanzaView msg);
[[nodiscard]] XMPP_TEST_EXPORT bool carbon_sender_is_account(StanzaView envelope,
                                                             std::string_view account_bare_jid);
[[nodiscard]] XMPP_TEST_EXPORT std::optional<xmpp_stanza_t *>
parse_carbon_inner_message(StanzaView envelope, std::string_view account_bare_jid);

[[nodiscard]] XMPP_TEST_EXPORT bool stanza_is_mam_result(StanzaView msg);
[[nodiscard]] XMPP_TEST_EXPORT std::optional<std::string>
mam_pubsub_query_id(StanzaView msg);
[[nodiscard]] XMPP_TEST_EXPORT std::optional<MamForwardedDispatch>
parse_mam_forwarded_dispatch(StanzaView envelope);

[[nodiscard]] XMPP_TEST_EXPORT std::optional<std::string>
mam_conversation_partner_jid(std::string_view from_bare,
                             std::string_view to_bare,
                             std::string_view account_bare_jid);

[[nodiscard]] XMPP_TEST_EXPORT std::optional<std::string>
conversation_channel_jid(std::string_view from_bare,
                         std::string_view to_bare,
                         std::string_view account_bare_jid);

[[nodiscard]] XMPP_TEST_EXPORT std::optional<std::string>
conversation_channel_jid_from_message(std::string_view from_full,
                                      std::string_view to_full,
                                      std::string_view account_bare_jid);

[[nodiscard]] XMPP_TEST_EXPORT std::optional<std::string>
mam_channel_jid_for_addresses(std::string_view from_bare,
                              std::string_view to_bare,
                              std::string_view account_bare_jid);

[[nodiscard]] XMPP_TEST_EXPORT bool should_discover_pm_channel_from_mam(
    const MamPmDiscoveryPolicy &policy);

[[nodiscard]] XMPP_TEST_EXPORT MamDedupNeedles mam_dedup_needles(std::string_view archive_id,
                                                                 std::string_view message_id);

[[nodiscard]] XMPP_TEST_EXPORT std::string mam_cache_from_label(std::string_view message_type,
                                                                std::string_view msg_from_full,
                                                                std::string_view msg_from_bare);

[[nodiscard]] XMPP_TEST_EXPORT time_t parse_forward_delay_stamp(std::string_view stamp);

}  // namespace xmpp