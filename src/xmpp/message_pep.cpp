// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "message_pep.hh"

#include <algorithm>
#include <cctype>
#include <ranges>

#include "xmpp/node.hh"

namespace xmpp {

namespace {

[[nodiscard]] bool jid_bare_iequals(std::string_view a, std::string_view b)
{
    return a.size() == b.size()
        && std::ranges::equal(a, b, [](unsigned char x, unsigned char y) {
            return std::tolower(x) == std::tolower(y);
        });
}

}  // namespace

bool pep_node_is_microblog(std::string_view node)
{
    return node == k_pep_microblog || node.starts_with("urn:xmpp:microblog:0:comments/");
}

bool pep_node_is_protocol_uri(std::string_view node)
{
    if (node.empty() || pep_node_is_microblog(node))
        return false;

    const bool protocol_ns = node.starts_with("eu.siacs.")
        || node.starts_with("com.google.")
        || node.starts_with("org.jivesoftware.");
    return node.contains("://")
        || node.starts_with("urn:")
        || protocol_ns;
}

bool pep_from_is_self(std::string_view from_full, std::string_view own_jid)
{
    if (from_full.empty() || own_jid.empty())
        return false;

    const std::string own_bare = ::jid(nullptr, std::string(own_jid).c_str()).bare;
    const std::string from_bare = ::jid(nullptr, std::string(from_full).c_str()).bare;
    return !own_bare.empty() && !from_bare.empty()
        && jid_bare_iequals(own_bare, from_bare);
}

bool pep_node_is_legacy_omemo(std::string_view node)
{
    return node.starts_with("eu.siacs.conversations.axolotl");
}

bool pep_node_is_known_protocol_node(std::string_view node)
{
    return node == k_pep_omemo2_devices
        || node == k_pep_avatar_metadata
        || node == k_pep_avatar_data
        || node == k_pep_bookmarks
        || node == k_pep_mds_displayed;
}

PubsubFeedGate classify_generic_pubsub_feed(
    std::string_view node,
    std::string_view from_full,
    std::string_view own_jid)
{
    PubsubFeedGate gate;
    if (node.empty() || from_full.empty())
        return gate;

    if (pep_node_is_legacy_omemo(node))
    {
        gate.drop_legacy_omemo = true;
        return gate;
    }

    if (pep_node_is_protocol_uri(node)
        || pep_from_is_self(from_full, own_jid)
        || pep_node_is_known_protocol_node(node))
        return gate;

    gate.is_generic_feed = true;
    return gate;
}

}  // namespace xmpp