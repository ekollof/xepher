// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <ctime>

#include "node.hh"
#pragma GCC visibility push(default)
#include "ns.hh"
#pragma GCC visibility pop

XMPP_TEST_EXPORT std::string stanza::uuid(xmpp_ctx_t *context) {
    std::shared_ptr<char> uuid {
        xmpp_uuid_gen(context),
        [=](auto x) { xmpp_free(context, x); }
    };
    return uuid.get();
}

XMPP_TEST_EXPORT std::string get_name(xmpp_stanza_t *stanza) {
    const char *result = nullptr;
    result = xmpp_stanza_get_name(stanza);
    if (result)
        return result;
    else
        return {};
}

XMPP_TEST_EXPORT std::optional<std::string> get_attribute(xmpp_stanza_t *stanza, const char *name) {
    const char *result = nullptr;
    result = xmpp_stanza_get_attribute(stanza, name);
    if (result)
        return result;
    else
        return {};
}

XMPP_TEST_EXPORT std::string get_text(xmpp_stanza_t *stanza) {
    const char *result = nullptr;
    result = xmpp_stanza_get_text_ptr(stanza);
    if (result)
        return result;
    else
        return {};
}

XMPP_TEST_EXPORT std::string stanza_element_text(xmpp_stanza_t *stanza)
{
    if (!stanza)
        return {};
    std::unique_ptr<char, decltype(&free)> text(xmpp_stanza_get_text(stanza), free);
    return text ? std::string(text.get()) : std::string {};
}

XMPP_TEST_EXPORT std::chrono::system_clock::time_point get_time(const std::string& text) {
    std::tm tm = {};
    if (strptime(text.data(), "%FT%T%z", &tm)) {
        throw std::invalid_argument("Bad time format");
    } else {
        return std::chrono::system_clock::from_time_t(std::mktime(&tm));
    }
}

jid::jid(xmpp_ctx_t *, std::string s) : full(std::move(s)) {
    // RFC 6122 / XMPP addressing: split on the first '/' (resource), then the
    // first '@' in the bare portion (local@domain).  No regex — nicks may contain
    // apostrophes, spaces, Unicode, etc.
    std::string_view view = full;

    if (const auto slash = view.find('/'); slash != std::string_view::npos)
    {
        resource.assign(view.substr(slash + 1));
        view = view.substr(0, slash);
    }

    bare.assign(view);

    if (const auto at = view.find('@'); at != std::string_view::npos)
    {
        local.assign(view.substr(0, at));
        domain.assign(view.substr(at + 1));
    }
    else
        domain = bare;
}

bool jid::is_bare() const {
    return resource.empty();
}

xml::node::node() {}

void xml::node::bind(xmpp_ctx_t *context, xmpp_stanza_t *stanza) {
    name = get_name(stanza);

    id = get_attribute(stanza, "id");
    ns = get_attribute(stanza, "xmlns");

    int count = xmpp_stanza_get_attribute_count(stanza);
    std::vector<const char*> attrvec(count * 2, nullptr);
    const char **attrs = attrvec.data();
    xmpp_stanza_get_attributes(stanza, attrs, count * 2);
    for (int i = 0; i < count; i++) {
        const char *key = attrs[(2*i)];
        const char *value = attrs[(2*i)+1];
        attributes.emplace(key, value);
    }

    text = get_text(stanza);

    for (xmpp_stanza_t *child = xmpp_stanza_get_children(stanza);
            child; child = xmpp_stanza_get_next(child)) {
        if (xmpp_stanza_is_text(child))
            text += get_text(child);
        else
            children.emplace_back(context, child);
    }
}

void xml::message::bind(xmpp_ctx_t *context, xmpp_stanza_t *stanza) {
    auto result = get_attribute(stanza, "from");
    if (result)
        from = jid(context, *result);
    result = get_attribute(stanza, "to");
    if (result)
        to = jid(context, *result);
    type = get_attribute(stanza, "type");

    node::bind(context, stanza);
}

std::optional<std::string> xml::presence::show() {
    auto child = get_children("show");
    if (child.size() > 0)
        return child.front().get().text;
    return {};
}

std::optional<std::string> xml::presence::status() {
    auto child = get_children("status");
    if (child.size() > 0)
        return child.front().get().text;
    return {};
}

void xml::presence::bind(xmpp_ctx_t *context, xmpp_stanza_t *stanza) {
    auto result = get_attribute(stanza, "from");
    if (result)
        from = jid(context, *result);
    result = get_attribute(stanza, "to");
    if (result)
        to = jid(context, *result);
    type = get_attribute(stanza, "type");

    node::bind(context, stanza);
}

void xml::iq::bind(xmpp_ctx_t *context, xmpp_stanza_t *stanza) {
    auto result = get_attribute(stanza, "from");
    if (result)
        from = jid(context, *result);
    result = get_attribute(stanza, "to");
    if (result)
        to = jid(context, *result);
    type = get_attribute(stanza, "type");

    node::bind(context, stanza);
}

void xml::error::bind(xmpp_ctx_t *context, xmpp_stanza_t *stanza) {
    auto result = get_attribute(stanza, "from");
    if (result)
        from = jid(context, *result);
    result = get_attribute(stanza, "to");
    if (result)
        to = jid(context, *result);

    node::bind(context, stanza);
}
