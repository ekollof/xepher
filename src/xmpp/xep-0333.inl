// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "node.hh"
#pragma GCC visibility push(default)
#include "ns.hh"
#pragma GCC visibility pop

namespace stanza {

    /* Message Processing Hints (XEP-0333) */
    struct xep0333 {

        // <store xmlns='urn:xmpp:hints'/>
        struct store : virtual public spec {
            store() : spec("store") {
                xmlns<urn::xmpp::hints>();
            }
        };

        // <no-store xmlns='urn:xmpp:hints'/>
        struct no_store : virtual public spec {
            no_store() : spec("no-store") {
                xmlns<urn::xmpp::hints>();
            }
        };

        // <no-permanent-store xmlns='urn:xmpp:hints'/>
        struct no_permanent_store : virtual public spec {
            no_permanent_store() : spec("no-permanent-store") {
                xmlns<urn::xmpp::hints>();
            }
        };

        // <received xmlns='urn:xmpp:chat-markers:0' id='...'/>  (XEP-0333 Chat Markers)
        struct markers_received : virtual public spec {
            markers_received() : spec("received") {
                xmlns<urn::xmpp::chat_markers::_0>();
            }
            markers_received& id(std::string_view s) { attr("id", s); return *this; }
        };

        // <displayed xmlns='urn:xmpp:chat-markers:0' id='...'/>  (XEP-0333 Chat Markers)
        struct markers_displayed : virtual public spec {
            markers_displayed() : spec("displayed") {
                xmlns<urn::xmpp::chat_markers::_0>();
            }
            markers_displayed& id(std::string_view s) { attr("id", s); return *this; }
        };

        // stanza::message mixin
        struct message : virtual public spec {
            message() : spec("message") {}

            message& store() { xep0333::store s; child(s); return *this; }
            message& no_store() { xep0333::no_store ns; child(ns); return *this; }
            message& no_permanent_store() { xep0333::no_permanent_store nps; child(nps); return *this; }
            message& chat_marker_received(std::string_view msg_id) {
                xep0333::markers_received r;
                r.id(msg_id);
                child(r);
                return *this;
            }
            message& chat_marker_displayed(std::string_view msg_id) {
                xep0333::markers_displayed d;
                d.id(msg_id);
                child(d);
                return *this;
            }
        };
    };

}
