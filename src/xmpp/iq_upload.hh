// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <string>
#include <string_view>

#include "test_export.hh"
#include "stanza_view.hh"

namespace xmpp {

[[nodiscard]] XMPP_TEST_EXPORT bool is_allowed_http_upload_put_header(std::string_view name);

[[nodiscard]] XMPP_TEST_EXPORT std::string sanitize_http_header_value(std::string_view value);

[[nodiscard]] XMPP_TEST_EXPORT std::string content_type_from_upload_filename(
    std::string_view filename);

[[nodiscard]] XMPP_TEST_EXPORT std::string format_upload_slot_error_message(
    StanzaView error_elem);

}  // namespace xmpp