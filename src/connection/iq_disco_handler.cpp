// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <algorithm>
#include <ranges>
#include <span>
#include <openssl/evp.h>
#include <fmt/core.h>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "config.hh"
#include "account.hh"
#include "channel.hh"
#include "connection.hh"
#include "omemo.hh"
#include "util.hh"
#include "debug.hh"
#include "connection/internal.hh"
#include "xmpp/node.hh"
#include "xmpp/stanza.hh"
#include "xmpp/stanza_view.hh"
#include "xmpp/iq_error.hh"
#include "xmpp/iq_pubsub_feed.hh"
#include "xmpp/iq_disco.hh"
#include "xmpp/iq_caps.hh"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "connection/iq_disco_handler.inl"
#pragma GCC diagnostic pop