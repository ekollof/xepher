// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "xmpp/server_capability_map.hh"

#include <algorithm>
#include <ranges>
#include <span>

#include <fmt/core.h>

namespace xmpp {

namespace {

[[nodiscard]] bool domain_has_feature(const server_capabilities &caps, std::string_view var)
{
    return features_contain(caps.domain_features, var);
}

[[nodiscard]] std::string format_identity_line(const disco_identity &id)
{
    if (!id.name.empty())
        return fmt::format("{}/{} ({})", id.category, id.type, id.name);
    return fmt::format("{}/{}", id.category, id.type);
}

[[nodiscard]] std::string notable_component_tags(const discovered_component &comp)
{
    std::vector<std::string> tags;
    if (features_contain(comp.features, "urn:xmpp:http:upload:0"))
        tags.emplace_back("upload");
    if (features_contain(comp.features, "urn:xmpp:mam:2"))
        tags.emplace_back("mam");
    if (std::ranges::any_of(comp.identities,
                            [](const disco_identity &id) { return id.category == "pubsub"; }))
        tags.emplace_back("pubsub");
    if (tags.empty())
        return {};

    std::string joined;
    bool first = true;
    std::ranges::for_each(tags, [&](const std::string &tag) {
        if (!first)
            joined += ", ";
        first = false;
        joined += tag;
    });
    return fmt::format(" [{}]", joined);
}

}  // namespace

std::vector<disco_identity> disco_identities_from_query(const StanzaView query)
{
    std::vector<disco_identity> identities;
    if (!query.valid())
        return identities;

    for (const StanzaView identity : query)
    {
        if (identity.name() != "identity")
            continue;
        disco_identity id;
        id.category = identity.attr_string("category");
        id.type = identity.attr_string("type");
        id.name = identity.attr_string("name");
        if (id.category.empty() && id.type.empty() && id.name.empty())
            continue;
        identities.push_back(std::move(id));
    }
    return identities;
}

bool features_contain(const std::span<const std::string> features,
                      const std::string_view var) noexcept
{
    return std::ranges::any_of(features, [&](const std::string &f) { return f == var; });
}

bool capability_enabled(const server_capabilities &caps, const capability_id id) noexcept
{
    switch (id)
    {
    case capability_id::stream_management:
        return caps.sm_available;
    case capability_id::client_state_indication:
        return caps.csi_available;
    case capability_id::message_carbons:
        return caps.carbons_advertised;
    case capability_id::global_mam:
    case capability_id::bookmarks:
    case capability_id::mds:
        return true;
    case capability_id::http_upload:
        return caps.upload_discovered;
    case capability_id::pubsub_feeds:
        return !caps.pubsub_services.empty();
    case capability_id::pubsub_mam:
        return !caps.pubsub_mam_services.empty();
    }
    return false;
}

std::string capability_status_label(const capability_id id,
                                    const server_capabilities &caps)
{
    switch (id)
    {
    case capability_id::stream_management:
        if (!caps.sm_available)
            return caps.sm_cached_disabled ? "disabled (cached downgrade)" : "not advertised";
        return caps.sm_enabled ? "enabled" : "available";
    case capability_id::client_state_indication:
        if (!caps.csi_available)
            return caps.csi_cached_disabled ? "disabled (cached downgrade)" : "not advertised";
        return "available";
    case capability_id::message_carbons:
        return caps.carbons_advertised ? "advertised on domain" : "not on domain disco";
    case capability_id::global_mam:
        return domain_has_feature(caps, "urn:xmpp:mam:2")
            ? "advertised on domain + always queried"
            : "account archive (always queried)";
    case capability_id::bookmarks:
        if (domain_has_feature(caps, "urn:xmpp:bookmarks:1"))
            return "native PEP (domain hint) + always queried";
        if (domain_has_feature(caps, "storage:bookmarks"))
            return "legacy storage (domain hint) + always queried";
        return "account PEP (always queried)";
    case capability_id::mds:
        return domain_has_feature(caps, "urn:xmpp:mds:displayed:0")
            ? "advertised on domain + always queried"
            : "account PEP (always queried)";
    case capability_id::http_upload:
        if (!caps.upload_discovered)
            return "not discovered";
        if (caps.upload_max_bytes > 0)
            return fmt::format("{} (max {} MB)", caps.upload_jid,
                               caps.upload_max_bytes / (1024 * 1024));
        return caps.upload_jid;
    case capability_id::pubsub_feeds:
        return caps.pubsub_services.empty()
            ? "no pubsub components yet"
            : fmt::format("{} service(s)", caps.pubsub_services.size());
    case capability_id::pubsub_mam:
        return caps.pubsub_mam_services.empty()
            ? "none with MAM"
            : fmt::format("{} service(s)", caps.pubsub_mam_services.size());
    }
    return {};
}

std::vector<std::string> format_disco_summary(const server_capabilities &caps)
{
    std::vector<std::string> lines;

    lines.emplace_back(fmt::format("Server discovery summary for {}",
                                   caps.domain.empty() ? "(unknown domain)" : caps.domain));

    lines.emplace_back("");
    lines.emplace_back("Domain disco#info:");
    if (caps.domain_identities.empty())
        lines.emplace_back("  (no identities recorded)");
    else
    {
        std::ranges::for_each(caps.domain_identities, [&](const disco_identity &id) {
            lines.emplace_back(fmt::format("  identity: {}", format_identity_line(id)));
        });
    }
    if (caps.domain_features.empty())
        lines.emplace_back("  (no features recorded)");
    else
    {
        std::ranges::for_each(caps.domain_features, [&](const std::string &feat) {
            lines.emplace_back(fmt::format("  feature: {}", feat));
        });
    }

    lines.emplace_back("");
    lines.emplace_back("Stream extensions:");
    lines.emplace_back(fmt::format("  XEP-0198 SM: {}",
                                   capability_status_label(capability_id::stream_management, caps)));
    lines.emplace_back(fmt::format("  XEP-0352 CSI: {}",
                                   capability_status_label(capability_id::client_state_indication,
                                                             caps)));

    lines.emplace_back("");
    lines.emplace_back("Derived capabilities:");
    lines.emplace_back(fmt::format("  Message carbons: {}",
                                   capability_status_label(capability_id::message_carbons, caps)));
    lines.emplace_back(fmt::format("  Global MAM: {}",
                                   capability_status_label(capability_id::global_mam, caps)));
    lines.emplace_back(fmt::format("  Bookmarks: {}",
                                   capability_status_label(capability_id::bookmarks, caps)));
    lines.emplace_back(fmt::format("  MDS: {}",
                                   capability_status_label(capability_id::mds, caps)));
    lines.emplace_back(fmt::format("  HTTP upload: {}",
                                   capability_status_label(capability_id::http_upload, caps)));
    lines.emplace_back(fmt::format("  PubSub feeds: {}",
                                   capability_status_label(capability_id::pubsub_feeds, caps)));
    lines.emplace_back(fmt::format("  PubSub MAM: {}",
                                   capability_status_label(capability_id::pubsub_mam, caps)));

    lines.emplace_back("");
    lines.emplace_back("Discovered components:");
    if (caps.components.empty())
        lines.emplace_back("  (none yet — run '/disco summary refresh' after connect)");
    else
    {
        std::ranges::for_each(caps.components, [&](const discovered_component &comp) {
            std::string id_line;
            if (comp.identities.empty())
                id_line = comp.jid;
            else
                id_line = fmt::format("{} — {}", comp.jid,
                                      format_identity_line(comp.identities.front()));
            lines.emplace_back(fmt::format("  {}{}", id_line, notable_component_tags(comp)));
        });
    }

    return lines;
}

}  // namespace xmpp