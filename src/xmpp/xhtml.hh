// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

// XEP-0071: XHTML-IM — shared rendering utilities.
//
// css_color_to_weechat  — map a CSS color value to a WeeChat color name.
// css_style_to_weechat  — parse an inline CSS style attr → (open, close) codes.
// xhtml_to_weechat      — recursively render an XHTML stanza tree to a
//                         WeeChat-formatted string (colors, bold, italic, …).
// html_strip_to_plain   — strip HTML tags from a raw HTML *string* (not a
//                         stanza), decode basic entities, return plain text.
//                         Used for Atom <content type='html'>.

#include <string>
#include <string_view>
#include <utility>

#include "test_export.hh"
#include "xmpp/stanza_view.hh"

std::string css_color_to_weechat(std::string_view css);

std::pair<std::string, std::string> css_style_to_weechat(const char *style);

[[nodiscard]] XMPP_TEST_EXPORT std::string xhtml_to_weechat(
    xmpp::StanzaView stanza,
    bool in_blockquote = false);

// Strip HTML tags from a plain string and decode basic HTML entities.
// Returns a plain-text approximation suitable for terminal display.
[[nodiscard]] XMPP_TEST_EXPORT std::string html_strip_to_plain(std::string_view html);
