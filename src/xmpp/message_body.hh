// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <string>
#include <string_view>

#include <strophe.h>

#include "test_export.hh"
#include "xmpp/stanza_view.hh"

namespace xmpp {

inline constexpr std::string_view k_styling_ns = "urn:xmpp:styling:0";
inline constexpr std::string_view k_markup_ns = "urn:xmpp:markup:0";

[[nodiscard]] XMPP_TEST_EXPORT bool stanza_has_unstyled_hint(StanzaView msg);
[[nodiscard]] XMPP_TEST_EXPORT bool stanza_has_markup(StanzaView msg);

// Apply XEP-0394 (if present) then XEP-0393 unless <unstyled/> is set.
// Returns plain copy of text when styling is skipped or text is empty.
[[nodiscard]] XMPP_TEST_EXPORT std::string format_inbound_message_body(
    xmpp_stanza_t *stanza, std::string_view text);

}  // namespace xmpp