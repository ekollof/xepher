// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "node.hh"
#pragma GCC visibility push(default)
#include "ns.hh"
#pragma GCC visibility pop

namespace stanza {

    /* Software Version (XEP-0092) */
    struct xep0092 {

        // <query xmlns='jabber:iq:version'><name>...</name><version>...</version><os>...</os></query>
        struct query : virtual public spec {
            query() : spec("query") {
                xmlns<jabber::iq::version>();
            }

            query& name(std::string_view s) {
                struct n : virtual public spec { n(std::string_view v) : spec("name") { text(v); } };
                n el(s);
                child(el);
                return *this;
            }
            query& version(std::string_view s) {
                struct v : virtual public spec { v(std::string_view vv) : spec("version") { text(vv); } };
                v el(s);
                child(el);
                return *this;
            }
            query& os(std::string_view s) {
                struct o : virtual public spec { o(std::string_view v) : spec("os") { text(v); } };
                o el(s);
                child(el);
                return *this;
            }
        };

        // stanza::iq mixin
        struct iq : virtual public spec {
            iq() : spec("iq") {}

            iq& version_query(xep0092::query q) { child(q); return *this; }
        };
    };

}
