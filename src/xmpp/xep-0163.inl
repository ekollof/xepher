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
        // Non-owning view of the optional payload; builder child() clones on build.
        std::shared_ptr<xmpp_stanza_t> payload_view;
        if (payload)
            payload_view = {payload, [](xmpp_stanza_t *) {}};

        auto item_spec = stanza::xep0060::item();
        if (item_id)
            item_spec.id(item_id);
        if (payload_view)
            item_spec.payload(payload_view);

        auto sp = stanza::iq()
            .type("set")
            .id(stanza::uuid(context))
            .pubsub(
                stanza::xep0060::pubsub().publish(
                    stanza::xep0060::publish(node).item(std::move(item_spec))
                )
            )
            .build(context);

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
