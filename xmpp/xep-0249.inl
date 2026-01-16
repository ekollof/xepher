// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

namespace stanza {

    // XEP-0249: Direct MUC Invitations
    struct xep0249 {
        
        // Send a direct invitation to join a MUC room
        struct x : virtual public spec {
            x(const char *room_jid,
              const char *password = nullptr,
              const char *reason = nullptr) : spec("x") {
                attr("xmlns", "jabber:x:conference");
                attr("jid", room_jid);
                if (password)
                    attr("password", password);
                if (reason)
                    attr("reason", reason);
            }
        };

    };

}
