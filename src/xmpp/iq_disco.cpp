// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "iq_disco.hh"

#include <algorithm>
#include <cctype>
#include <ranges>

namespace xmpp {

namespace {

[[nodiscard]] bool attr_iequals(std::string_view a, std::string_view b)
{
    return a.size() == b.size()
        && std::ranges::equal(a, b, [](unsigned char x, unsigned char y) {
            return std::tolower(x) == std::tolower(y);
        });
}

}  // namespace

bool is_adhoc_commands_disco_node(const std::string_view node)
{
    return node == "http://jabber.org/protocol/commands";
}

bool is_channel_search_item_open(const std::string_view open_raw)
{
    return open_raw.empty()
        || attr_iequals(open_raw, "true")
        || open_raw == "1";
}

std::string normalize_channel_search_service_type(const std::string_view service_type)
{
    if (service_type == "xep-0045")
        return "muc";
    if (service_type == "xep-0369")
        return "mix";
    return std::string(service_type);
}

std::string join_bracketed_meta(const std::vector<std::string> &parts)
{
    if (parts.empty())
        return {};

    std::string out = "[";
    bool first = true;
    std::ranges::for_each(parts, [&](const std::string &part) {
        if (!first)
            out += ", ";
        first = false;
        out += part;
    });
    out += "]";
    return out;
}

std::vector<std::string> disco_feature_vars(const StanzaView query)
{
    std::vector<std::string> features;
    if (!query.valid())
        return features;

    for (const StanzaView feature : query)
    {
        if (feature.name() != "feature")
            continue;
        const std::string var = feature.attr_string("var");
        if (!var.empty())
            features.push_back(var);
    }
    return features;
}

}  // namespace xmpp