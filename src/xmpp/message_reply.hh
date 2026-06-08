// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "test_export.hh"
#include "xmpp/stanza_view.hh"

namespace xmpp {

struct MessageReply {
    std::string target_id;
};

[[nodiscard]] XMPP_TEST_EXPORT std::optional<MessageReply> parse_message_reply(StanzaView msg);

// Reject OG/OOB box-drawing continuation sub-lines (after color codes stripped).
[[nodiscard]] XMPP_TEST_EXPORT bool is_og_preview_continuation_line(std::string_view plain);

// Skip leading ↪ reply-chain prefixes from plain body text.
[[nodiscard]] XMPP_TEST_EXPORT std::string strip_leading_reply_chain(std::string_view text);

[[nodiscard]] XMPP_TEST_EXPORT bool should_truncate_reply_excerpt(std::string_view text);

[[nodiscard]] XMPP_TEST_EXPORT std::string build_reply_excerpt(std::string_view clean_text);

[[nodiscard]] XMPP_TEST_EXPORT std::optional<std::string>
nick_from_line_tags(std::span<const std::string_view> tags);

// Extract displayable body text from a buffer line message field (strips colors).
[[nodiscard]] XMPP_TEST_EXPORT std::string extract_line_body_text(std::string_view raw);

// Quote-column body for the separate reply context line (XEP-0461).
[[nodiscard]] XMPP_TEST_EXPORT std::string format_reply_quote_body(
    std::string_view quote_nick, std::string_view excerpt);

inline constexpr std::string_view default_reply_excerpt()
{
    return "[reply]";
}

}  // namespace xmpp