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
#include "xmpp/stanza_view.hh"
#include "xmpp/iq_handlers.hh"
#include "weechat/runtime_port.hh"
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

    const char *name = xmpp_stanza_get_name(stanza);
    if (name)
    {
        const std::string_view stanza_name(name);
        if (stanza_name == "enable" || stanza_name == "resume"
            || stanza_name == "a" || stanza_name == "r")
        {
            account.last_top_level_ext_sent = last_top_level_ext::sm;
        }
        else if (stanza_name == "active" || stanza_name == "inactive")
        {
            const char *ns = xmpp_stanza_get_ns(stanza);
            if (ns && std::string_view(ns) == "urn:xmpp:csi:0")
                account.last_top_level_ext_sent = last_top_level_ext::csi;
        }
        const bool is_app_stanza = stanza_name == "message"
            || stanza_name == "presence"
            || stanza_name == "iq";

        // XEP-0198 §3.1: defer application stanzas until SM negotiation completes.
        if (account.sm_awaiting_negotiation && is_app_stanza)
        {
            xmpp_stanza_t *copy = xmpp_stanza_copy(stanza);
            account.sm_pending_replay.push_back({
                0,
                time(nullptr),
                std::shared_ptr<xmpp_stanza_t>(copy, xmpp_stanza_release),
            });
            return;
        }

        if (is_app_stanza && account.sm_enabled)
        {
            sm_increment_handled_count(account.sm_h_outbound);
            // Keep a copy in the retransmit queue (XEP-0198 §5)
            xmpp_stanza_t *copy = xmpp_stanza_copy(stanza);
            account.sm_outqueue.push_back({
                account.sm_h_outbound,
                time(nullptr),
                std::shared_ptr<xmpp_stanza_t>(copy, xmpp_stanza_release),
            });
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
    const ::xmpp::StanzaView view(stanza);
    XDEBUG("Received version request from {}", view.from().value_or(""));

    auto reply = ::xmpp::handle_version_iq(
        view, weechat::RuntimePort::default_runtime(), account.jid().data());
    account.connection.send(reply.build(account.context).get());

    return true;
}

bool weechat::connection::time_handler(xmpp_stanza_t *stanza)
{
    const ::xmpp::StanzaView view(stanza);
    XDEBUG("Received time request from {}", view.from().value_or(""));

    auto reply = ::xmpp::handle_time_iq(view, account.jid().data());
    account.connection.send(reply.build(account.context).get());

    return true;
}


