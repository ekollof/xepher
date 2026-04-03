// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "node.hh"
#pragma GCC visibility push(default)
#include "ns.hh"
#pragma GCC visibility pop

namespace stanza {

    /* Message Moderation (XEP-0425) */
    struct xep0425 {

        // <moderate xmlns='urn:xmpp:message-moderate:1'>
        //   [<reason>text</reason>]
        // </moderate>
        //
        // XEP-0425 §3: the client sends this inside <apply-to id='...'>.
        // The <retract> element is part of the *notification* the MUC service
        // broadcasts back to room members — it MUST NOT be in the request IQ.
        struct moderate : virtual public spec {
            moderate() : spec("moderate") {
                xmlns<urn::xmpp::message_moderate::_1>();
            }

            moderate& reason(std::string_view s) {
                struct reason_el : virtual public spec {
                    reason_el(std::string_view v) : spec("reason") { text(v); }
                };
                reason_el re(s);
                child(re);
                return *this;
            }
        };

        // stanza::message mixin
        struct message : virtual public spec {
            message() : spec("message") {}

            message& moderate(xep0425::moderate m) { child(m); return *this; }
        };
    };

}
