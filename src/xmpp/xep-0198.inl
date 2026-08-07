// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "node.hh"
#pragma GCC visibility push(default)
#include "ns.hh"
#pragma GCC visibility pop

namespace stanza {

    /* Stream Management (XEP-0198) */
    struct xep0198 {
        // <enable xmlns='urn:xmpp:sm:3' resume='true'/>
        struct enable : virtual public spec {
            enable(bool resume = true, uint32_t max_seconds = 0) : spec("enable") {
                attr("xmlns", "urn:xmpp:sm:3");
                if (resume) {
                    attr("resume", "true");
                }
                if (max_seconds > 0) {
                    attr("max", std::to_string(max_seconds));
                }
            }
        };

        // <resume xmlns='urn:xmpp:sm:3' h='N' previd='session-id'/>
        struct resume : virtual public spec {
            resume(uint32_t h, std::string_view previd) : spec("resume") {
                attr("xmlns", "urn:xmpp:sm:3");
                attr("h", std::to_string(h));
                attr("previd", previd);
            }
        };

        // <r xmlns='urn:xmpp:sm:3'/>
        struct request : virtual public spec {
            request() : spec("r") {
                attr("xmlns", "urn:xmpp:sm:3");
            }
        };

        // <a xmlns='urn:xmpp:sm:3' h='N'/>
        struct answer : virtual public spec {
            answer(uint32_t h) : spec("a") {
                attr("xmlns", "urn:xmpp:sm:3");
                attr("h", std::to_string(h));
            }
        };
    };

}
