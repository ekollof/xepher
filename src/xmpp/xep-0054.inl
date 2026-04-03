// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <optional>
#include <string>
#include <strophe.h>

// stanza::* builder types are available via the including translation unit.

namespace xmpp { namespace xep0054 {

    // XEP-0054: vcard-temp — build a vCard IQ get request.
    // Returns a caller-owned xmpp_stanza_t* (call xmpp_stanza_release when done).
    inline xmpp_stanza_t *vcard_request(xmpp_ctx_t *context, const char *to)
    {
        // <vCard xmlns='vcard-temp'/>
        struct vcard_spec : virtual public stanza::spec {
            vcard_spec() : spec("vCard") {
                attr("xmlns", "vcard-temp");
            }
        };

        auto b = stanza::iq()
            .type("get")
            .id(stanza::uuid(context));
        if (to) b.to(to);

        vcard_spec vc;
        // stanza::iq doesn't have a vcard child method; add via pubsub-like
        // workaround: build iq, then post-attach vCard.
        auto sp = b.build(context);
        auto vc_sp = vc.build(context);
        xmpp_stanza_add_child(sp.get(), vc_sp.get());

        xmpp_stanza_clone(sp.get());
        return sp.get();
    }

    // XEP-0054: vcard-temp — build a vCard IQ set (publish) request.
    // Pass std::nullopt for fields you do not want to set/overwrite.
    struct vcard_fields {
        std::optional<std::string> fn;
        std::optional<std::string> nickname;
        std::optional<std::string> email;    // goes in <EMAIL><USERID>
        std::optional<std::string> url;
        std::optional<std::string> desc;
        std::optional<std::string> org;      // goes in <ORG><ORGNAME>
        std::optional<std::string> title;
        std::optional<std::string> tel;      // goes in <TEL><NUMBER>
        std::optional<std::string> bday;
        std::optional<std::string> note;
    };

    // Returns a caller-owned xmpp_stanza_t* (call xmpp_stanza_release when done).
    inline xmpp_stanza_t *vcard_set(xmpp_ctx_t *context, const vcard_fields &f)
    {
        // Build <vCard xmlns='vcard-temp'> inline using post-build tree mutation.
        // The builder handles simple text children; for nested wrappers like
        // <EMAIL><USERID>x</USERID></EMAIL> we use spec types below.

        // Simple text-child element: <NAME>value</NAME>
        struct text_el : virtual public stanza::spec {
            text_el(std::string_view name, std::string_view val)
                : spec(std::string(name)) { text(val); }
        };

        // Wrapper with one text-child: <OUTER><INNER>value</INNER></OUTER>
        struct wrapped_el : virtual public stanza::spec {
            wrapped_el(std::string_view outer, std::string_view inner,
                       std::string_view val)
                : spec(std::string(outer))
            {
                text_el te(inner, val);
                child(te);
            }
        };

        struct vcard_spec : virtual public stanza::spec {
            vcard_spec(const vcard_fields &f) : spec("vCard") {
                attr("xmlns", "vcard-temp");
                if (f.fn)       { text_el e("FN",       *f.fn);       child(e); }
                if (f.nickname) { text_el e("NICKNAME",  *f.nickname); child(e); }
                if (f.url)      { text_el e("URL",       *f.url);      child(e); }
                if (f.desc)     { text_el e("DESC",      *f.desc);     child(e); }
                if (f.title)    { text_el e("TITLE",     *f.title);    child(e); }
                if (f.bday)     { text_el e("BDAY",      *f.bday);     child(e); }
                if (f.note)     { text_el e("NOTE",      *f.note);     child(e); }
                if (f.email && !f.email->empty()) {
                    wrapped_el e("EMAIL", "USERID", *f.email); child(e);
                }
                if (f.tel && !f.tel->empty()) {
                    wrapped_el e("TEL", "NUMBER", *f.tel); child(e);
                }
                if (f.org && !f.org->empty()) {
                    wrapped_el e("ORG", "ORGNAME", *f.org); child(e);
                }
            }
        };

        auto sp = stanza::iq()
            .type("set")
            .id(stanza::uuid(context))
            .build(context);

        vcard_spec vc(f);
        auto vc_sp = vc.build(context);
        xmpp_stanza_add_child(sp.get(), vc_sp.get());

        xmpp_stanza_clone(sp.get());
        return sp.get();
    }

} }
