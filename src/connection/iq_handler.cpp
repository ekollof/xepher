// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <memory>
#include <optional>
#include <charconv>
#include <string>
#include <vector>
#include <list>
#include <array>
#include <algorithm>
#include <ranges>
#include <span>
#include <expected>
#include <time.h>
#include <sys/stat.h>
#include <stdint.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <curl/curl.h>
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
#include "xmpp/stanza_view.hh"
#include "xmpp/iq_error.hh"
#include "xmpp/xep-0054.inl"
#include "xmpp/xep-0084.inl"
#include "xmpp/xep-0172.inl"
#include "xmpp/xep-0292.inl"

#include "connection/iq_handler.inl"
