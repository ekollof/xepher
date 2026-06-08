// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "iq_mam.hh"

#include <algorithm>
#include <cctype>
#include <ranges>

namespace xmpp {

namespace {

[[nodiscard]] bool attr_iequals(std::string_view a, std::string_view b)
{
    return a.size() == b.size()
        && std::ranges::equal(a, b, [](unsigned char x, unsigned char y) {
            return std::tolower(x) == std::tolower(y);
        });
}

}  // namespace

bool is_mam_fin_bool_attr_true(const std::string_view value)
{
    return attr_iequals(value, "true") || attr_iequals(value, "1");
}

std::string mam_fin_rsm_last(const StanzaView fin)
{
    if (!fin.valid())
        return {};

    const StanzaView rsm = fin.child("set", "http://jabber.org/protocol/rsm");
    if (!rsm.valid())
        return {};

    const StanzaView last = rsm.child("last");
    if (!last.valid())
        return {};

    return last.text();
}

bool iq_has_item_not_found_error(const StanzaView iq)
{
    if (!iq.valid())
        return false;

    const StanzaView err = iq.child("error");
    if (!err.valid())
        return false;

    return err.child("item-not-found", "urn:ietf:params:xml:ns:xmpp-stanzas").valid();
}

}  // namespace xmpp