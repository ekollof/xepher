// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <optional>
#include <string_view>

#include "node.hh"
#pragma GCC visibility push(default)
#include "ns.hh"
#pragma GCC visibility pop

namespace stanza {

    /* MUC Administration (XEP-0045 muc#admin) — outbound IQ builders */
    struct xep0045admin {

        // <reason>text</reason>
        struct reason : virtual public spec {
            explicit reason(std::string_view text_) : spec("reason") {
                text(text_);
            }
        };

        // <item nick='N' role='none'/>  (kick)
        struct item_by_nick : virtual public spec {
            explicit item_by_nick(std::string_view nick, std::string_view role)
                : spec("item") {
                attr("nick", nick);
                attr("role", role);
            }
            item_by_nick& reason(std::string_view r) {
                xep0045admin::reason rel(r);
                child(rel);
                return *this;
            }
        };

        // <item jid='J' affiliation='outcast'/>  (ban / affiliation set)
        struct item_by_jid : virtual public spec {
            explicit item_by_jid(std::string_view jid, std::string_view affiliation)
                : spec("item") {
                attr("jid", jid);
                attr("affiliation", affiliation);
            }
            // XEP-0045 §9.4 / §1.35.4: optional reserved room nick on member modify;
            // empty string unsets a reserved nickname.
            item_by_jid& nick(std::string_view n) {
                attr("nick", n);
                return *this;
            }
            item_by_jid& reason(std::string_view r) {
                xep0045admin::reason rel(r);
                child(rel);
                return *this;
            }
        };

        // For retrieving affiliation lists (XEP-0045 §9.3) to discover real JIDs
        // for MUC OMEMO (plan 2.2 admin fallback). Usage:
        //   xep0045admin::item_by_affiliation it("member");
        //   q.item(it);
        struct item_by_affiliation : virtual public spec {
            explicit item_by_affiliation(std::string_view affiliation)
                : spec("item") {
                attr("affiliation", affiliation);
            }
        };

        // <query xmlns='http://jabber.org/protocol/muc#admin'>…</query>
        struct query : virtual public spec {
            query() : spec("query") {
                xmlns<jabber_org::protocol::muc::admin>();
            }
            query& item(item_by_nick& it) { child(it); return *this; }
            query& item(item_by_jid& it)  { child(it); return *this; }
            query& item(item_by_affiliation& it) { child(it); return *this; }
        };

        // stanza::iq mixin
        struct iq : virtual public spec {
            iq() : spec("iq") {}

            iq& muc_admin(query& q) { child(q); return *this; }
        };
    };

}
