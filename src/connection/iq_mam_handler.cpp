// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <memory>
#include <optional>
#include <string>
#include <algorithm>
#include <ranges>
#include <time.h>
#include <fmt/core.h>
#include <fmt/chrono.h>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "weechat/runtime_port.hh"
#include "config.hh"
#include "account.hh"
#include "channel.hh"
#include "connection.hh"
#include "omemo.hh"
#include "util.hh"
#include "xmpp/node.hh"
#include "xmpp/stanza.hh"
#include "xmpp/stanza_view.hh"
#include "xmpp/iq_mam.hh"
#include "weechat/ui_port.hh"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "connection/iq_mam_handler.inl"
#pragma GCC diagnostic pop