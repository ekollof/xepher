// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "iq_vcard.hh"

#include <algorithm>
#include <cctype>
#include <ranges>

namespace xmpp {

namespace {

[[nodiscard]] std::string child_text(const StanzaView parent, const std::string_view name)
{
    const StanzaView child = parent.child(std::string(name));
    return child.valid() ? child.text() : std::string {};
}

}  // namespace

vcard_temp_fields vcard_fields_from_stanza(const StanzaView vcard)
{
    vcard_temp_fields f;
    if (!vcard.valid())
        return f;

    f.fn       = child_text(vcard, "FN");
    f.nickname = child_text(vcard, "NICKNAME");
    f.url      = child_text(vcard, "URL");
    f.desc     = child_text(vcard, "DESC");
    f.bday     = child_text(vcard, "BDAY");
    f.note     = child_text(vcard, "NOTE");
    f.title    = child_text(vcard, "TITLE");

    const StanzaView org_el = vcard.child("ORG");
    if (org_el.valid())
        f.org = child_text(org_el, "ORGNAME");

    const StanzaView email_el = vcard.child("EMAIL");
    if (email_el.valid())
        f.email = child_text(email_el, "USERID");

    const StanzaView tel_el = vcard.child("TEL");
    if (tel_el.valid())
        f.tel = child_text(tel_el, "NUMBER");

    return f;
}

bool apply_vcard_set_field_override(
    vcard_temp_fields &fields,
    const std::string_view field,
    const std::string_view value)
{
    const std::string val(value);
    if (field == "fn")       { fields.fn       = val; return true; }
    if (field == "nickname") { fields.nickname = val; return true; }
    if (field == "email")    { fields.email    = val; return true; }
    if (field == "url")      { fields.url      = val; return true; }
    if (field == "desc")     { fields.desc     = val; return true; }
    if (field == "org")      { fields.org      = val; return true; }
    if (field == "title")    { fields.title    = val; return true; }
    if (field == "tel")      { fields.tel      = val; return true; }
    if (field == "bday")     { fields.bday     = val; return true; }
    if (field == "note")     { fields.note     = val; return true; }
    return false;
}

std::string format_vcard_temp_adr(const StanzaView adr)
{
    if (!adr.valid())
        return {};

    std::string out;
    static constexpr std::string_view k_parts[] = {
        "STREET", "LOCALITY", "REGION", "PCODE", "CTRY",
    };
    for (const std::string_view part : k_parts)
    {
        const std::string p = child_text(adr, part);
        if (p.empty())
            continue;
        if (!out.empty())
            out += ", ";
        out += p;
    }
    return out;
}

bool is_vcard4_pubsub_node(const std::string_view node)
{
    return node == "urn:xmpp:vcard4";
}

}  // namespace xmpp