// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <optional>
#include <string>
#include <unordered_set>
#include <algorithm>
#include <ranges>
#include <span>
#include <expected>
#include <time.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/utsname.h>
#include <fmt/core.h>
#include <fmt/chrono.h>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "xmpp/node.hh"
#include "xmpp/stanza.hh"
#include "xmpp/atom.hh"
#include "config.hh"
#include "account.hh"
#include "user.hh"
#include "channel.hh"
#include "connection.hh"
#include "omemo.hh"
#include "pgp.hh"
#include "util.hh"
#include "avatar.hh"
#include "debug.hh"
#include "connection/internal.hh"
#include "xmpp/server_capability_map.hh"
#include "xmpp/iq_bookmarks.hh"
#include "xmpp/muc_join.hh"
#include "weechat/ui_port.hh"
#include "buffer.hh"

#include "connection/stream_management.inl"
#include "connection/optional_server_probes.inl"
#include "connection/post_sm_connect.inl"
#include "connection/connect_lifecycle.inl"
