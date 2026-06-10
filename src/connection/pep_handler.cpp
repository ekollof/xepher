// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <string>
#include <string_view>
#include <vector>
#include <fmt/core.h>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "util.hh"
#include "account.hh"
#include "user.hh"
#include "channel.hh"
#include "connection.hh"
#include "avatar.hh"
#include "debug.hh"
#include "xmpp/node.hh"
#include "xmpp/stanza.hh"
#include "connection/internal.hh"
#include "xmpp/atom.hh"
#include "xmpp/message_pep.hh"
#include "xmpp/message_pep_feed.hh"
#include "xmpp/iq_bookmarks.hh"
#include "xmpp/xep-0060.inl"
#include "weechat/ui_port.hh"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "connection/pep_handler.inl"
#pragma GCC diagnostic pop