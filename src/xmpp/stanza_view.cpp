// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "stanza_view.hh"

#include "node.hh"
#include "test_export.hh"

namespace xmpp {

XMPP_TEST_EXPORT bool StanzaView::is_text() const
{
    return stanza_ && xmpp_stanza_is_text(stanza_);
}

std::string_view StanzaView::name() const
{
    if (!stanza_)
        return {};
    const char *n = xmpp_stanza_get_name(stanza_);
    return n ? std::string_view(n) : std::string_view {};
}

std::optional<std::string_view> StanzaView::xmlns() const
{
    if (!stanza_)
        return std::nullopt;
    const char *ns = xmpp_stanza_get_ns(stanza_);
    if (!ns)
        return std::nullopt;
    return std::string_view(ns);
}

std::optional<std::string_view> StanzaView::attr(std::string_view name) const
{
    if (!stanza_)
        return std::nullopt;
    const char *v = xmpp_stanza_get_attribute(stanza_, std::string(name).c_str());
    if (!v)
        return std::nullopt;
    return std::string_view(v);
}

std::string StanzaView::attr_string(std::string_view name) const
{
    if (auto value = attr(name))
        return std::string(*value);
    return {};
}

StanzaView StanzaView::child(std::string_view name) const
{
    if (!stanza_)
        return StanzaView(nullptr);
    return StanzaView(xmpp_stanza_get_child_by_name(stanza_, std::string(name).c_str()));
}

StanzaView StanzaView::child(std::string_view name, std::string_view ns) const
{
    if (!stanza_)
        return StanzaView(nullptr);
    return StanzaView(xmpp_stanza_get_child_by_name_and_ns(
        stanza_, std::string(name).c_str(), std::string(ns).c_str()));
}

std::string StanzaView::text() const
{
    return stanza_element_text(stanza_);
}

}  // namespace xmpp