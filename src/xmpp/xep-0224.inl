// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "node.hh"
#pragma GCC visibility push(default)
#include "ns.hh"
#pragma GCC visibility pop

namespace stanza {

    /* Attention (XEP-0224) */
    struct xep0224 {

        // <attention xmlns='urn:xmpp:attention:0'/>
        struct attention : virtual public spec {
            attention() : spec("attention") {
                xmlns<urn::xmpp::attention::_0>();
            }
        };

        // stanza::message mixin
        struct message : virtual public spec {
            message() : spec("message") {}

            message& attention() {
                xep0224::attention el;
                child(el);
                return *this;
            }
        };
    };

}
