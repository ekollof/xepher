// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <ctime>
#include <string_view>

#include "test_export.hh"
#include "weechat/runtime_port.hh"
#include "node.hh"
#include "stanza_view.hh"

namespace xmpp {

// Pure XEP-0092 version reply (caller builds + sends).
[[nodiscard]] XMPP_TEST_EXPORT stanza::iq
handle_version_iq(StanzaView request, weechat::RuntimePort &runtime, std::string_view local_jid);

// Pure XEP-0202 time reply (caller builds + sends).
[[nodiscard]] XMPP_TEST_EXPORT stanza::iq
handle_time_iq(StanzaView request, std::string_view local_jid, std::time_t now = std::time(nullptr));

// Pure XEP-0199 ping reply (caller builds + sends).
[[nodiscard]] XMPP_TEST_EXPORT stanza::iq
handle_ping_iq(StanzaView request, std::string_view local_jid);

}  // namespace xmpp