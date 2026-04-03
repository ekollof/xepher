// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <strophe.h>

// stanza::* builder types are available via the including translation unit.

namespace xmpp { namespace xep0410 {

    // XEP-0410: MUC Self-Ping (Schrödinger's Chat)
    
    // Send a self-ping to our own MUC nickname.
    // muc_jid: full MUC JID (room@server/nickname)
    // Returns a caller-owned xmpp_stanza_t* (call xmpp_stanza_release when done).
    inline xmpp_stanza_t *muc_self_ping(xmpp_ctx_t *context, const char *muc_jid)
    {
        auto sp = stanza::iq()
            .type("get")
            .id(stanza::uuid(context))
            .to(muc_jid)
            .ping()
            .build(context);

        xmpp_stanza_clone(sp.get());
        return sp.get();
    }

} }
