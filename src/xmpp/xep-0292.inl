// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

// XEP-0292: vCard4 Over XMPP
// Namespace: urn:ietf:params:xml:ns:vcard-4.0
// vCard4 is published via PubSub (node urn:xmpp:vcard4) or requested via IQ.

#include <strophe.h>

#define NS_VCARD4 "urn:ietf:params:xml:ns:vcard-4.0"
#define NS_VCARD4_PUBSUB "urn:xmpp:vcard4"

// stanza::* builder types are available via the including translation unit.

namespace xmpp { namespace xep0292 {

    // Build an IQ get to retrieve a contact's vCard4 via PubSub items request.
    // Returns a caller-owned xmpp_stanza_t* (call xmpp_stanza_release when done).
    inline xmpp_stanza_t *vcard4_request(xmpp_ctx_t *context, const char *to)
    {
        auto b = stanza::iq()
            .type("get")
            .id(stanza::uuid(context));
        if (to) b.to(to);
        b.pubsub(
            stanza::xep0060::pubsub().items(
                stanza::xep0060::items("urn:xmpp:vcard4")
            )
        );
        auto sp = b.build(context);
        xmpp_stanza_clone(sp.get());
        return sp.get();
    }

} }
