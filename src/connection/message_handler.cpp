// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <optional>
#include <charconv>
#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>
#include <list>
#include <array>
#include <algorithm>
#include <ranges>
#include <span>
#include <expected>
#include <sstream>
#include <iomanip>
#include <time.h>
#include <stdint.h>
#include <sys/utsname.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <fmt/core.h>
#include <fmt/chrono.h>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "color.hh"
#include "xmpp/node.hh"
#include "xmpp/stanza.hh"
#include "xmpp/atom.hh"
#include "config.hh"
#include "account.hh"
#include "user.hh"
#include "channel.hh"
#include "buffer.hh"
#include "connection.hh"
#include "omemo.hh"
#include "pgp.hh"
#include "util.hh"
#include "avatar.hh"
#include "debug.hh"
#include "message.hh"
#include "connection/internal.hh"
#include "xmpp/chat_state.hh"
#include "xmpp/message_ack.hh"
#include "xmpp/message_body.hh"
#include "xmpp/message_forward.hh"
#include "xmpp/message_media.hh"
#include "xmpp/message_omemo.hh"
#include "xmpp/message_invite.hh"
#include "xmpp/message_ephemeral.hh"
#include "xmpp/message_spoiler.hh"
#include "xmpp/message_fallback.hh"
#include "xmpp/message_line_tag.hh"
#include "xmpp/message_correct.hh"
#include "xmpp/message_retract.hh"
#include "xmpp/message_reactions.hh"
#include "xmpp/message_reply.hh"
#include "xmpp/stanza_view.hh"
#include "weechat/line_store.hh"
#include "xmpp/xhtml.hh"
#include "xmpp/xep-0054.inl"
#include "xmpp/xep-0084.inl"
#include "xmpp/xep-0172.inl"
#include "xmpp/xep-0292.inl"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "connection/message_handler.inl"
#pragma GCC diagnostic pop
