// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

// XEP-0292: vCard4 Over XMPP
// Namespace: urn:ietf:params:xml:ns:vcard-4.0
// vCard4 is published via PubSub (node urn:xmpp:vcard4) or requested via IQ.

#include <strophe.h>
#include "../strophe.hh"

#define NS_VCARD4 "urn:ietf:params:xml:ns:vcard-4.0"
#define NS_VCARD4_PUBSUB "urn:xmpp:vcard4"

namespace xmpp { namespace xep0292 {

    // Build an IQ get to retrieve a contact's vCard4 via PubSub items request.
    inline xmpp_stanza_t *vcard4_request(xmpp_ctx_t *context, const char *to)
    {
        xmpp_string_guard id(context, xmpp_uuid_gen(context));
        xmpp_stanza_t *iq = xmpp_iq_new(context, "get", id.c_str());
        if (to) xmpp_stanza_set_to(iq, to);

        xmpp_stanza_t *pubsub = xmpp_stanza_new(context);
        xmpp_stanza_set_name(pubsub, "pubsub");
        xmpp_stanza_set_ns(pubsub, "http://jabber.org/protocol/pubsub");

        xmpp_stanza_t *items = xmpp_stanza_new(context);
        xmpp_stanza_set_name(items, "items");
        xmpp_stanza_set_attribute(items, "node", NS_VCARD4_PUBSUB);
        xmpp_stanza_add_child(pubsub, items);
        xmpp_stanza_release(items);

        xmpp_stanza_add_child(iq, pubsub);
        xmpp_stanza_release(pubsub);

        return iq;
    }

} }
