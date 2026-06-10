// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <string>
#include <string_view>
#include <fmt/core.h>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "account.hh"
#include "channel.hh"
#include "connection.hh"
#include "debug.hh"
#include "omemo.hh"
#include "xmpp/node.hh"
#include "xmpp/stanza.hh"
#include "xmpp/xep-0060.inl"

#include "connection/internal.hh"
#include "weechat/ui_port.hh"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "connection/iq_omemo_pubsub_handler.inl"
#pragma GCC diagnostic pop