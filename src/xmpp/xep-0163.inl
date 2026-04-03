// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <strophe.h>

// stanza::* builder types are available because node.hh (which includes all
// xep-NNNN.inl files) is included before this file in every translation unit.

namespace xmpp { namespace xep0163 {

    // XEP-0163: Personal Eventing Protocol (PEP)
    // PEP is simplified PubSub sent to user's bare JID.

    // Generic PEP publish — publishes an item to a PEP node.
    //
    // payload: an already-allocated xmpp_stanza_t* that will be added as a
    //          child of <item> (may be nullptr).  The caller retains ownership
    //          of payload — this function does not release it.
    // item_id: optional PubSub item id attribute.
    //
    // Returns a caller-owned xmpp_stanza_t* (call xmpp_stanza_release when done).
    inline xmpp_stanza_t *publish_pep(xmpp_ctx_t *context, const char *node,
                                      xmpp_stanza_t *payload,
                                      const char *item_id = nullptr)
    {
        // Build the skeleton with an empty <item>; attach payload and item_id
        // after the tree is materialised (builder does not wrap raw stanzas).
        auto sp = stanza::iq()
            .type("set")
            .id(stanza::uuid(context))
            .pubsub(
                stanza::xep0060::pubsub().publish(
                    stanza::xep0060::publish(node).item(
                        stanza::xep0060::item()
                    )
                )
            )
            .build(context);

        // Walk the built tree to attach the optional item_id and payload.
        xmpp_stanza_t *ps  = xmpp_stanza_get_child_by_name(sp.get(), "pubsub");
        xmpp_stanza_t *pub = ps  ? xmpp_stanza_get_child_by_name(ps,  "publish") : nullptr;
        xmpp_stanza_t *it  = pub ? xmpp_stanza_get_child_by_name(pub, "item")    : nullptr;
        if (it)
        {
            if (item_id)
                xmpp_stanza_set_attribute(it, "id", item_id);
            if (payload)
                xmpp_stanza_add_child(it, payload);
        }

        xmpp_stanza_clone(sp.get());  // bump refcount; shared_ptr dtor drops its ref
        return sp.get();              // caller owns one reference
    }

    // Subscribe to a PEP node (usually automatic via roster).
    // Returns a caller-owned xmpp_stanza_t* (call xmpp_stanza_release when done).
    inline xmpp_stanza_t *subscribe_pep(xmpp_ctx_t *context, const char *node,
                                        const char *jid)
    {
        auto sp = stanza::iq()
            .type("set")
            .id(stanza::uuid(context))
            .to(jid)
            .pubsub(
                stanza::xep0060::pubsub().subscribe(
                    stanza::xep0060::subscribe(node, jid)
                )
            )
            .build(context);

        xmpp_stanza_clone(sp.get());
        return sp.get();
    }

} }
