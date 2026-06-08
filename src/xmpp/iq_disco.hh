// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "test_export.hh"
#include "stanza_view.hh"

namespace xmpp {

[[nodiscard]] XMPP_TEST_EXPORT bool is_adhoc_commands_disco_node(std::string_view node);

[[nodiscard]] XMPP_TEST_EXPORT bool is_channel_search_item_open(std::string_view open_raw);

[[nodiscard]] XMPP_TEST_EXPORT std::string normalize_channel_search_service_type(
    std::string_view service_type);

[[nodiscard]] XMPP_TEST_EXPORT std::string join_bracketed_meta(
    const std::vector<std::string> &parts);

[[nodiscard]] XMPP_TEST_EXPORT std::vector<std::string> disco_feature_vars(StanzaView query);

}  // namespace xmpp