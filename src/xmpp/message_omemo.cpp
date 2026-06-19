// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "message_omemo.hh"

#include <ranges>

#include "util.hh"

namespace xmpp {

namespace {

[[nodiscard]] bool id_nonempty(std::string_view id)
{
    return !id.empty();
}

}  // namespace

StanzaView stanza_axolotl_encrypted(StanzaView msg)
{
    return msg.child("encrypted", k_axolotl_ns);
}

std::optional<std::uint32_t> axolotl_header_sender_id(StanzaView encrypted)
{
    if (!encrypted.valid())
        return std::nullopt;

    const StanzaView header = encrypted.child("header");
    if (!header.valid())
        return std::nullopt;

    const std::string sid_str = header.attr_string("sid");
    if (sid_str.empty())
        return std::nullopt;

    if (auto sid = parse_uint32(sid_str); sid)
        return *sid;
    return std::nullopt;
}

bool is_own_device_omemo_self_copy(StanzaView encrypted, std::uint32_t own_device_id)
{
    if (auto sender = axolotl_header_sender_id(encrypted))
        return *sender == own_device_id;
    return false;
}

std::string omemo_stable_id(const OmemoStableIdRefs &ids)
{
    if (id_nonempty(ids.origin_id))
        return std::string(ids.origin_id);
    if (id_nonempty(ids.stanza_id))
        return std::string(ids.stanza_id);
    return std::string(ids.message_id);
}

OmemoSelfCopyAdvice evaluate_omemo_self_copy_advice(
    bool has_encrypted,
    bool is_self_outbound_copy,
    bool is_mam_replay,
    bool is_own_device_self_copy,
    bool is_carbon_copy)
{
    OmemoSelfCopyAdvice advice;
    if (!has_encrypted || !is_self_outbound_copy)
        return advice;

    // Live carbons from another device must be decrypted/displayed here.
    if (is_carbon_copy && !is_mam_replay && !is_own_device_self_copy)
        return advice;

    if (!is_mam_replay || is_own_device_self_copy)
    {
        advice.apply_advice = true;
        advice.clear_encrypted_on_mam = is_mam_replay;
    }
    return advice;
}

bool should_note_omemo_peer_traffic(
    bool has_encrypted,
    bool is_self_outbound_copy,
    bool is_pm_channel,
    bool is_mam_replay)
{
    return has_encrypted && !is_self_outbound_copy && is_pm_channel && !is_mam_replay;
}

bool should_auto_enable_channel_omemo(
    bool has_encrypted,
    bool is_self_outbound_copy,
    bool channel_omemo_enabled,
    bool is_mam_replay)
{
    return has_encrypted && !is_self_outbound_copy && !channel_omemo_enabled
        && !is_mam_replay;
}

std::optional<std::string> resolve_omemo_decode_jid(
    const bool is_muc,
    std::string_view from_bare,
    std::optional<std::string_view> muc_sender_real_jid)
{
    if (is_muc)
    {
        if (muc_sender_real_jid && !muc_sender_real_jid->empty())
        {
            if (*muc_sender_real_jid == from_bare)
                return std::nullopt;  // server reported the MUC itself as the "real" JID for this occupant — ignore (issue #6 speed bump: prevents room@conf being used as OMEMO peer)
            return std::string(*muc_sender_real_jid);
        }
        return std::nullopt;
    }
    return std::string(from_bare);
}

std::vector<std::string> omemo_plaintext_cache_ids(const OmemoPlaintextCacheIds &ids)
{
    std::vector<std::string> out;
    auto push_unique = [&](std::string_view id) {
        if (!id_nonempty(id))
            return;
        const std::string s(id);
        if (std::ranges::find(out, s) == out.end())
            out.push_back(s);
    };

    push_unique(ids.stanza_id);
    push_unique(ids.origin_id);
    push_unique(ids.message_id);
    return out;
}

OmemoDecryptFailureDisposition disposition_for_omemo_decrypt_failure(
    const OmemoDecryptFailureInput &in)
{
    if (in.is_self_outbound_copy && in.is_carbon_copy)
        return OmemoDecryptFailureDisposition::ShowUndecryptablePlaceholder;
    if (in.is_self_outbound_copy)
        return OmemoDecryptFailureDisposition::ContinueAfterOmemo;
    if (in.is_mam_replay)
        return OmemoDecryptFailureDisposition::ShowUndecryptablePlaceholder;
    if (in.payload_missing_or_empty)
        return OmemoDecryptFailureDisposition::AbortSilent;
    return OmemoDecryptFailureDisposition::ShowDecryptionError;
}

bool axolotl_payload_is_empty(StanzaView encrypted)
{
    if (!encrypted.valid())
        return true;
    const StanzaView payload = encrypted.child("payload");
    return !payload.valid() || payload.text().empty();
}

bool should_skip_display_after_omemo(
    bool has_encrypted,
    bool has_cleartext,
    bool is_self_outbound_copy,
    bool is_mam_replay,
    bool is_carbon_copy)
{
    if (is_carbon_copy && !is_mam_replay)
        return false;
    return has_encrypted && !has_cleartext && (is_self_outbound_copy || is_mam_replay);
}

}  // namespace xmpp