// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "message_fallback.hh"

#include "../util.hh"

namespace xmpp {

namespace {

[[nodiscard]] bool stanza_has_special_fallback_handler(StanzaView msg)
{
    return msg.child("reactions", k_reactions_ns).valid()
        || msg.child("retract", k_retract_ns).valid()
        || msg.child("apply-to", k_fasten_ns).valid();
}

[[nodiscard]] FallbackBodyResult trim_reply_fallback_quote(
    StanzaView fallback_elem, std::string_view text)
{
    const StanzaView fb_body = fallback_elem.child("body");
    if (!fb_body.valid())
        return {};

    const std::string end_attr = fb_body.attr_string("end");
    if (end_attr.empty())
        return {};

    const long end = parse_int64(end_attr).value_or(0);
    if (end <= 0)
        return {};

    const std::string start_attr = fb_body.attr_string("start");
    long start = start_attr.empty() ? 0L : parse_int64(start_attr).value_or(0);
    if (start < 0)
        start = 0;

    if (static_cast<std::size_t>(end) < text.size())
    {
        std::string rebuilt;
        if (start > 0)
            rebuilt = std::string(text.substr(0, static_cast<std::size_t>(start)));

        std::string_view suffix = text.substr(static_cast<std::size_t>(end));
        const auto first_non_ws = suffix.find_first_not_of(" \t\r\n");
        if (first_non_ws != std::string_view::npos)
            suffix.remove_prefix(first_non_ws);
        rebuilt += suffix;

        FallbackBodyResult result;
        result.disposition = FallbackBodyDisposition::Trimmed;
        result.trimmed = std::move(rebuilt);
        return result;
    }

    FallbackBodyResult result;
    result.disposition = FallbackBodyDisposition::Cleared;
    return result;
}

}  // namespace

bool stanza_has_fallback(StanzaView msg)
{
    return msg.child("fallback", k_fallback_ns).valid();
}

FallbackBodyResult apply_fallback_body_trim(
    StanzaView msg, std::string_view body_text, bool has_message_correction)
{
    if (body_text.empty() || has_message_correction || !stanza_has_fallback(msg))
        return {};

    if (stanza_has_special_fallback_handler(msg))
    {
        FallbackBodyResult result;
        result.disposition = FallbackBodyDisposition::Cleared;
        return result;
    }

    const StanzaView fallback_elem = msg.child("fallback", k_fallback_ns);
    if (!msg.child("reply", k_reply_ns).valid())
        return {};

    return trim_reply_fallback_quote(fallback_elem, body_text);
}

}  // namespace xmpp