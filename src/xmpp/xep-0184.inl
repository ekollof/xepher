// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "node.hh"
#pragma GCC visibility push(default)
#include "ns.hh"
#pragma GCC visibility pop

namespace stanza {

    /* Message Delivery Receipts (XEP-0184) */
    struct xep0184 {

        // <received xmlns='urn:xmpp:receipts' id='...'/>
        struct received : virtual public spec {
            received() : spec("received") {
                xmlns<urn::xmpp::receipts>();
            }
            received& id(std::string_view s) { attr("id", s); return *this; }
        };

        // <request xmlns='urn:xmpp:receipts'/>
        struct request : virtual public spec {
            request() : spec("request") {
                xmlns<urn::xmpp::receipts>();
            }
        };

        // stanza::message mixin
        struct message : virtual public spec {
            message() : spec("message") {}

            message& receipt_received(std::string_view msg_id) {
                xep0184::received r;
                r.id(msg_id);
                child(r);
                return *this;
            }

            message& receipt_request() {
                xep0184::request r;
                child(r);
                return *this;
            }
        };
    };

}
