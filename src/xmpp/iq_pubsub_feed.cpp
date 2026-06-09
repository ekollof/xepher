// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <array>
#include <ranges>

#include "iq_pubsub_feed.hh"
#include "node.hh"

namespace xmpp {

bool is_skipped_non_atom_feed_item_id(const std::string_view item_id)
{
    return item_id.starts_with("urn:xmpp:avatar:")
        || item_id.starts_with("urn:xmpp:omemo:")
        || item_id.starts_with("urn:xmpp:bookmarks:");
}

bool is_microblog_comments_node(const std::string_view node)
{
    return node.starts_with(k_microblog_comments_prefix);
}

bool is_pubsub_component_jid(const std::string_view jid_sv,
                               const std::span<const std::string> known_pubsub_services)
{
    if (jid_sv.empty())
        return false;

    if (std::ranges::contains(known_pubsub_services, jid_sv))
        return true;

    // Domain-only service JIDs (news.movim.eu) have no localpart.
    if (!jid_sv.contains('@'))
        return true;

    const jid parsed(nullptr, std::string(jid_sv));
    if (parsed.local.empty())
        return true;

    static constexpr std::array<std::string_view, 4> k_pubsub_locals = {
        "feed", "pubsub", "pep", "microblog"};
    return std::ranges::contains(k_pubsub_locals, std::string_view(parsed.local));
}

bool should_default_pep_microblog_node(const std::string_view service_jid,
                                       const std::span<const std::string> known_pubsub_services)
{
    if (!service_jid.contains('@'))
        return false;
    return !is_pubsub_component_jid(service_jid, known_pubsub_services);
}

}  // namespace xmpp