// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <iterator>
#include <optional>
#include <string_view>
#include <strophe.h>

#include "../test_export.hh"

namespace xmpp {

// Non-owning read-only view over an inbound libstrophe stanza.
class StanzaView {
public:
    explicit StanzaView(xmpp_stanza_t *stanza = nullptr) : stanza_(stanza) {}

    [[nodiscard]] bool valid() const { return stanza_ != nullptr; }
    [[nodiscard]] xmpp_stanza_t *raw() const { return stanza_; }

    [[nodiscard]] XMPP_TEST_EXPORT std::string_view name() const;
    [[nodiscard]] XMPP_TEST_EXPORT std::optional<std::string_view> attr(std::string_view name) const;
    [[nodiscard]] XMPP_TEST_EXPORT std::string attr_string(std::string_view name) const;
    [[nodiscard]] std::optional<std::string_view> from() const { return attr("from"); }
    [[nodiscard]] std::optional<std::string_view> to() const { return attr("to"); }
    [[nodiscard]] std::optional<std::string_view> id() const { return attr("id"); }
    [[nodiscard]] std::optional<std::string_view> type() const { return attr("type"); }

    [[nodiscard]] XMPP_TEST_EXPORT StanzaView child(std::string_view name) const;
    [[nodiscard]] XMPP_TEST_EXPORT StanzaView child(std::string_view name, std::string_view ns) const;
    [[nodiscard]] XMPP_TEST_EXPORT std::string text() const;

    class child_iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = StanzaView;
        using difference_type = std::ptrdiff_t;
        using pointer = const StanzaView *;
        using reference = StanzaView;

        child_iterator() = default;
        child_iterator(xmpp_stanza_t *first, bool end) : current_(first), end_(end) {}

        reference operator*() const { return StanzaView(current_); }
        child_iterator &operator++()
        {
            if (!end_ && current_)
                current_ = xmpp_stanza_get_next(current_);
            return *this;
        }
        child_iterator operator++(int)
        {
            child_iterator tmp = *this;
            ++*this;
            return tmp;
        }
        bool operator==(const child_iterator &other) const
        {
            return end_ == other.end_ && current_ == other.current_;
        }

    private:
        xmpp_stanza_t *current_{nullptr};
        bool end_{true};
    };

    [[nodiscard]] child_iterator children_begin() const
    {
        if (!stanza_)
            return {nullptr, true};
        return {xmpp_stanza_get_children(stanza_), false};
    }
    [[nodiscard]] child_iterator children_end() const { return {nullptr, true}; }

private:
    xmpp_stanza_t *stanza_;
};

inline auto begin(StanzaView view) { return view.children_begin(); }
inline auto end(StanzaView view) { return view.children_end(); }

}  // namespace xmpp