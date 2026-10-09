// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
#include "xmpp/iq_pubsub_mam.hh"

#include "xmpp/iq_mam.hh"
#include "xmpp/xep-0059.inl"

namespace xmpp {
auto recover_pubsub_mam_cursor(const PubsubMamQuery &query, StanzaView iq)
    -> std::optional<PubsubMamQuery>
{
    if (query.cursor.empty() || query.stale_retried || !iq_has_item_not_found_error(iq))
        return std::nullopt;
    auto retry = query;
    retry.cursor.clear();
    retry.visited_cursors.clear();
    retry.initial_tail = false;
    retry.stable = true;
    retry.stale_retried = true;
    return retry;
}

auto pubsub_mam_publisher(StanzaView message, StanzaView item,
                          std::string_view service) -> std::string
{
    if (auto publisher = item.attr_string("publisher"); !publisher.empty())
        return publisher;
    if (auto publisher = message.attr_string("from"); !publisher.empty())
        return publisher;
    return std::string(service);
}

auto make_pubsub_mam_iq(std::string_view id, std::string_view from,
                       const PubsubMamQuery &query) -> stanza::iq
{
    stanza::xep0313::query mam;
    mam.node(query.node).queryid(id);
    stanza::xep0059::set rsm;
    rsm.max(static_cast<unsigned>(query.max_items > 0 ? query.max_items : 20));
    if (query.initial_tail)
        rsm.before();
    else if (!query.cursor.empty())
        rsm.after(query.cursor);
    mam.rsm(rsm);
    if (query.order_creation)
    {
        struct order_spec : stanza::spec {
            order_spec() : spec("order") {
                xmlns<urn::xmpp::order_by::_1>();
                attr("by", "creation");
            }
        } order;
        mam.child(order);
    }
    auto iq = stanza::iq().from(from).to(query.service).type("set").id(id);
    iq.xep0313().query(mam);
    return iq;
}

auto advance_pubsub_mam(PubsubMamQuery query, StanzaView fin)
    -> std::expected<PubsubMamPage, std::string>
{
    query.stable = mam_fetch_remains_stable(fin, query.stable);
    const auto last = mam_fin_rsm_last(fin);
    const bool complete = is_mam_fin_bool_attr_true(fin.attr_string("complete"));
    if (last.empty() && !complete)
        return std::unexpected("incomplete PubSub MAM page has no RSM last cursor");
    // A first restore intentionally requests only the latest page. Later
    // restores walk forward from its checkpoint until every new item is read.
    if (complete || query.initial_tail)
        return PubsubMamPage{std::nullopt, query.stable
            ? std::optional<std::string>(last.empty() ? query.cursor : last) : std::nullopt};
    if (last == query.cursor || !query.visited_cursors.insert(last).second)
        return std::unexpected("PubSub MAM cursor repeated; stopping paging");
    query.cursor = last;
    return PubsubMamPage{std::move(query), {}};
}
}
