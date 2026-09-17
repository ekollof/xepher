// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "message_fallback.hh"

#include "util.hh"

namespace xmpp {

namespace {

[[nodiscard]] bool stanza_has_special_fallback_handler(StanzaView msg)
{
    return msg.child("reactions", k_reactions_ns).valid()
        || msg.child("retract", k_retract_ns).valid()
        || msg.child("apply-to", k_fasten_ns).valid();
}

// XEP-0428 start/end are Unicode code-point offsets (inclusive/exclusive).
[[nodiscard]] std::size_t utf8_codepoint_byte_offset(std::string_view text, std::size_t cp_index)
{
    std::size_t byte = 0;
    std::size_t cp = 0;
    while (byte < text.size() && cp < cp_index)
    {
        const unsigned char c = static_cast<unsigned char>(text[byte]);
        std::size_t len = 1;
        if ((c & 0xE0) == 0xC0)
            len = 2;
        else if ((c & 0xF0) == 0xE0)
            len = 3;
        else if ((c & 0xF8) == 0xF0)
            len = 4;
        if (byte + len > text.size())
            return text.size();
        byte += len;
        ++cp;
    }
    return byte;
}

[[nodiscard]] StanzaView find_reply_fallback(StanzaView msg)
{
    StanzaView first_fallback;
    for (const auto &child : msg)
    {
        if (child.name() != "fallback")
            continue;
        const auto ns = child.xmlns();
        if (!ns || *ns != k_fallback_ns)
            continue;
        if (!first_fallback.valid())
            first_fallback = child;
        if (child.attr_string("for") == k_reply_ns)
            return child;
    }
    return first_fallback;
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

    const long end_cp = parse_int64(end_attr).value_or(0);
    if (end_cp <= 0)
        return {};

    const std::string start_attr = fb_body.attr_string("start");
    long start_cp = start_attr.empty() ? 0L : parse_int64(start_attr).value_or(0);
    if (start_cp < 0)
        start_cp = 0;
    if (start_cp >= end_cp)
        return {};

    const std::size_t start_byte =
        utf8_codepoint_byte_offset(text, static_cast<std::size_t>(start_cp));
    const std::size_t end_byte =
        utf8_codepoint_byte_offset(text, static_cast<std::size_t>(end_cp));
    if (start_byte >= end_byte)
        return {};

    FallbackBodyResult result;
    result.stripped = std::string(text.substr(start_byte, end_byte - start_byte));

    if (start_byte > 0 || end_byte < text.size())
    {
        std::string rebuilt;
        if (start_byte > 0)
            rebuilt = std::string(text.substr(0, start_byte));

        std::string_view suffix = text.substr(end_byte);
        const auto first_non_ws = suffix.find_first_not_of(" \t\r\n");
        if (first_non_ws != std::string_view::npos)
            suffix.remove_prefix(first_non_ws);
        rebuilt += suffix;

        result.disposition = FallbackBodyDisposition::Trimmed;
        result.trimmed = std::move(rebuilt);
        return result;
    }

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

    if (!msg.child("reply", k_reply_ns).valid())
        return {};

    const StanzaView fallback_elem = find_reply_fallback(msg);
    if (!fallback_elem.valid())
        return {};

    return trim_reply_fallback_quote(fallback_elem, body_text);
}

}  // namespace xmpp
