// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <string>

#include "node.hh"
#pragma GCC visibility push(default)
#include "ns.hh"
#pragma GCC visibility pop

namespace stanza {

    /* Ephemeral Messages (XEP-0466) */
    struct xep0466 {

        // <ephemeral xmlns='urn:xmpp:ephemeral:0' timer='N'/>
        struct ephemeral : virtual public spec {
            explicit ephemeral(long timer_secs) : spec("ephemeral") {
                xmlns<urn::xmpp::ephemeral::_0>();
                attr("timer", std::to_string(timer_secs));
            }
        };

        // stanza::message mixin
        struct message : virtual public spec {
            message() : spec("message") {}

            message& ephemeral(long timer_secs) {
                xep0466::ephemeral el(timer_secs);
                child(el);
                return *this;
            }
        };
    };

}
