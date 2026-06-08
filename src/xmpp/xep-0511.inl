// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <string_view>

#include "node.hh"

namespace stanza {

    /* Link Metadata (XEP-0511) — OpenGraph in <rdf:Description>. */

    struct xep0511 {

        struct rdf_description : virtual public spec {
            explicit rdf_description(std::string_view about_url) : spec("rdf:Description") {
                attr("xmlns:rdf", "http://www.w3.org/1999/02/22-rdf-syntax-ns#");
                attr("xmlns:og",  "https://ogp.me/ns#");
                attr("rdf:about", about_url);
            }

            rdf_description& og(std::string_view elem_name, std::string_view value) {
                if (value.empty()) return *this;
                struct og_el : virtual public spec {
                    og_el(std::string_view n, std::string_view v) : spec(n) { text(v); }
                };
                child(og_el{elem_name, value});
                return *this;
            }
        };
    };

} // namespace stanza