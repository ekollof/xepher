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
    inline xmpp_stanza_t *publish_nick(xmpp_ctx_t *context, const char *nick)
    {
        // Build the <nick> payload element
        xmpp_stanza_t *nick_elem = xmpp_stanza_new(context);
        xmpp_stanza_set_name(nick_elem, "nick");
        xmpp_stanza_set_ns(nick_elem, "http://jabber.org/protocol/nick");

        if (nick && *nick)
        {
            xmpp_stanza_t *nick_text = xmpp_stanza_new(context);
            xmpp_stanza_set_text(nick_text, nick);
            xmpp_stanza_add_child(nick_elem, nick_text);
            xmpp_stanza_release(nick_text);
        }

        // Use the generic PEP publish helper from xep-0163
        xmpp_stanza_t *iq = xep0163::publish_pep(
            context,
            "http://jabber.org/protocol/nick",
            nick_elem);

        xmpp_stanza_release(nick_elem);

        return iq;
    }

} }
