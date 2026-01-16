// XEP-0352: Client State Indication
// https://xmpp.org/extensions/xep-0352.html

#pragma once

#include "node.hh"
#pragma GCC visibility push(default)
#include "ns.hh"
#pragma GCC visibility pop

namespace stanza {

    /* Client State Indication */
    struct xep0352 {
        struct active : virtual public spec {
            active() : spec("active") {
                attr("xmlns", "urn:xmpp:csi:0");
            }
        };

        struct inactive : virtual public spec {
            inactive() : spec("inactive") {
                attr("xmlns", "urn:xmpp:csi:0");
            }
        };
    };

}
