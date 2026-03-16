// This->Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <optional>
#include <charconv>
#include <thread>
#include <filesystem>
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
#include "config.hh"
#include "account.hh"
#include "user.hh"
#include "channel.hh"
#include "connection.hh"
#include "omemo.hh"
#include "pgp.hh"
#include "util.hh"
#include "avatar.hh"
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
    char timestamp[32] = {0};
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local_tm);

    const char *stanza_name = xmpp_stanza_get_name(stanza);
    fprintf(fp, "[%s] %s %s\n%s\n\n",
            timestamp,
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

    weechat_printf(NULL, "Received version request from %s", xmpp_stanza_get_from(stanza));

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
    weechat_printf(NULL, "Received time request from %s", xmpp_stanza_get_from(stanza));

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
    char utc_str[32];
    strftime(utc_str, sizeof(utc_str), "%Y-%m-%dT%H:%M:%SZ", tm_utc);
    
    // Calculate timezone offset
    long tz_offset = tm_local->tm_gmtoff;  // Offset in seconds
    int tz_hours = tz_offset / 3600;
    int tz_mins = abs((tz_offset % 3600) / 60);
    char tzo_str[16];
    snprintf(tzo_str, sizeof(tzo_str), "%+03d:%02d", tz_hours, tz_mins);

    query.add_child(libstrophe::stanza(account.context)
                    .set_name("utc")
                    .add_child(libstrophe::stanza(account.context)
                               .set_text(utc_str)));
    query.add_child(libstrophe::stanza(account.context)
                    .set_name("tzo")
                    .add_child(libstrophe::stanza(account.context)
                               .set_text(tzo_str)));

    reply.add_child(std::move(query));

    account.connection.send(reply);

    return true;
}

#include "connection/presence_handler.inl"

#include "connection/message_handler.inl"

#include "connection/iq_handler.inl"

#include "connection/session_lifecycle.inl"
