// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <strophe.h>
#include "../strophe.hh"

namespace xmpp { namespace xep0410 {

    // XEP-0410: MUC Self-Ping (Schrödinger's Chat)
    
    // Send a self-ping to our own MUC nickname
    // to: full MUC JID (room@server/nickname)
    inline xmpp_stanza_t *muc_self_ping(xmpp_ctx_t *context, const char *muc_jid)
    {
        xmpp_string_guard id(context, xmpp_uuid_gen(context));
        xmpp_stanza_t *iq = xmpp_iq_new(context, "get", id.c_str());
        xmpp_stanza_set_to(iq, muc_jid);
        
        xmpp_stanza_t *ping = xmpp_stanza_new(context);
        xmpp_stanza_set_name(ping, "ping");
        xmpp_stanza_set_ns(ping, "urn:xmpp:ping");
        
        xmpp_stanza_add_child(iq, ping);
        xmpp_stanza_release(ping);
        
        return iq;
    }

} }
