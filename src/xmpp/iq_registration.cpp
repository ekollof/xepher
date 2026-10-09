// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <algorithm>
#include <ranges>
#include "xmpp/iq_registration.hh"

namespace xmpp {
auto registration_form(StanzaView query, std::string_view username, std::string_view password)
    -> std::expected<data_form, std::string>
{
    const auto x = query.child("x", "jabber:x:data");
    if (query.child("captcha", "urn:xmpp:captcha").valid())
        return std::unexpected("CAPTCHA registration is unsupported; use the server's web registration");
    for (auto field : x) {
        if (field.child("media", "urn:xmpp:media-element").valid())
            return std::unexpected("Registration requires a media challenge; use the server's web registration");
    }
    auto form = parse_data_form(x);
    if (!form) return std::unexpected(form.error());
    if (form->type != "form") return std::unexpected("Expected a registration input form");
    for (auto &field : form->fields) {
        if (field.var == "FORM_TYPE" && std::ranges::contains(field.values, "urn:xmpp:captcha"))
            return std::unexpected("CAPTCHA registration is unsupported; use the server's web registration");
        if (!field.editable()) continue;
        if (field.var == "password") field.type = "text-private";
        if ((field.var == "username" || field.var == "password")
            && (field.values.empty() || (field.values.size() == 1 && field.values.front().empty()))) {
            const std::vector<std::string> values{std::string(field.var == "username" ? username : password)};
            auto updated = set_data_form_values(field, values);
            if (!updated) return std::unexpected(updated.error());
        }
    }
    return form;
}

auto registration_submission(const data_form &form) -> std::expected<stanza::spec, std::string>
{
    auto submission = submit_data_form(form);
    if (!submission) return std::unexpected(submission.error());
    struct query_spec : stanza::spec {
        query_spec() : spec("query") { attr("xmlns", "jabber:iq:register"); }
    } query;
    query.child(*submission);
    return query;
}
}
