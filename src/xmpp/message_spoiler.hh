// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "test_export.hh"
#include "xmpp/stanza_view.hh"

namespace xmpp {

inline constexpr std::string_view k_spoiler_ns = "urn:xmpp:spoiler:0";

[[nodiscard]] XMPP_TEST_EXPORT std::optional<std::string>
parse_spoiler_hint(StanzaView msg);

[[nodiscard]] XMPP_TEST_EXPORT std::string
format_spoiler_display_prefix(std::optional<std::string_view> hint);

}  // namespace xmpp