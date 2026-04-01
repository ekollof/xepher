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

    /* Spoiler Messages (XEP-0382) */
    struct xep0382 {

        // <spoiler xmlns='urn:xmpp:spoiler:0'>[hint]</spoiler>
        struct spoiler : virtual public spec {
            // No-hint variant
            spoiler() : spec("spoiler") {
                xmlns<urn::xmpp::spoiler::_0>();
            }
            // With hint text
            explicit spoiler(std::string_view hint) : spec("spoiler") {
                xmlns<urn::xmpp::spoiler::_0>();
                text(hint);
            }
        };

        // stanza::message mixin
        struct message : virtual public spec {
            message() : spec("message") {}

            message& spoiler() {
                xep0382::spoiler el;
                child(el);
                return *this;
            }
            message& spoiler(std::string_view hint) {
                xep0382::spoiler el(hint);
                child(el);
                return *this;
            }
        };
    };

}
