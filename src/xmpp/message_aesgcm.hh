// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <optional>
#include <string_view>

#include "test_export.hh"
#include "xmpp/message_media.hh"

namespace xmpp {

// XEP-0454: parse aesgcm:// URL (+ optional data:image/jpeg thumbnail line)
// from decrypted OMEMO message body into ESFS-compatible download metadata.
[[nodiscard]] XMPP_TEST_EXPORT std::optional<EncryptedMediaShare>
parse_aesgcm_body_share(std::string_view body);

}  // namespace xmpp