// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <string>
#include <string_view>

struct t_gui_buffer;

#include "../test_export.hh"

namespace weechat {

inline constexpr std::string_view k_glyph_pending = " ⌛";
inline constexpr std::string_view k_glyph_delivered = " ✓";
inline constexpr std::string_view k_glyph_seen = " ✓✓";

// Strip a trailing delivery-status glyph from a buffer line message.
[[nodiscard]] XMPP_TEST_EXPORT std::string strip_status_glyph_suffix(std::string message);

// Walk buffer lines backwards; update the line tagged id_<acked_id> with new_glyph.
// Returns true when a matching line was found and updated.
[[nodiscard]] bool line_store_update_line_glyph_by_tag(struct t_gui_buffer *buffer,
                                                         std::string_view acked_id,
                                                         std::string_view new_glyph);

}  // namespace weechat