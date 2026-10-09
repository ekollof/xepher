// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "xmpp/data_form.hh"
#include "test_export.hh"

namespace xmpp {
// A null form fetches configuration; cancellation never includes answers.
[[nodiscard]] XMPP_TEST_EXPORT auto make_room_config_query(
    const data_form *form = nullptr, bool cancel = false)
    -> std::expected<stanza::spec, std::string>;
}
