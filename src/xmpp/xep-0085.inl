// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "node.hh"
#pragma GCC visibility push(default)
#include "ns.hh"
#pragma GCC visibility pop

namespace stanza {

    /* Chat State Notifications (XEP-0085) */
    struct xep0085 {

        // <active/>, <composing/>, <paused/>, <inactive/>, <gone/>
        // (any element in the chatstates namespace)
        struct chatstate : virtual public spec {
            chatstate(std::string_view state_name) : spec(state_name) {
                xmlns<jabber_org::protocol::chatstates>();
            }
        };

        // stanza::message mixin
        struct message : virtual public spec {
            message() : spec("message") {}

            message& chatstate(std::string_view state) {
                xep0085::chatstate cs(state);
                child(cs);
                return *this;
            }
        };
    };

}
