// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <optional>
#include <string>
#include <string_view>

struct t_gui_buffer;

#include "test_export.hh"

namespace weechat {

inline constexpr std::string_view k_glyph_pending = " ⌛";
inline constexpr std::string_view k_glyph_delivered = " ✓";
inline constexpr std::string_view k_glyph_seen = " ✓✓";

// Default backward scan depth for buffer line lookups (MAM dedup uses the same cap).
inline constexpr int k_line_store_default_max_scan = 256;

// Strip a leading/trailing delivery-status glyph from message text.
[[nodiscard]] XMPP_TEST_EXPORT std::string strip_status_glyph_prefix(std::string message);
[[nodiscard]] XMPP_TEST_EXPORT std::string strip_status_glyph_suffix(std::string message);
[[nodiscard]] XMPP_TEST_EXPORT std::string strip_delivery_glyphs(std::string message);

// Body text safe for /edit prefill: extract after tab, strip colours and delivery glyphs.
[[nodiscard]] XMPP_TEST_EXPORT std::string clean_editable_line_body(std::string_view raw);

// Outgoing PM self-message: glyph at body start (WeeChat private lines hide nick column).
[[nodiscard]] XMPP_TEST_EXPORT std::string format_self_pm_line(std::string_view prefix,
                                                                 std::string_view body,
                                                                 std::string_view glyph = k_glyph_pending);

// Update delivery glyph on the body; migrates legacy suffix and mistaken prefix glyphs.
[[nodiscard]] XMPP_TEST_EXPORT std::string apply_delivery_glyph_to_line(std::string line,
                                                                          std::string_view glyph);

// Walk buffer lines backwards; update the line tagged id_<acked_id> with new_glyph.
// Returns true when a matching line was found and updated.
[[nodiscard]] bool line_store_update_line_glyph_by_tag(struct t_gui_buffer *buffer,
                                                         std::string_view acked_id,
                                                         std::string_view new_glyph);

// Scan recent buffer lines for any tag string containing one of the needles.
[[nodiscard]] XMPP_TEST_EXPORT bool line_store_buffer_contains_any_tag(
    struct t_gui_buffer *buffer,
    std::initializer_list<std::string_view> needles,
    int max_scan = k_line_store_default_max_scan);

enum class LineStoreLookupResult {
    NotFound,
    Found,
    SenderRejected,
};

// True when a line tagged for target_id exists and nick_ sender tag matches.
[[nodiscard]] XMPP_TEST_EXPORT LineStoreLookupResult
line_store_find_message_line_for_sender(struct t_gui_buffer *buffer,
                                        std::string_view target_id,
                                        std::string_view sender_key,
                                        int max_scan = k_line_store_default_max_scan);

// Update message text on the first line whose tags match target_id.
[[nodiscard]] XMPP_TEST_EXPORT bool line_store_update_message_by_id(
    struct t_gui_buffer *buffer,
    std::string_view target_id,
    std::string_view new_message,
    int max_scan = k_line_store_default_max_scan);

// Tombstone the first matching line (no sender verification).
[[nodiscard]] XMPP_TEST_EXPORT LineStoreLookupResult
line_store_tombstone_message_by_id(struct t_gui_buffer *buffer,
                                   std::string_view target_id,
                                   std::string_view tombstone_message,
                                   std::string_view replacement_tags,
                                   int max_scan = k_line_store_default_max_scan);

// Tombstone with XEP-0424 sender verification (nick_ or occupant_id_ tags).
[[nodiscard]] XMPP_TEST_EXPORT LineStoreLookupResult
line_store_tombstone_retraction_by_id(struct t_gui_buffer *buffer,
                                      std::string_view target_id,
                                      std::string_view tombstone_message,
                                      std::string_view replacement_tags,
                                      std::string_view sender_key,
                                      std::string_view occupant_id,
                                      bool prefer_occupant_id,
                                      int max_scan = k_line_store_default_max_scan);

struct ReplyQuoteLookup {
    std::string excerpt;
    std::string quote_nick;
};

// XEP-0461: find the body line of a tagged message group and build quote context.
[[nodiscard]] XMPP_TEST_EXPORT std::optional<ReplyQuoteLookup>
line_store_lookup_reply_quote(struct t_gui_buffer *buffer,
                              std::string_view target_id,
                              int max_scan = k_line_store_default_max_scan);

// XEP-0444: replace reaction suffix on the first line matching target_id.
[[nodiscard]] XMPP_TEST_EXPORT bool line_store_apply_reactions_by_id(
    struct t_gui_buffer *buffer,
    std::string_view target_id,
    std::string_view emojis,
    int max_scan = k_line_store_default_max_scan);

}  // namespace weechat