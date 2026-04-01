// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <optional>
#include <string>

#include "node.hh"
#pragma GCC visibility push(default)
#include "ns.hh"
#pragma GCC visibility pop

namespace stanza {

    /* Instant Messaging and Presence */
    struct rfc6121 {
        struct item : virtual public spec {
            item(std::string_view jid_s) : spec("item") {
                attr("jid", jid_s);
            }

            item& name(std::string_view s) { attr("name", s); return *this; }
            item& subscription(std::string_view s) { attr("subscription", s); return *this; }
        };

        struct query : virtual public spec {
            query() : spec("query") {
                xmlns<jabber::iq::roster>();
            }

            query& item(rfc6121::item i) { child(i); return *this; }
        };

        struct iq : virtual public spec {
            iq() : spec("iq") {}

            iq& rfc6121() { return *this; }

            iq& query(rfc6121::query q = {}) { child(q); return *this; }
        };
    };

}
