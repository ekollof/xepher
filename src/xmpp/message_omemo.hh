// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "test_export.hh"
#include "xmpp/stanza_view.hh"

namespace xmpp {

inline constexpr std::string_view k_axolotl_ns = "eu.siacs.conversations.axolotl";
inline constexpr std::string_view k_omemo_undecryptable_placeholder =
    "[undecryptable OMEMO message]";

[[nodiscard]] XMPP_TEST_EXPORT StanzaView stanza_axolotl_encrypted(StanzaView msg);

[[nodiscard]] XMPP_TEST_EXPORT std::optional<std::uint32_t>
axolotl_header_sender_id(StanzaView encrypted);

[[nodiscard]] XMPP_TEST_EXPORT bool is_own_device_omemo_self_copy(
    StanzaView encrypted, std::uint32_t own_device_id);

struct OmemoStableIdRefs {
    std::string_view origin_id;
    std::string_view stanza_id;
    std::string_view message_id;
};

// Prefer origin-id, then stanza-id, then message id attribute.
[[nodiscard]] XMPP_TEST_EXPORT std::string omemo_stable_id(const OmemoStableIdRefs &ids);

struct OmemoSelfCopyAdvice {
    bool apply_advice = false;
    bool clear_encrypted_on_mam = false;
};

[[nodiscard]] XMPP_TEST_EXPORT OmemoSelfCopyAdvice evaluate_omemo_self_copy_advice(
    bool has_encrypted,
    bool is_self_outbound_copy,
    bool is_mam_replay,
    bool is_own_device_self_copy,
    bool is_carbon_copy = false);

[[nodiscard]] XMPP_TEST_EXPORT bool should_note_omemo_peer_traffic(
    bool has_encrypted,
    bool is_self_outbound_copy,
    bool is_pm_channel,
    bool is_mam_replay);

[[nodiscard]] XMPP_TEST_EXPORT bool should_auto_enable_channel_omemo(
    bool has_encrypted,
    bool is_self_outbound_copy,
    bool channel_omemo_enabled,
    bool is_mam_replay);

// PM: peer bare JID from the stanza. MUC: sender real bare JID only — nullopt when unknown
// (never fall back to the room JID for Signal session lookup).
[[nodiscard]] XMPP_TEST_EXPORT std::optional<std::string> resolve_omemo_decode_jid(
    bool is_muc,
    std::string_view from_bare,
    std::optional<std::string_view> muc_sender_real_jid);

struct OmemoPlaintextCacheIds {
    std::string_view stanza_id;
    std::string_view origin_id;
    std::string_view message_id;
};

// Unique non-empty message ids to store under a channel key.
[[nodiscard]] XMPP_TEST_EXPORT std::vector<std::string>
omemo_plaintext_cache_ids(const OmemoPlaintextCacheIds &ids);

enum class OmemoDecryptFailureDisposition {
    ContinueAfterOmemo,
    ShowUndecryptablePlaceholder,
    AbortSilent,
    ShowDecryptionError,
};

struct OmemoDecryptFailureInput {
    bool is_self_outbound_copy = false;
    bool is_mam_replay = false;
    bool is_carbon_copy = false;
    bool payload_missing_or_empty = false;
};

[[nodiscard]] XMPP_TEST_EXPORT OmemoDecryptFailureDisposition
disposition_for_omemo_decrypt_failure(const OmemoDecryptFailureInput &in);

[[nodiscard]] XMPP_TEST_EXPORT bool axolotl_payload_is_empty(StanzaView encrypted);

[[nodiscard]] XMPP_TEST_EXPORT bool should_skip_display_after_omemo(
    bool has_encrypted,
    bool has_cleartext,
    bool is_self_outbound_copy,
    bool is_mam_replay,
    bool is_carbon_copy = false);

}  // namespace xmpp