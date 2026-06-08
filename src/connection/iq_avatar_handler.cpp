// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <string_view>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "account.hh"
#include "avatar.hh"
#include "connection.hh"
#include "debug.hh"
#include "xmpp/node.hh"
#include "xmpp/message_pep.hh"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "connection/iq_avatar_handler.inl"
#pragma GCC diagnostic pop