// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "node.hh"
#pragma GCC visibility push(default)
#include "ns.hh"
#pragma GCC visibility pop

namespace stanza {

    /* MUC Room Registration (XEP-0045 §7.10). */
    struct xep0045register {

        // muc#register is the FORM_TYPE, not the IQ query namespace.
        struct query : virtual public spec {
            query() : spec("query") {
                xmlns<jabber::iq::register_>();
            }
            query& form(xep0004::form& f) { child(f); return *this; }
        };

        struct iq : virtual public spec {
            iq() : spec("iq") {}

            iq& muc_register(query& q) { child(q); return *this; }
        };
    };

}
