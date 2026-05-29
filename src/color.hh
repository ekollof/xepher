// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <string>
#include "test_export.hh"

namespace weechat {

// XEP-0392: Consistent Color Generation
// Generate a consistent WeeChat color code from a string (JID or nickname)
XMPP_TEST_EXPORT std::string consistent_color(const std::string& input);

XMPP_TEST_EXPORT std::string angle_to_weechat_color(double angle);

// Safe C++23 WeeChat color retriever (prevents nullptr crashes on FreeBSD)
XMPP_TEST_EXPORT std::string xmpp_color(std::string_view name);

} // namespace weechat
