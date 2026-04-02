// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "node.hh"
#pragma GCC visibility push(default)
#include "ns.hh"
#pragma GCC visibility pop

namespace stanza {

    /* Occupant identifiers for semi-anonymous MUCs (XEP-0421) */
    struct xep0421 {

        // <occupant-id xmlns='urn:xmpp:occupant-id:0' id='...'/>
        struct occupant_id : virtual public spec {
            occupant_id(std::string_view id_val) : spec("occupant-id") {
                xmlns<urn::xmpp::occupant_id::_0>();
                attr("id", id_val);
            }
        };

        // stanza::message mixin
        struct message : virtual public spec {
            message() : spec("message") {}

            message& occupant_id(xep0421::occupant_id o) { child(o); return *this; }
        };

        // stanza::presence mixin
        struct presence : virtual public spec {
            presence() : spec("presence") {}

            presence& occupant_id(xep0421::occupant_id o) { child(o); return *this; }
        };
    };

}
