// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <string>
#include <vector>
#include <ranges>
#include <time.h>
#include <fmt/core.h>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "config.hh"
#include "weechat/runtime_port.hh"
#include "account.hh"
#include "channel.hh"
#include "connection.hh"
#include "util.hh"
#include "xmpp/atom.hh"
#include "xmpp/node.hh"
#include "xmpp/stanza.hh"
#include "xmpp/message_pep_feed.hh"
#include "xmpp/iq_pubsub_feed.hh"
#include "xmpp/xep-0059.inl"
#include "xmpp/xep-0060.inl"
#include "weechat/ui_port.hh"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "connection/iq_pubsub_feed_handler.inl"
#pragma GCC diagnostic pop