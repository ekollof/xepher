// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "test_export.hh"
#include "stanza_view.hh"

namespace xmpp {

struct vcard_temp_fields {
    std::optional<std::string> fn;
    std::optional<std::string> nickname;
    std::optional<std::string> email;
    std::optional<std::string> url;
    std::optional<std::string> desc;
    std::optional<std::string> org;
    std::optional<std::string> title;
    std::optional<std::string> tel;
    std::optional<std::string> bday;
    std::optional<std::string> note;
};

[[nodiscard]] XMPP_TEST_EXPORT vcard_temp_fields vcard_fields_from_stanza(StanzaView vcard);

[[nodiscard]] XMPP_TEST_EXPORT bool apply_vcard_set_field_override(
    vcard_temp_fields &fields,
    std::string_view field,
    std::string_view value);

[[nodiscard]] XMPP_TEST_EXPORT std::string format_vcard_temp_adr(StanzaView adr);

[[nodiscard]] XMPP_TEST_EXPORT bool is_vcard4_pubsub_node(std::string_view node);

}  // namespace xmpp