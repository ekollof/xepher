// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "node.hh"
#include "xep-0059.inl"
#pragma GCC visibility push(default)
#include "ns.hh"
#pragma GCC visibility pop

namespace stanza {

    /* Publish-Subscribe (XEP-0060) */
    struct xep0060 {

        // <item id='...'> ... </item>
        struct item : virtual public spec {
            item() : spec("item") {}

            item& id(std::string_view s) { attr("id", s); return *this; }

            item& payload(spec& s) { child(s); return *this; }
        };

        // <items node='...'/>
        struct items : virtual public spec {
            items() : spec("items") {}
            items(std::string_view node_uri) : spec("items") {
                attr("node", node_uri);
            }

            items& node(std::string_view s) { attr("node", s); return *this; }
            items& max_items(unsigned n) {
                attr("max_items", std::to_string(n));
                return *this;
            }

            items& item(xep0060::item i) { child(i); return *this; }
        };

        // <publish node='...'>
        struct publish : virtual public spec {
            publish() : spec("publish") {}
            publish(std::string_view node_uri) : spec("publish") {
                attr("node", node_uri);
            }

            publish& node(std::string_view s) { attr("node", s); return *this; }

            publish& item(xep0060::item i) { child(i); return *this; }
        };

        // <publish-options>
        //   <x xmlns='jabber:x:data' type='submit'>...</x>
        // </publish-options>
        struct publish_options : virtual public spec {
            publish_options() : spec("publish-options") {}

            publish_options& child_spec(spec& s) { child(s); return *this; }
        };

        // <subscribe node='...' jid='...'/>
        struct subscribe : virtual public spec {
            subscribe(std::string_view node_uri, std::string_view jid_s) : spec("subscribe") {
                attr("node", node_uri);
                attr("jid", jid_s);
            }
        };

        // <unsubscribe node='...' jid='...'/>
        struct unsubscribe : virtual public spec {
            unsubscribe(std::string_view node_uri, std::string_view jid_s) : spec("unsubscribe") {
                attr("node", node_uri);
                attr("jid", jid_s);
            }
        };

        // <retract node='...'>
        struct retract : virtual public spec {
            retract(std::string_view node_uri) : spec("retract") {
                attr("node", node_uri);
            }

            retract& item(xep0060::item i) { child(i); return *this; }
        };

        // <pubsub xmlns='http://jabber.org/protocol/pubsub'>
        struct pubsub : virtual public spec {
            pubsub() : spec("pubsub") {
                xmlns<jabber_org::protocol::pubsub>();
            }

            pubsub& items(xep0060::items i)   { child(i); return *this; }
            pubsub& publish(xep0060::publish p) { child(p); return *this; }
            pubsub& publish_options(xep0060::publish_options p) { child(p); return *this; }
            pubsub& subscribe(xep0060::subscribe s) { child(s); return *this; }
            pubsub& unsubscribe(xep0060::unsubscribe s) { child(s); return *this; }
            pubsub& retract(xep0060::retract r) { child(r); return *this; }
            // RSM <set> sibling of <items> (for paginated fetches)
            pubsub& rsm(xep0059::set s) { child(s); return *this; }
        };

        // Helper: stanza::iq mixin
        struct iq : virtual public spec {
            iq() : spec("iq") {}

            iq& xep0060() { return *this; }

            iq& pubsub(xep0060::pubsub p) { child(p); return *this; }
        };
    };

}

namespace xml {

    /* Publish-Subscribe parse layer (XEP-0060) */
    class xep0060 : virtual public node {
    public:
        struct item {
            item(node& n) {
                id = n.get_attr("id");
                // Payload is the first child element (if any)
                if (!n.children.empty())
                    payload = n.children.front();
            }

            std::optional<std::string> id;
            std::optional<node> payload;
        };

        struct items {
            items(node& n) {
                node_uri = n.get_attr("node");
                for (auto& ch : n.get_children("item"))
                    this->items_list.emplace_back(ch.get());
            }

            std::optional<std::string> node_uri;
            std::vector<item> items_list;
        };

        struct event_items {
            event_items(node& n) {
                node_uri = n.get_attr("node");
                for (auto& ch : n.get_children("item"))
                    items_list.emplace_back(ch.get());
                for (auto& ch : n.get_children("retract"))
                    retracted_ids.push_back(ch.get().get_attr("id").value_or(""));
            }

            std::optional<std::string> node_uri;
            std::vector<item> items_list;
            std::vector<std::string> retracted_ids;
        };

    private:
        std::optional<std::optional<items>> _items;
        std::optional<std::optional<event_items>> _event_items;
    public:
        // Parses <pubsub xmlns='...'><items node='...'>...</items></pubsub>
        std::optional<items>& pubsub_items() {
            if (!_items) {
                auto ps = get_children<jabber_org::protocol::pubsub>("pubsub");
                if (!ps.empty()) {
                    auto its = ps.front().get().get_children("items");
                    if (!its.empty())
                        _items = items(its.front().get());
                    else
                        _items.emplace(std::nullopt);
                } else {
                    _items.emplace(std::nullopt);
                }
            }
            return *_items;
        }

        // Parses <event xmlns='http://jabber.org/protocol/pubsub#event'><items ...>
        std::optional<event_items>& pubsub_event_items() {
            if (!_event_items) {
                auto evs = get_children<jabber_org::protocol::pubsub::event>("event");
                if (!evs.empty()) {
                    auto its = evs.front().get().get_children("items");
                    if (!its.empty())
                        _event_items = event_items(its.front().get());
                    else
                        _event_items.emplace(std::nullopt);
                } else {
                    _event_items.emplace(std::nullopt);
                }
            }
            return *_event_items;
        }
    };

}
