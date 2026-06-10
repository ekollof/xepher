// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <string>
#include <strophe.h>

// stanza::* builder types are available via the including translation unit.

namespace xmpp::xep0231 {

inline constexpr std::string_view k_bob_ns = "urn:xmpp:bob";

// XEP-0231: request uncached BoB data from the hosting entity.
// Returns a caller-owned xmpp_stanza_t* (call xmpp_stanza_release when done).
inline xmpp_stanza_t *request_bob_data(xmpp_ctx_t *context,
                                       std::string_view to,
                                       std::string_view cid,
                                       std::string_view id)
{
    struct bob_data_req : virtual public stanza::spec {
        explicit bob_data_req(std::string_view cid_value) : spec("data")
        {
            xmlns<urn::xmpp::bob>();
            attr("cid", std::string(cid_value));
        }
    };

    bob_data_req req(cid);
    stanza::iq iq_spec;
    iq_spec.type("get")
        .id(std::string(id))
        .to(std::string(to));
    iq_spec.child(req);
    auto sp = iq_spec.build(context);

    xmpp_stanza_clone(sp.get());
    return sp.get();
}

}  // namespace xmpp::xep0231