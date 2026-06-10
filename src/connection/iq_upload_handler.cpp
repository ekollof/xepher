// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <memory>
#include <string>
#include <vector>
#include <span>
#include <algorithm>
#include <ranges>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <curl/curl.h>
#include <fmt/core.h>
#include <fmt/chrono.h>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "weechat/runtime_port.hh"
#include "account.hh"
#include "channel.hh"
#include "connection.hh"
#include "connection/internal.hh"
#include "debug.hh"
#include "xmpp/node.hh"
#include "xmpp/stanza.hh"
#include "xmpp/stanza_view.hh"
#include "xmpp/iq_upload.hh"
#include "weechat/ui_port.hh"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "connection/iq_upload_handler.inl"
#pragma GCC diagnostic pop