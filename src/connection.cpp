// This->Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <optional>
#include <charconv>
#include <thread>
#include <filesystem>
#include <array>
#include <list>
#include <ranges>
#include <time.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <curl/curl.h>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include <openssl/rand.h>
#include <fmt/core.h>
#include <fmt/chrono.h>
#include <libxml/uri.h>
#include <utility>
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

void weechat::connection::init()
{
    srand(time(nullptr));
    libstrophe::initialize();
}

void weechat::connection::send(xmpp_stanza_t *stanza)
{
    append_raw_xml_trace(account, "SEND", stanza);

    // Increment outbound counter for actual stanzas (not SM elements)
    const char *name = xmpp_stanza_get_name(stanza);
    if (name && account.sm_enabled)
    {
        std::string stanza_name(name);
        if (stanza_name == "message" || stanza_name == "presence" || stanza_name == "iq")
        {
            account.sm_h_outbound++;
            // Keep a copy in the retransmit queue (XEP-0198 §5)
            xmpp_stanza_t *copy = xmpp_stanza_copy(stanza);
            account.sm_outqueue.emplace_back(
                account.sm_h_outbound,
                std::shared_ptr<xmpp_stanza_t>(copy, xmpp_stanza_release));
        }
    }
    m_conn.send(stanza);
}

void weechat::connection::send_threadsafe(xmpp_stanza_t *stanza)
{
    // Bypass non-thread-safe SM counter and raw XML trace.
    // xmpp_send() is mutex-protected in libstrophe, making it
    // safe to call from upload worker threads while the main
    // thread is blocked (e.g. Python hook deadlocks).
    m_conn.send(stanza);
}

bool weechat::connection::version_handler(xmpp_stanza_t *stanza)
{
    const char *weechat_name = "weechat";
    std::unique_ptr<char, decltype(&free)> weechat_version(weechat_info_get("version", nullptr), free);

    XDEBUG("Received version request from {}", xmpp_stanza_get_from(stanza));

    auto reply = stanza::iq()
        .type("result")
        .to(xmpp_stanza_get_from(stanza))
        .from(account.jid().data())
        .id(xmpp_stanza_get_id(stanza))
        .version_query(stanza::xep0092::query()
            .name(weechat_name)
            .version(weechat_version.get()));

    account.connection.send(reply.build(account.context).get());

    return true;
}

bool weechat::connection::time_handler(xmpp_stanza_t *stanza)
{
    XDEBUG("Received time request from {}", xmpp_stanza_get_from(stanza));

    time_t now = time(nullptr);
    struct tm *tm_utc = gmtime(&now);
    struct tm *tm_local = localtime(&now);

    std::string utc_str = fmt::format("{:%Y-%m-%dT%H:%M:%SZ}", *tm_utc);
    long tz_offset = tm_local->tm_gmtoff;
    int tz_hours = tz_offset / 3600;
    int tz_mins = abs((tz_offset % 3600) / 60);
    std::string tzo_str = fmt::format("{:+03d}:{:02d}", tz_hours, tz_mins);

    auto reply = stanza::iq()
        .type("result")
        .to(xmpp_stanza_get_from(stanza))
        .from(account.jid().data())
        .id(xmpp_stanza_get_id(stanza))
        .time_element(stanza::xep0202::time()
            .utc(utc_str)
            .tzo(tzo_str));

    account.connection.send(reply.build(account.context).get());

    return true;
}


