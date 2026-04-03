// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <strophe.h>
#include "xep-0163.inl"

namespace xmpp { namespace xep0172 {

    // XEP-0172: User Nickname
    // Publish the user's display nickname via PEP so that contacts can
    // show a human-readable name instead of the bare JID.
    //
    // Stanza emitted:
    //   <iq type='set' id='...'>
    //     <pubsub xmlns='http://jabber.org/protocol/pubsub'>
    //       <publish node='http://jabber.org/protocol/nick'>
    //         <item>
    //           <nick xmlns='http://jabber.org/protocol/nick'>Alice</nick>
    //         </item>
    //       </publish>
    //     </pubsub>
    //   </iq>
    //
    // Returns a caller-owned xmpp_stanza_t* (call xmpp_stanza_release when done).
    inline xmpp_stanza_t *publish_nick(xmpp_ctx_t *context, const char *nick)
    {
        // <nick xmlns='http://jabber.org/protocol/nick'> text </nick>
        struct nick_payload : virtual public stanza::spec {
            nick_payload(const char *n) : spec("nick") {
                xmlns<jabber_org::protocol::nick>();
                if (n && *n)
                    text(n);
            }
        };

        nick_payload np(nick);
        auto sp = stanza::iq()
            .type("set")
            .id(stanza::uuid(context))
            .pubsub(
                stanza::xep0060::pubsub().publish(
                    stanza::xep0060::publish("http://jabber.org/protocol/nick")
                        .item(stanza::xep0060::item().payload(np))
                )
            )
            .build(context);

        xmpp_stanza_clone(sp.get());
        return sp.get();
    }

} }
