// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "node.hh"
#pragma GCC visibility push(default)
#include "ns.hh"
#pragma GCC visibility pop

namespace stanza {

    /* Out of Band Data (XEP-0066) */
    struct xep0066 {

        // <x xmlns='jabber:x:oob'><url>...</url></x>
        struct oob : virtual public spec {
            oob(std::string_view url) : spec("x") {
                xmlns<jabber::x::oob>();
                struct url_elem : virtual public spec {
                    url_elem(std::string_view u) : spec("url") { text(u); }
                };
                url_elem ue(url);
                child(ue);
            }
        };

        // stanza::message mixin
        struct message : virtual public spec {
            message() : spec("message") {}

            message& oob(xep0066::oob o) { child(o); return *this; }
        };
    };

}
