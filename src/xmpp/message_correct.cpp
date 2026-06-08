// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "message_correct.hh"

#include <fmt/core.h>

#include "xmpp/node.hh"

namespace xmpp {

bool stanza_has_message_correction(StanzaView msg)
{
    return msg.child("replace", k_message_correct_ns).valid();
}

std::optional<MessageCorrection> parse_message_correction(StanzaView msg)
{
    const StanzaView replace = msg.child("replace", k_message_correct_ns);
    if (!replace.valid())
        return std::nullopt;

    const std::string target_id = replace.attr_string("id");
    if (target_id.empty())
        return std::nullopt;

    MessageCorrection correction;
    correction.target_id = target_id;
    return correction;
}

std::string message_correction_sender_key(
    std::string_view from_full,
    std::string_view from_bare,
    bool is_muc_channel)
{
    if (!is_muc_channel)
        return std::string(from_bare);

    const std::string resource = ::jid(nullptr, std::string(from_full).c_str()).resource;
    return resource.empty() ? std::string(from_bare) : resource;
}

std::string format_message_correction_text(std::string_view corrected_body)
{
    return fmt::format("📝 {}", corrected_body);
}

}  // namespace xmpp