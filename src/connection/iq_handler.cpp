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
#include "xmpp/xep-0054.inl"
#include "xmpp/xep-0084.inl"
#include "xmpp/xep-0172.inl"
#include "xmpp/xep-0292.inl"

namespace {

[[nodiscard]] auto iq_error_text(xmpp_stanza_t *error_elem) -> std::string
{
    if (!error_elem)
        return "unknown error";
    if (auto *text_el = xmpp_stanza_get_child_by_name(error_elem, "text"))
    {
        const auto t = stanza_element_text(text_el);
        if (!t.empty())
            return t;
    }
    for (auto *child = xmpp_stanza_get_children(error_elem);
         child; child = xmpp_stanza_get_next(child))
    {
        const char *name = xmpp_stanza_get_name(child);
        if (name && std::string_view(name) != "text")
            return name;
    }
    return "unknown error";
}

} // namespace

#include "connection/iq_handler.inl"
