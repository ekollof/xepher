// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "test_export.hh"
#include "xmpp/stanza_view.hh"

namespace xmpp {

struct ParsedJid {
    std::string full;
    std::string bare;
    std::string resource;
};

struct PresenceCaps {
    std::string node;
    std::string verification;
};

struct MucUserItem {
    std::optional<std::string> affiliation;
    std::optional<std::string> role;
    std::optional<std::string> nick;
    std::optional<std::string> real_jid;
    std::string reason;
};

struct MucPresenceExtension {
    std::vector<int> statuses;
    std::vector<MucUserItem> items;
};

struct PresenceError {
    std::string reason;
    std::optional<std::string> description;
};

struct ParsedPresence {
    std::optional<ParsedJid> from;
    std::optional<ParsedJid> to;
    std::optional<std::string> type;
    std::optional<std::string> show;
    std::optional<std::string> status;
    std::optional<std::chrono::system_clock::time_point> idle_since;
    std::optional<std::string> signature;
    std::optional<PresenceCaps> caps;
    bool has_muc = false;
    std::optional<MucPresenceExtension> muc_user;
    std::optional<PresenceError> error;
};

[[nodiscard]] XMPP_TEST_EXPORT ParsedJid parse_jid_parts(std::string_view full_jid);
[[nodiscard]] XMPP_TEST_EXPORT std::string muc_presence_error_reason(StanzaView error_elem);
[[nodiscard]] XMPP_TEST_EXPORT ParsedPresence parse_presence(StanzaView view);

}  // namespace xmpp