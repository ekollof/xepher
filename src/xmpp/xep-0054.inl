// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <optional>
#include <string>
#include <strophe.h>
#include "../strophe.hh"

namespace xmpp { namespace xep0054 {

    // XEP-0054: vcard-temp — build a vCard IQ get request
    inline xmpp_stanza_t *vcard_request(xmpp_ctx_t *context, const char *to)
    {
        xmpp_string_guard id(context, xmpp_uuid_gen(context));
        xmpp_stanza_t *iq = xmpp_iq_new(context, "get", id.c_str());
        if (to)
            xmpp_stanza_set_to(iq, to);

        xmpp_stanza_t *vcard = xmpp_stanza_new(context);
        xmpp_stanza_set_name(vcard, "vCard");
        xmpp_stanza_set_ns(vcard, "vcard-temp");

        xmpp_stanza_add_child(iq, vcard);
        xmpp_stanza_release(vcard);

        return iq;
    }

    // Helper: append a simple text child element to parent if value is non-empty.
    inline void _append_text_child(xmpp_ctx_t *ctx, xmpp_stanza_t *parent,
                                   const char *name, const std::string &value)
    {
        if (value.empty()) return;
        xmpp_stanza_t *el  = xmpp_stanza_new(ctx);
        xmpp_stanza_set_name(el, name);
        xmpp_stanza_t *txt = xmpp_stanza_new(ctx);
        xmpp_stanza_set_text(txt, value.c_str());
        xmpp_stanza_add_child(el, txt);
        xmpp_stanza_release(txt);
        xmpp_stanza_add_child(parent, el);
        xmpp_stanza_release(el);
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

    inline xmpp_stanza_t *vcard_set(xmpp_ctx_t *context, const vcard_fields &f)
    {
        xmpp_string_guard id(context, xmpp_uuid_gen(context));
        xmpp_stanza_t *iq = xmpp_iq_new(context, "set", id.c_str());

        xmpp_stanza_t *vcard = xmpp_stanza_new(context);
        xmpp_stanza_set_name(vcard, "vCard");
        xmpp_stanza_set_ns(vcard, "vcard-temp");

        if (f.fn)       _append_text_child(context, vcard, "FN",       *f.fn);
        if (f.nickname) _append_text_child(context, vcard, "NICKNAME", *f.nickname);
        if (f.url)      _append_text_child(context, vcard, "URL",      *f.url);
        if (f.desc)     _append_text_child(context, vcard, "DESC",     *f.desc);
        if (f.title)    _append_text_child(context, vcard, "TITLE",    *f.title);
        if (f.bday)     _append_text_child(context, vcard, "BDAY",     *f.bday);
        if (f.note)     _append_text_child(context, vcard, "NOTE",     *f.note);

        if (f.email && !f.email->empty())
        {
            xmpp_stanza_t *email_el = xmpp_stanza_new(context);
            xmpp_stanza_set_name(email_el, "EMAIL");
            _append_text_child(context, email_el, "USERID", *f.email);
            xmpp_stanza_add_child(vcard, email_el);
            xmpp_stanza_release(email_el);
        }

        if (f.tel && !f.tel->empty())
        {
            xmpp_stanza_t *tel_el = xmpp_stanza_new(context);
            xmpp_stanza_set_name(tel_el, "TEL");
            _append_text_child(context, tel_el, "NUMBER", *f.tel);
            xmpp_stanza_add_child(vcard, tel_el);
            xmpp_stanza_release(tel_el);
        }

        if (f.org && !f.org->empty())
        {
            xmpp_stanza_t *org_el = xmpp_stanza_new(context);
            xmpp_stanza_set_name(org_el, "ORG");
            _append_text_child(context, org_el, "ORGNAME", *f.org);
            xmpp_stanza_add_child(vcard, org_el);
            xmpp_stanza_release(org_el);
        }

        xmpp_stanza_add_child(iq, vcard);
        xmpp_stanza_release(vcard);

        return iq;
    }

} }
