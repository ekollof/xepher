// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
#pragma once

#include <expected>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "xmpp/node.hh"
#include "xmpp/stanza_view.hh"
#include "test_export.hh"

namespace xmpp {
[[nodiscard]] XMPP_TEST_EXPORT auto parse_adhoc_arguments(std::string_view input)
    -> std::expected<std::vector<std::string>, std::string>;

struct AdhocSession {
    bool has_form{false};
    std::string default_action;
    std::vector<std::string> actions;
    std::map<std::string, std::vector<std::string>> hidden_fields;
};

[[nodiscard]] XMPP_TEST_EXPORT auto parse_adhoc_session(StanzaView command)
    -> std::expected<AdhocSession, std::string>;

// An empty action requests the session's default, or execute for a new command.
[[nodiscard]] XMPP_TEST_EXPORT auto make_adhoc_command(
    std::string_view node, std::string_view session_id, std::string_view action,
    std::span<const std::string_view> fields, const AdhocSession *session = nullptr)
    -> std::expected<stanza::spec, std::string>;
}
