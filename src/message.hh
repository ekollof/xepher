// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <cstddef>
#include <optional>
#include "test_export.hh"

#define MESSAGE_MAX_LENGTH 40000

namespace weechat { class account; }

XMPP_TEST_EXPORT void message__htmldecode(char *dest, const char *src, std::size_t n);

std::string message__decode(weechat::account *account,
                            std::string_view text);

// Replace text emoticons (:-), <3, …) and :shortcode: aliases (GitHub/gemoji) with Unicode emoji.
XMPP_TEST_EXPORT std::string replace_emoticons(std::string_view text);

// Resolve :shortcode: to Unicode emoji; returns input unchanged when not a known shortcode.
[[nodiscard]] XMPP_TEST_EXPORT std::string resolve_emoji_shortcode(std::string_view input);

// Extract shortcode prefix for tab completion from buffer input.
// nullopt when the cursor token is not a :shortcode: (avoids polluting normal completion).
// Engaged empty prefix after "/react " yields the default shortcode list.
[[nodiscard]] XMPP_TEST_EXPORT std::optional<std::string_view>
emoji_shortcode_completion_prefix(std::string_view line);

// :alias: strings matching prefix (empty = first N sorted).
[[nodiscard]] XMPP_TEST_EXPORT std::vector<std::string>
emoji_shortcode_completions(std::string_view prefix, std::size_t limit = 64);
