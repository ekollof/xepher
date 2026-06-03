// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "node.hh"
#pragma GCC visibility push(default)
#include "ns.hh"
#pragma GCC visibility pop

namespace stanza {

    /* Entity Time (XEP-0202) */
    struct xep0202 {

        // <time xmlns='urn:xmpp:time'><utc>...</utc><tzo>...</tzo></time>
        struct time : virtual public spec {
            time() : spec("time") {
                xmlns<urn::xmpp::time>();
            }

            time& utc(std::string_view s) {
                struct u : virtual public spec { u(std::string_view v) : spec("utc") { text(v); } };
                u el(s);
                child(el);
                return *this;
            }
            time& tzo(std::string_view s) {
                struct t : virtual public spec { t(std::string_view v) : spec("tzo") { text(v); } };
                t el(s);
                child(el);
                return *this;
            }
        };

        // stanza::iq mixin
        struct iq : virtual public spec {
            iq() : spec("iq") {}

            iq& time_element(xep0202::time t) { child(t); return *this; }
        };
    };

}
