// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <algorithm>
#include <ranges>
#include <span>
#include <unordered_map>

#include <fmt/format.h>

#include "account.hh"
#include "weechat/ui_port.hh"
#include "xmpp/iq_disco.hh"
#include "xmpp/server_capability_map.hh"
#include "xmpp/stanza.hh"
#include "xmpp/stanza_view.hh"

void weechat::account::clear_server_capability_snapshot()
{
    server_domain_identities_.clear();
    server_domain_features_.clear();
    server_components_.clear();
    pending_disco_summary_info_id_.reset();
    pending_disco_summary_items_id_.reset();
}

void weechat::account::record_domain_disco(const ::xmpp::StanzaView query)
{
    if (!query.valid())
        return;

    server_domain_identities_ = ::xmpp::disco_identities_from_query(query);
    server_domain_features_ = ::xmpp::disco_feature_vars(query);
    std::ranges::sort(server_domain_features_);
}

void weechat::account::record_component_disco(std::string_view jid,
                                              const ::xmpp::StanzaView query)
{
    if (!query.valid() || jid.empty())
        return;

    const std::string bare = ::jid(nullptr, std::string(jid)).bare;
    if (bare.empty())
        return;

    ::xmpp::discovered_component comp;
    comp.jid = bare;
    comp.identities = ::xmpp::disco_identities_from_query(query);
    comp.features = ::xmpp::disco_feature_vars(query);
    std::ranges::sort(comp.features);
    server_components_[bare] = std::move(comp);
}

[[nodiscard]] ::xmpp::server_capabilities
weechat::account::gather_server_capabilities() const
{
    ::xmpp::server_capabilities caps;

    ::jid parsed(nullptr, std::string(option_jid.string()));
    caps.domain = parsed.domain;

    caps.domain_identities = server_domain_identities_;
    caps.domain_features = server_domain_features_;

    caps.components.reserve(server_components_.size());
    for (const auto &[_, comp] : server_components_)
        caps.components.push_back(comp);
    std::ranges::sort(caps.components, {}, &::xmpp::discovered_component::jid);

    caps.sm_available = sm_available;
    caps.csi_available = csi_available;
    caps.sm_enabled = sm_enabled;

    const auto cached = stream_ext_cache_get(caps.domain);
    if (cached.sm && !*cached.sm)
        caps.sm_cached_disabled = true;
    if (cached.csi && !*cached.csi)
        caps.csi_cached_disabled = true;

    caps.carbons_advertised =
        ::xmpp::features_contain(caps.domain_features, "urn:xmpp:carbons:2");

    caps.upload_discovered = !upload_service.empty();
    caps.upload_jid = upload_service;
    caps.upload_max_bytes = upload_max_size;

    caps.pubsub_services = known_pubsub_services;
    caps.pubsub_mam_services.assign(pubsub_mam_services.begin(),
                                    pubsub_mam_services.end());
    std::ranges::sort(caps.pubsub_mam_services);

    return caps;
}

void weechat::account::print_disco_summary_to_buffer(std::string_view title)
{
    if (!buffer)
        return;

    auto ui = weechat::UiPort::for_buffer(buffer);
    if (!title.empty())
        ui->printf_network(std::string(title));

    const auto caps = gather_server_capabilities();
    for (const std::string &line : ::xmpp::format_disco_summary(caps))
        ui->printf(line);
}

void weechat::account::schedule_connect_disco_summary()
{
    if (connect_disco_summary_timer_hook_)
    {
        weechat_unhook(connect_disco_summary_timer_hook_);
        connect_disco_summary_timer_hook_ = nullptr;
    }

    // One-shot delay so disco#items fan-out (upload, pubsub, …) can populate
    // server_components_ before we snapshot capabilities for support logs.
    connect_disco_summary_timer_hook_ = static_cast<struct t_hook *>(weechat_hook_timer(
        6 * 1000, 0, 1, &account::connect_disco_summary_timer_cb, this, nullptr));
}

void weechat::account::send_server_disco_summary_refresh()
{
    if (!connected() || !context)
        return;

    ::jid parsed(nullptr, std::string(option_jid.string()));
    if (parsed.domain.empty())
        return;

    const std::string info_id = stanza::uuid(context);
    pending_disco_summary_info_id_ = info_id;

    connection.send(stanza::iq()
                        .from(jid())
                        .to(parsed.domain)
                        .type("get")
                        .id(info_id)
                        .xep0030()
                        .query()
                        .build(context)
                        .get());

    const std::string items_id = stanza::uuid(context);
    pending_disco_summary_items_id_ = items_id;

    connection.send(stanza::iq()
                        .from(jid())
                        .to(parsed.domain)
                        .type("get")
                        .id(items_id)
                        .xep0030()
                        .query_items()
                        .build(context)
                        .get());
}