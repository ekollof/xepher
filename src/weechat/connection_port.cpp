// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "weechat/connection_port.hh"

#include <string>
#include <string_view>

#include "fmt/core.h"

namespace weechat {

namespace {

[[nodiscard]] std::string bare_from_bound(std::string_view full)
{
    const auto slash = full.find('/');
    return slash == std::string_view::npos
        ? std::string(full)
        : std::string(full.substr(0, slash));
}

[[nodiscard]] std::string bound_jid_or_empty(xmpp_conn_t *conn)
{
    if (!conn || !xmpp_conn_is_connected(conn))
        return {};
    const char *bound = xmpp_conn_get_bound_jid(conn);
    if (!bound || bound[0] == '\0')
        return {};
    return std::string(bound);
}

}  // namespace

LibstropheConnectionPort::LibstropheConnectionPort(xmpp_conn_t *conn, std::string &bare_cache,
                                                   std::string_view configured_jid)
    : conn_(conn), bare_cache_(&bare_cache), configured_jid_(configured_jid)
{
}

bool LibstropheConnectionPort::is_connected() const
{
    return conn_ && xmpp_conn_is_connected(conn_);
}

std::string LibstropheConnectionPort::bound_jid_bare()
{
    if (!bare_cache_->empty())
        return *bare_cache_;

    const std::string full = bound_jid_or_empty(conn_);
    if (!full.empty())
        return bare_from_bound(full);

    return std::string(configured_jid_);
}

std::string LibstropheConnectionPort::bound_jid_full()
{
    const std::string full = bound_jid_or_empty(conn_);
    if (!full.empty())
        return full;

    // Build user@domain/weechat from the configured JID when not yet bound.
    const std::string opt(configured_jid_);
    const auto at = opt.find('@');
    const std::string node = at == std::string::npos ? opt : opt.substr(0, at);
    std::string domain = at == std::string::npos ? opt : opt.substr(at + 1);
    const auto slash = domain.find('/');
    if (slash != std::string::npos)
        domain = domain.substr(0, slash);
    return fmt::format("{}@{}/{}", node, domain, "weechat");
}

void LibstropheConnectionPort::refresh_bare_cache()
{
    const std::string full = bound_jid_or_empty(conn_);
    if (!full.empty())
        *bare_cache_ = bare_from_bound(full);
}

void LibstropheConnectionPort::clear_bare_cache()
{
    bare_cache_->clear();
}

}  // namespace weechat