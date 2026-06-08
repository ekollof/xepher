// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <string>
#include <string_view>
#include <time.h>
#include <fmt/core.h>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "account.hh"
#include "channel.hh"
#include "connection.hh"
#include "xmpp/node.hh"
#include "xmpp/stanza.hh"
#include "xmpp/stanza_view.hh"
#include "xmpp/iq_handlers.hh"
#include "xmpp/iq_ping.hh"
#include "xmpp/xep-0045.inl"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "connection/iq_ping_handler.inl"
#pragma GCC diagnostic pop