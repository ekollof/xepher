// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <string>

namespace weechat {

// XEP-0392: Consistent Color Generation
// Generate a consistent WeeChat color code from a string (JID or nickname)
std::string consistent_color(const std::string& input);

} // namespace weechat
