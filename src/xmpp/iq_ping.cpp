// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "iq_ping.hh"

namespace xmpp {

bool is_ping_get_iq(StanzaView iq)
{
    return iq.type() == "get" && iq.child("ping", k_ping_ns).valid();
}

long compute_ping_rtt_ms(const std::time_t start, const std::time_t now)
{
    return static_cast<long>((now - start) * 1000);
}

std::optional<MucPingFrom> parse_muc_ping_from(const std::string_view from_full)
{
    const auto slash = from_full.find('/');
    if (slash == std::string_view::npos || slash + 1 >= from_full.size())
        return std::nullopt;

    MucPingFrom parsed;
    parsed.room_jid.assign(from_full.substr(0, slash));
    parsed.resource.assign(from_full.substr(slash + 1));
    return parsed;
}

bool is_muc_self_ping(
    const MucPingFrom &from,
    const std::string_view account_nick,
    const std::function<bool(std::string_view room_jid)> &has_room_channel)
{
    return !from.resource.empty()
        && from.resource == account_nick
        && has_room_channel(from.room_jid);
}

MucSelfPingErrorOutcome classify_muc_self_ping_error(const StanzaView error_elem)
{
    if (!error_elem.valid())
        return MucSelfPingErrorOutcome::not_joined;

    const auto has_cond = [&](const std::string_view name) {
        return error_elem.child(name, k_ietf_stanza_ns).valid();
    };

    if (has_cond("service-unavailable")
        || has_cond("feature-not-implemented")
        || has_cond("item-not-found"))
        return MucSelfPingErrorOutcome::still_joined;

    if (has_cond("remote-server-not-found") || has_cond("remote-server-timeout"))
        return MucSelfPingErrorOutcome::ambiguous;

    return MucSelfPingErrorOutcome::not_joined;
}

}  // namespace xmpp