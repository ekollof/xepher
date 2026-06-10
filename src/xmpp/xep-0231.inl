// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <string>
#include <string_view>
#include <strophe.h>

// stanza::* builder types are available via the including translation unit.

namespace xmpp::xep0231 {

inline constexpr std::string_view k_bob_ns = "urn:xmpp:bob";
inline constexpr std::size_t k_max_payload_bytes = 8192;
inline constexpr std::size_t k_inline_payload_bytes = 1024;
inline constexpr std::string_view k_default_max_age = "86400";

struct data_elem : virtual public stanza::spec {
    data_elem(std::string_view cid,
              std::string_view mime,
              std::string_view b64 = {},
              std::string_view max_age = k_default_max_age)
        : spec("data")
    {
        xmlns<urn::xmpp::bob>();
        attr("cid", std::string(cid));
        if (!mime.empty())
            attr("type", std::string(mime));
        if (!max_age.empty())
            attr("max-age", std::string(max_age));
        if (!b64.empty())
            text(b64);
    }
};

struct xhtml_img : virtual public stanza::spec {
    explicit xhtml_img(std::string_view cid_url, std::string_view alt = "image")
        : spec("img")
    {
        attr("src", std::string(cid_url));
        if (!alt.empty())
            attr("alt", std::string(alt));
    }
};

struct xhtml_p : virtual public stanza::spec {
    xhtml_p() : spec("p") {}
    xhtml_p& img(xhtml_img i) { child(i); return *this; }
};

struct xhtml_body : virtual public stanza::spec {
    xhtml_body() : spec("body")
    {
        attr("xmlns", "http://www.w3.org/1999/xhtml");
    }
    xhtml_body& paragraph(xhtml_p p) { child(p); return *this; }
};

struct xhtml_im : virtual public stanza::spec {
    xhtml_im() : spec("html")
    {
        attr("xmlns", "http://jabber.org/protocol/xhtml-im");
    }
    xhtml_im& body(xhtml_body b) { child(b); return *this; }
};

// XEP-0231: request uncached BoB data from the hosting entity.
// Returns a caller-owned xmpp_stanza_t* (call xmpp_stanza_release when done).
inline xmpp_stanza_t *request_bob_data(xmpp_ctx_t *context,
                                       std::string_view to,
                                       std::string_view cid,
                                       std::string_view id)
{
    data_elem req(cid, {});
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