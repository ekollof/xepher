// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>
#include <ctime>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include "test_export.hh"
#include "xmpp/stanza_view.hh"

// Transparent hash/equality for unordered_(map|set) lookup by std::string_view.
struct transparent_string_hash {
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(std::string_view sv) const noexcept
    {
        return std::hash<std::string_view>{}(sv);
    }
    [[nodiscard]] std::size_t operator()(const std::string &s) const noexcept
    {
        return std::hash<std::string_view>{}(s);
    }
    [[nodiscard]] std::size_t operator()(const char *s) const noexcept
    {
        return std::hash<std::string_view>{}(s);
    }
};

XMPP_TEST_EXPORT int char_cmp(const void *p1, const void *p2);

XMPP_TEST_EXPORT std::string unescape(std::string_view str);

[[nodiscard]] XMPP_TEST_EXPORT std::expected<std::uint32_t, std::string>
parse_uint32(std::string_view value);

// XEP-0198 §5: parse <enabled/> location (RFC 6120 §4.9.3.19 host:port or [ipv6]:port).
struct sm_reconnect_endpoint {
    std::string host;
    std::uint16_t port = 0;
};

[[nodiscard]] XMPP_TEST_EXPORT std::optional<sm_reconnect_endpoint>
parse_sm_location(std::string_view location);

[[nodiscard]] XMPP_TEST_EXPORT std::expected<std::int64_t, std::string>
parse_int64(std::string_view value);

// Format a local-time timestamp for MAM banners (YYYY-MM-DD HH:MM).
[[nodiscard]] XMPP_TEST_EXPORT std::string format_local_timestamp(std::time_t t);

// Format a UTC timestamp for MAM filters and XEP-0319 idle (YYYY-MM-DDTHH:MMZ).
[[nodiscard]] XMPP_TEST_EXPORT std::string format_utc_timestamp(std::time_t t);

// XEP-0393: Message Styling
XMPP_TEST_EXPORT std::string apply_xep393_styling(std::string_view text);

// XEP-0394: Message Markup (receive-only)
// Returns a WeeChat-colour-coded string derived from `plain_text` using the
// <markup xmlns='urn:xmpp:markup:0'> child of `stanza`, or an empty string
// if no <markup> element is found.
[[nodiscard]] XMPP_TEST_EXPORT std::string apply_xep394_markup(
    xmpp::StanzaView stanza,
    std::string_view plain_text);

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
