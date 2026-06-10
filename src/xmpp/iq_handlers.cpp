// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "iq_handlers.hh"

#include <cmath>
#include <string>
#include <fmt/core.h>

#include "util.hh"

namespace xmpp {

stanza::iq handle_version_iq(StanzaView request, weechat::RuntimePort &runtime, std::string_view local_jid)
{
    const std::string from = request.from().transform([](std::string_view v) {
        return std::string(v);
    }).value_or("");
    const std::string id = request.id().transform([](std::string_view v) {
        return std::string(v);
    }).value_or("");

    return stanza::iq()
        .type("result")
        .to(from)
        .from(std::string(local_jid))
        .id(id)
        .version_query(stanza::xep0092::query()
            .name("weechat")
            .version(runtime.version_string()));
}

stanza::iq handle_time_iq(StanzaView request, std::string_view local_jid, std::time_t now)
{
    const std::string from = request.from().transform([](std::string_view v) {
        return std::string(v);
    }).value_or("");
    const std::string id = request.id().transform([](std::string_view v) {
        return std::string(v);
    }).value_or("");

    std::tm tm_local {};
    localtime_r(&now, &tm_local);

    const std::string utc_str = format_utc_timestamp(now);
    const long tz_offset = tm_local.tm_gmtoff;
    const int tz_hours = static_cast<int>(tz_offset / 3600);
    const int tz_mins = static_cast<int>(std::abs((tz_offset % 3600) / 60));
    const std::string tzo_str = fmt::format("{:+03d}:{:02d}", tz_hours, tz_mins);

    return stanza::iq()
        .type("result")
        .to(from)
        .from(std::string(local_jid))
        .id(id)
        .time_element(stanza::xep0202::time()
            .utc(utc_str)
            .tzo(tzo_str));
}

stanza::iq handle_ping_iq(StanzaView request, std::string_view local_jid)
{
    return stanza::iq()
        .type("result")
        .id(request.attr_string("id"))
        .to(request.attr_string("from"))
        .from(std::string(local_jid));
}

}  // namespace xmpp