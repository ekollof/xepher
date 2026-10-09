// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "xmpp/data_form.hh"

namespace xmpp {
[[nodiscard]] XMPP_TEST_EXPORT auto registration_form(StanzaView query,
    std::string_view username, std::string_view password) -> std::expected<data_form, std::string>;
[[nodiscard]] XMPP_TEST_EXPORT auto registration_submission(const data_form &form)
    -> std::expected<stanza::spec, std::string>;
}
