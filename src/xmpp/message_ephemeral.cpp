// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "message_ephemeral.hh"

#include <fmt/core.h>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "weechat/runtime_port.hh"
#include "util.hh"

namespace xmpp {

std::optional<std::int64_t> parse_ephemeral_timer(StanzaView msg)
{
    const StanzaView ephemeral = msg.child("ephemeral", k_ephemeral_ns);
    if (!ephemeral.valid())
        return std::nullopt;

    const std::string timer_attr = ephemeral.attr_string("timer");
    if (auto value = parse_int64(timer_attr); value && *value > 0)
        return *value;
    return std::nullopt;
}

std::string format_ephemeral_display_prefix(std::int64_t timer_secs)
{
    return std::string(weechat::RuntimePort::default_runtime().color("magenta"))
        + "[⏱ " + std::to_string(timer_secs) + "s] "
        + std::string(weechat::RuntimePort::default_runtime().color("resetcolor"));
}

bool should_schedule_ephemeral_tombstone(std::int64_t timer_secs, std::string_view stable_id)
{
    return timer_secs > 0 && !stable_id.empty();
}

}  // namespace xmpp