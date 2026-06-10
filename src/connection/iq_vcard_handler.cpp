// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <memory>
#include <string>
#include <string_view>
#include <fmt/core.h>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "account.hh"
#include "channel.hh"
#include "user.hh"
#include "connection.hh"
#include "debug.hh"
#include "xmpp/node.hh"
#include "xmpp/stanza.hh"
#include "xmpp/stanza_view.hh"
#include "xmpp/iq_vcard.hh"
#include "xmpp/iq_bookmarks.hh"
#include "xmpp/xep-0054.inl"
#include "xmpp/xep-0292.inl"
#include "weechat/ui_port.hh"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "connection/iq_vcard_handler.inl"
#pragma GCC diagnostic pop