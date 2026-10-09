// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
#include "account.hh"

#include <algorithm>
#include <ranges>
#include <fmt/core.h>

void weechat::account::start_pubsub_mam(std::string_view service, std::string_view node,
                                      int max_items)
{
    ::xmpp::PubsubMamQuery query;
    query.service = std::string(service);
    query.node = std::string(node);
    query.max_items = max_items;
    query.cursor = mam_cursor_get(fmt::format("pubsub:{}/{}", service, node));
    query.initial_tail = query.cursor.empty();
    if (!query.cursor.empty())
        query.visited_cursors.insert(query.cursor);
    const auto caps = gather_server_capabilities();
    query.order_creation = std::ranges::any_of(caps.components, [&](const auto &component) {
        return component.jid == service && std::ranges::any_of(component.features,
            [](std::string_view feature) {
                return feature == "urn:xmpp:order-by:1"
                    || feature == "urn:xmpp:order-by:1@urn:xmpp:mam:2";
            });
    });
    send_pubsub_mam_page(std::move(query));
}

void weechat::account::send_pubsub_mam_page(::xmpp::PubsubMamQuery query)
{
    if (!connected() || !xmpp_feeds_enabled()
        || !feed_is_open(fmt::format("{}/{}", query.service, query.node)))
        return;
    const auto id = stanza::uuid(context);
    auto iq = ::xmpp::make_pubsub_mam_iq(id, jid(), query);
    pubsub_mam_queries.emplace(id, std::move(query));
    connection.send(iq.build(context).get());
}
