// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <string>
#include <string_view>

#include "test_export.hh"
#include "stanza_view.hh"

namespace xmpp {

[[nodiscard]] XMPP_TEST_EXPORT bool is_mam_fin_bool_attr_true(std::string_view value);

[[nodiscard]] XMPP_TEST_EXPORT std::string mam_fin_rsm_last(StanzaView fin);

[[nodiscard]] XMPP_TEST_EXPORT bool iq_has_item_not_found_error(StanzaView iq);

}  // namespace xmpp