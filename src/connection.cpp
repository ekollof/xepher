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
#include "xmpp/xep-0054.inl"
#include "xmpp/xep-0084.inl"
#include "xmpp/xep-0172.inl"
#include "xmpp/xep-0292.inl"

extern "C" {
#include "diff/diff.h"
}

namespace {

[[nodiscard]] auto parse_omemo_device_id(const char *value) -> std::optional<std::uint32_t>
{
    if (!value || !*value)
        return std::nullopt;

    std::uint32_t parsed = 0;
    const auto *begin = value;
    const auto *end = value + std::strlen(value);
    const auto [ptr, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc {} || ptr != end || parsed == 0 || parsed > 0x7fffffffU)
        return std::nullopt;

    return parsed;
}

[[nodiscard]] auto raw_xml_trace_path(weechat::account &account) -> std::string
{
    std::shared_ptr<char> eval_path(
        weechat_string_eval_expression(
            fmt::format("${{weechat_data_dir}}/xmpp/raw_xml_{}.log", account.name).c_str(),
            NULL, NULL, NULL),
        &free);
    return eval_path ? std::string(eval_path.get()) : std::string {};
}

void append_raw_xml_trace(weechat::account &account,
                          const char *direction,
                          xmpp_stanza_t *stanza)
{
    if (!stanza)
        return;

    if (!xmpp_raw_xml_log_is_on())
        return;

    const auto path = raw_xml_trace_path(account);
    if (path.empty())
        return;

    try
    {
        std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    }
    catch (...)
    {
        return;
    }

    char *xml = nullptr;
    size_t xml_len = 0;
    xmpp_stanza_to_text(stanza, &xml, &xml_len);
    if (!xml)
        return;
    struct xmpp_string_cleanup {
        xmpp_conn_t *conn; char *ptr;
        ~xmpp_string_cleanup() { if (ptr) xmpp_free(xmpp_conn_get_context(conn), ptr); }
    } xml_guard{ account.connection, xml };

    FILE *fp = fopen(path.c_str(), "a");
    if (!fp)
        return;

    time_t now = time(NULL);
    struct tm local_tm = {0};
    localtime_r(&now, &local_tm);
    std::string timestamp = fmt::format("{:%Y-%m-%d %H:%M:%S}", local_tm);

    const char *stanza_name = xmpp_stanza_get_name(stanza);
    fprintf(fp, "[%s] %s %s\n%s\n\n",
            timestamp.c_str(),
            direction ? direction : "XML",
            stanza_name ? stanza_name : "(unknown)",
            xml);
    fclose(fp);
}

}

