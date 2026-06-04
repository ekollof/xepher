// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <string>
#include <strophe.h>
#include "test_export.hh"

XMPP_TEST_EXPORT int char_cmp(const void *p1, const void *p2);

XMPP_TEST_EXPORT std::string unescape(const std::string& str);

// XEP-0393: Message Styling
XMPP_TEST_EXPORT std::string apply_xep393_styling(std::string_view text);

// XEP-0394: Message Markup (receive-only)
// Returns a WeeChat-colour-coded string derived from `plain_text` using the
// <markup xmlns='urn:xmpp:markup:0'> child of `stanza`, or an empty string
// if no <markup> element is found.
std::string apply_xep394_markup(xmpp_stanza_t *stanza, std::string_view plain_text);

// Markdown renderer for Atom feed plain-text content.
// Converts a barebones subset of Markdown to WeeChat colour/attribute codes.
// Applied to <content type='text'> (or no type) Atom entries.
std::string apply_markdown_to_weechat(std::string_view text);

// Returns true if `mime` is an image type (starts with "image/").
XMPP_TEST_EXPORT bool is_image_mime_type(std::string_view mime);

// Read image pixel dimensions from PNG/JPEG file headers.
// Returns {width, height} or {0, 0} if unsupported or unreadable.
XMPP_TEST_EXPORT std::pair<size_t, size_t> read_image_dimensions(const char *path);

// Compute icat -columns/-rows from pixel dimensions.
// Uses default columns=40 and maintains aspect ratio.
// Returns " -columns C -rows R" string (with leading space) or empty if no dims.
std::string icat_dimension_args(size_t pixel_width, size_t pixel_height);
