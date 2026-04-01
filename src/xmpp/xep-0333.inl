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

        // stanza::message mixin
        struct message : virtual public spec {
            message() : spec("message") {}

            message& store() { xep0333::store s; child(s); return *this; }
            message& no_store() { xep0333::no_store ns; child(ns); return *this; }
            message& no_permanent_store() { xep0333::no_permanent_store nps; child(nps); return *this; }
        };
    };

}