void weechat::connection::init()
{
    srand(time(NULL));
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

bool weechat::connection::version_handler(xmpp_stanza_t *stanza)
{
    const char *weechat_name = "weechat";
    std::unique_ptr<char, decltype(&free)> weechat_version(weechat_info_get("version", NULL), free);

    XDEBUG("Received version request from {}", xmpp_stanza_get_from(stanza));

    auto reply = libstrophe::stanza::reply(stanza)
        .set_type("result");

    auto query = libstrophe::stanza(account.context)
        .set_name("query");
    if (const char *ns = xmpp_stanza_get_ns(xmpp_stanza_get_children(stanza)); ns) {
        query.set_ns(ns);
    }

    query.add_child(libstrophe::stanza(account.context)
                    .set_name("name")
                    .add_child(libstrophe::stanza(account.context)
                               .set_text(weechat_name)));
    query.add_child(libstrophe::stanza(account.context)
                    .set_name("version")
                    .add_child(libstrophe::stanza(account.context)
                               .set_text(weechat_version.get())));

    reply.add_child(std::move(query));

    account.connection.send(reply);

    return true;
}

bool weechat::connection::time_handler(xmpp_stanza_t *stanza)
{
    XDEBUG("Received time request from {}", xmpp_stanza_get_from(stanza));

    auto reply = libstrophe::stanza::reply(stanza)
        .set_type("result");

    auto query = libstrophe::stanza(account.context)
        .set_name("time");
    if (const char *ns = xmpp_stanza_get_ns(xmpp_stanza_get_children(stanza)); ns) {
        query.set_ns(ns);
    }

    // Get current time
    time_t now = time(NULL);
    struct tm *tm_utc = gmtime(&now);
    struct tm *tm_local = localtime(&now);

    // Format UTC time as ISO 8601: YYYY-MM-DDTHH:MM:SSZ
    std::string utc_str = fmt::format("{:%Y-%m-%dT%H:%M:%SZ}", *tm_utc);
    
    // Calculate timezone offset
    long tz_offset = tm_local->tm_gmtoff;  // Offset in seconds
    int tz_hours = tz_offset / 3600;
    int tz_mins = abs((tz_offset % 3600) / 60);
    std::string tzo_str = fmt::format("{:+03d}:{:02d}", tz_hours, tz_mins);

    query.add_child(libstrophe::stanza(account.context)
                    .set_name("utc")
                    .add_child(libstrophe::stanza(account.context)
                               .set_text(utc_str.c_str())));
    query.add_child(libstrophe::stanza(account.context)
                    .set_name("tzo")
                    .add_child(libstrophe::stanza(account.context)
                               .set_text(tzo_str.c_str())));

    reply.add_child(std::move(query));

    account.connection.send(reply);

    return true;
}

#include "connection/presence_handler.inl"

// XEP-0466: Ephemeral Messages — tombstone timer callback.
// Fires N seconds after an ephemeral message is displayed.
// Finds the message in the buffer by id_ tag and replaces its text with a tombstone.
//
// Lifetime: ctx objects live in g_ephemeral_pending (a static list).  No raw
// new/delete — the list owns the storage; the callback erases the element.
struct ephemeral_tombstone_ctx {
    struct t_gui_buffer *buffer;
    std::string msg_id;
};

// RAII storage for pending ephemeral tombstones.  Elements are stable (list
// iterator / pointer invalidation rules) so it is safe to hand a raw pointer
// into the list to weechat_hook_timer.
static std::list<ephemeral_tombstone_ctx> g_ephemeral_pending;

static int ephemeral_tombstone_cb(const void *pointer, void *data, int remaining_calls)
{
    (void) data;
    (void) remaining_calls;

    if (!pointer)
        return WEECHAT_RC_OK;

    const auto *ctx = static_cast<const ephemeral_tombstone_ctx *>(pointer);
    struct t_gui_buffer *buf = ctx->buffer;
    const std::string &msg_id = ctx->msg_id;

    if (buf)
    {
        // Search buffer lines for the one tagged id_<msg_id>
        std::string id_tag = "id_" + msg_id;
        void *lines = weechat_hdata_pointer(weechat_hdata_get("buffer"), buf, "lines");
        if (lines)
        {
            void *last_line = weechat_hdata_pointer(weechat_hdata_get("lines"), lines, "last_line");
            while (last_line)
            {
                void *line_data = weechat_hdata_pointer(weechat_hdata_get("line"),
                                                        last_line, "data");
                if (line_data)
                {
                    int tags_count = weechat_hdata_integer(weechat_hdata_get("line_data"),
                                                           line_data, "tags_count");
                    bool found = false;
                    for (int n = 0; n < tags_count && !found; ++n)
                    {
                        std::string tag_key = fmt::format("{}|tags_array", n);
                        const char *tag = weechat_hdata_string(weechat_hdata_get("line_data"),
                                                               line_data, tag_key.c_str());
                        if (tag && id_tag == tag)
                            found = true;
                    }
                    if (found)
                    {
                        // Replace message text with tombstone
                        struct t_hashtable *props = weechat_hashtable_new(
                            4, WEECHAT_HASHTABLE_STRING, WEECHAT_HASHTABLE_STRING,
                            nullptr, nullptr);
                        if (props)
                        {
                            weechat_hashtable_set(props, "message",
                                "\x1b[9m[This message has disappeared]\x1b[0m");
                            weechat_hdata_update(weechat_hdata_get("line_data"), line_data, props);
                            weechat_hashtable_free(props);
                        }
                        break;
                    }
                }
                last_line = weechat_hdata_pointer(weechat_hdata_get("line"),
                                                  last_line, "prev_line");
            }
        }
    }

    // Erase from the owning list — no delete needed.
    g_ephemeral_pending.remove_if([ctx](const ephemeral_tombstone_ctx &e) {
        return &e == ctx;
    });

    return WEECHAT_RC_OK;
}

#include "connection/message_handler.inl"

#include "connection/iq_handler.inl"

#include "connection/session_lifecycle.inl"
