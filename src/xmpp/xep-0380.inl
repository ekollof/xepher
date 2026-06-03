// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "node.hh"
#pragma GCC visibility push(default)
#include "ns.hh"
#pragma GCC visibility pop

namespace stanza {

    /* Explicit Message Encryption (XEP-0380) */
    struct xep0380 {

        // <encryption xmlns='urn:xmpp:eme:0' namespace='...'/>
        struct eme : virtual public spec {
            eme(std::string_view ns) : spec("encryption") {
                xmlns<urn::xmpp::eme::_0>();
                attr("namespace", ns);
            }
        };

        // stanza::message mixin
        struct message : virtual public spec {
            message() : spec("message") {}

            message& eme(xep0380::eme e) { child(e); return *this; }
        };
    };

}
