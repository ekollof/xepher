// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <strophe.h>

namespace xmpp { namespace xep0191 {

    // XEP-0191: Blocking Command

    // Request block list
    inline xmpp_stanza_t *blocklist_request(xmpp_ctx_t *context)
    {
        xmpp_stanza_t *iq = xmpp_iq_new(context, "get", xmpp_uuid_gen(context));
        
        xmpp_stanza_t *blocklist = xmpp_stanza_new(context);
        xmpp_stanza_set_name(blocklist, "blocklist");
        xmpp_stanza_set_ns(blocklist, "urn:xmpp:blocking");
        
        xmpp_stanza_add_child(iq, blocklist);
        xmpp_stanza_release(blocklist);
        
        return iq;
    }

    // Block one or more JIDs
    inline xmpp_stanza_t *block_jid(xmpp_ctx_t *context, const char **jids, int count)
    {
        xmpp_stanza_t *iq = xmpp_iq_new(context, "set", xmpp_uuid_gen(context));
        
        xmpp_stanza_t *block = xmpp_stanza_new(context);
        xmpp_stanza_set_name(block, "block");
        xmpp_stanza_set_ns(block, "urn:xmpp:blocking");
        
        for (int i = 0; i < count; i++)
        {
            xmpp_stanza_t *item = xmpp_stanza_new(context);
            xmpp_stanza_set_name(item, "item");
            xmpp_stanza_set_attribute(item, "jid", jids[i]);
            xmpp_stanza_add_child(block, item);
            xmpp_stanza_release(item);
        }
        
        xmpp_stanza_add_child(iq, block);
        xmpp_stanza_release(block);
        
        return iq;
    }

    // Unblock one or more JIDs (or all if count == 0)
    inline xmpp_stanza_t *unblock_jid(xmpp_ctx_t *context, const char **jids, int count)
    {
        xmpp_stanza_t *iq = xmpp_iq_new(context, "set", xmpp_uuid_gen(context));
        
        xmpp_stanza_t *unblock = xmpp_stanza_new(context);
        xmpp_stanza_set_name(unblock, "unblock");
        xmpp_stanza_set_ns(unblock, "urn:xmpp:blocking");
        
        if (count > 0)
        {
            for (int i = 0; i < count; i++)
            {
                xmpp_stanza_t *item = xmpp_stanza_new(context);
                xmpp_stanza_set_name(item, "item");
                xmpp_stanza_set_attribute(item, "jid", jids[i]);
                xmpp_stanza_add_child(unblock, item);
                xmpp_stanza_release(item);
            }
        }
        // Empty unblock = unblock all
        
        xmpp_stanza_add_child(iq, unblock);
        xmpp_stanza_release(unblock);
        
        return iq;
    }

} }
