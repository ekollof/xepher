// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "stanza_view.hh"

#include <algorithm>
#include <array>
#include <string>

#include "node.hh"
#include "test_export.hh"

namespace xmpp {

namespace {

constexpr std::size_t k_nul_term_cap = 256;

// NUL-terminate a string_view for libstrophe C APIs without heap allocation
// for typical short element/attribute names and namespaces.
class nul_term_view {
    std::array<char, k_nul_term_cap> stack_{};
    std::string heap_;
    const char *ptr_ = nullptr;

public:
    explicit nul_term_view(std::string_view sv)
    {
        if (sv.size() + 1 <= stack_.size())
        {
            std::ranges::copy(sv, stack_.begin());
            stack_[sv.size()] = '\0';
            ptr_ = stack_.data();
        }
        else
        {
            heap_.assign(sv);
            ptr_ = heap_.c_str();
        }
    }

    [[nodiscard]] const char *c_str() const { return ptr_; }
};

}  // namespace

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
    const char *v = xmpp_stanza_get_attribute(stanza_, nul_term_view(name).c_str());
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
    return StanzaView(xmpp_stanza_get_child_by_name(stanza_, nul_term_view(name).c_str()));
}

StanzaView StanzaView::child(std::string_view name, std::string_view ns) const
{
    if (!stanza_)
        return StanzaView(nullptr);
    const nul_term_view name_nul(name);
    const nul_term_view ns_nul(ns);
    return StanzaView(xmpp_stanza_get_child_by_name_and_ns(
        stanza_, name_nul.c_str(), ns_nul.c_str()));
}

std::string StanzaView::text() const
{
    return stanza_element_text(stanza_);
}

}  // namespace xmpp