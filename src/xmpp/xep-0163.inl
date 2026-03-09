// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <strophe.h>
#include "../strophe.hh"

namespace xmpp { namespace xep0163 {

    // XEP-0163: Personal Eventing Protocol (PEP)
    // PEP is simplified PubSub sent to user's bare JID
    
    // Generic PEP publish - publishes an item to a PEP node
    // The 'to' address should be empty (defaults to user's own bare JID)
    inline xmpp_stanza_t *publish_pep(xmpp_ctx_t *context, const char *node, 
                                      xmpp_stanza_t *payload, const char *item_id = nullptr)
    {
        xmpp_string_guard id(context, xmpp_uuid_gen(context));
        xmpp_stanza_t *iq = xmpp_iq_new(context, "set", id.c_str());
        
        xmpp_stanza_t *pubsub = xmpp_stanza_new(context);
        xmpp_stanza_set_name(pubsub, "pubsub");
        xmpp_stanza_set_ns(pubsub, "http://jabber.org/protocol/pubsub");
        
        xmpp_stanza_t *publish = xmpp_stanza_new(context);
        xmpp_stanza_set_name(publish, "publish");
        xmpp_stanza_set_attribute(publish, "node", node);
        
        xmpp_stanza_t *item = xmpp_stanza_new(context);
        xmpp_stanza_set_name(item, "item");
        if (item_id)
            xmpp_stanza_set_attribute(item, "id", item_id);
        
        if (payload)
        {
            xmpp_stanza_add_child(item, payload);
        }
        
        xmpp_stanza_add_child(publish, item);
        xmpp_stanza_release(item);
        
        xmpp_stanza_add_child(pubsub, publish);
        xmpp_stanza_release(publish);
        
        xmpp_stanza_add_child(iq, pubsub);
        xmpp_stanza_release(pubsub);
        
        return iq;
    }
    
    // Subscribe to a PEP node (usually automatic via roster)
    inline xmpp_stanza_t *subscribe_pep(xmpp_ctx_t *context, const char *node, 
                                         const char *jid)
    {
        xmpp_string_guard id(context, xmpp_uuid_gen(context));
        xmpp_stanza_t *iq = xmpp_iq_new(context, "set", id.c_str());
        xmpp_stanza_set_to(iq, jid);
        
        xmpp_stanza_t *pubsub = xmpp_stanza_new(context);
        xmpp_stanza_set_name(pubsub, "pubsub");
        xmpp_stanza_set_ns(pubsub, "http://jabber.org/protocol/pubsub");
        
        xmpp_stanza_t *subscribe = xmpp_stanza_new(context);
        xmpp_stanza_set_name(subscribe, "subscribe");
        xmpp_stanza_set_attribute(subscribe, "node", node);
        xmpp_stanza_set_attribute(subscribe, "jid", jid);
        
        xmpp_stanza_add_child(pubsub, subscribe);
        xmpp_stanza_release(subscribe);
        
        xmpp_stanza_add_child(iq, pubsub);
        xmpp_stanza_release(pubsub);
        
        return iq;
    }

} }
