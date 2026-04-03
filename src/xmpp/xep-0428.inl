// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "node.hh"
#pragma GCC visibility push(default)
#include "ns.hh"
#pragma GCC visibility pop

namespace stanza {

    /* Fallback Indication (XEP-0428) */
    struct xep0428 {

        // <fallback xmlns='urn:xmpp:fallback:0' for='...'>
        //   <body start='0' end='N'/>
        //   <subject start='0' end='N'/>
        // </fallback>
        //
        // XEP-0428 §4: the <body>/<subject> children indicate which portion
        // of the body/subject is the human-readable fallback text.
        // body_end should be the character length of the fallback body text;
        // pass 0 to omit the <body> child (e.g. when there is no body fallback).
        struct fallback : virtual public spec {
            fallback(std::string_view for_ns, std::size_t body_end = 0) : spec("fallback") {
                xmlns<urn::xmpp::fallback::_0>();
                attr("for", for_ns);
                if (body_end > 0) {
                    struct body_range : virtual public spec {
                        body_range(std::size_t end) : spec("body") {
                            attr("start", "0");
                            attr("end", std::to_string(end));
                        }
                    };
                    body_range br(body_end);
                    child(br);
                }
            }
        };

        // stanza::message mixin
        struct message : virtual public spec {
            message() : spec("message") {}

            message& fallback(xep0428::fallback f) { child(f); return *this; }
        };
    };

}
