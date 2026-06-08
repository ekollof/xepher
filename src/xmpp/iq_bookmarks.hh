// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <string>
#include <string_view>

#include "test_export.hh"

namespace xmpp {

[[nodiscard]] XMPP_TEST_EXPORT bool is_bookmark_autojoin_true(std::string_view autojoin_attr);

[[nodiscard]] XMPP_TEST_EXPORT bool is_biboumi_gateway_room(std::string_view jid);

[[nodiscard]] XMPP_TEST_EXPORT std::string bookmark_enter_command(
    std::string_view jid,
    std::string_view nick);

}  // namespace xmpp