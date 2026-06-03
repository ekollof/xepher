// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <optional>
#include <string>

#include "node.hh"
#pragma GCC visibility push(default)
#include "ns.hh"
#pragma GCC visibility pop

namespace xml {

    /* Current Jabber OpenPGP Usage */
    class xep0027 : virtual public node {
    private:
        std::optional<std::optional<std::string>> _signature;
        std::optional<std::optional<std::string>> _encrypted;
    public:
        std::optional<std::string>& signature() {
            if (!_signature)
            {
                auto child = get_children<jabber::x::signed_>("x");
                if (child.size() > 0)
                    _signature = child.front().get().text;
                else
                    _signature.emplace(std::nullopt);
            }
            return *_signature;
        }

        std::optional<std::string>& encrypted() {
            if (!_encrypted)
            {
                auto child = get_children<jabber::x::encrypted>("x");
                if (child.size() > 0)
                    _encrypted = child.front().get().text;
                else
                    _encrypted.emplace(std::nullopt);
            }
            return *_encrypted;
        }
    };

}

namespace stanza {

    /* Current Jabber OpenPGP Usage (XEP-0027) — stanza builder side */
    struct xep0027 {

        // <x xmlns='jabber:x:encrypted'>ciphertext</x>
        struct encrypted : virtual public spec {
            encrypted(std::string_view ciphertext) : spec("x") {
                xmlns<jabber::x::encrypted>();
                text(ciphertext);
            }
        };

        // stanza::message mixin
        struct message : virtual public spec {
            message() : spec("message") {}

            message& pgp_encrypted(xep0027::encrypted e) { child(e); return *this; }
        };
    };

}
