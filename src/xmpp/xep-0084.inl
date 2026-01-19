// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <strophe.h>

namespace weechat::xep0084
{
    // XEP-0084: User Avatar (PEP-based)
    // Namespace constants
    inline constexpr const char *METADATA_NS = "urn:xmpp:avatar:metadata";
    inline constexpr const char *DATA_NS = "urn:xmpp:avatar:data";
    
    // Request avatar data from PubSub
    inline xmpp_stanza_t *request_avatar_data(xmpp_ctx_t *context, 
                                              const char *to, 
                                              const char *id)
    {
        xmpp_stanza_t *iq = xmpp_iq_new(context, "get", id);
        xmpp_stanza_set_to(iq, to);
        
        xmpp_stanza_t *pubsub = xmpp_stanza_new(context);
        xmpp_stanza_set_name(pubsub, "pubsub");
        xmpp_stanza_set_ns(pubsub, "http://jabber.org/protocol/pubsub");
        
        xmpp_stanza_t *items = xmpp_stanza_new(context);
        xmpp_stanza_set_name(items, "items");
        xmpp_stanza_set_attribute(items, "node", DATA_NS);
        
        xmpp_stanza_add_child(pubsub, items);
        xmpp_stanza_add_child(iq, pubsub);
        
        xmpp_stanza_release(items);
        xmpp_stanza_release(pubsub);
        
        return iq;
    }
}
