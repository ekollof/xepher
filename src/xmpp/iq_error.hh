// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <span>
#include <string>
#include <string_view>

#include "test_export.hh"
#include "stanza_view.hh"

namespace xmpp {

// Human-readable summary of an <error/> element (condition name or <text/>).
[[nodiscard]] XMPP_TEST_EXPORT std::string iq_error_text(StanzaView error_elem);

// First matching stanza error condition under <error/>, or "unknown".
[[nodiscard]] XMPP_TEST_EXPORT std::string
iq_error_condition(StanzaView iq, std::span<const std::string_view> candidates);

}  // namespace xmpp