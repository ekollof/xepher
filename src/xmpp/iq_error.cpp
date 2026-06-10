// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "iq_error.hh"

#include <string_view>

namespace xmpp {

std::string iq_error_text(StanzaView error_elem)
{
    if (!error_elem.valid())
        return "unknown error";

    const StanzaView text_el = error_elem.child("text");
    if (text_el.valid())
    {
        const std::string text = text_el.text();
        if (!text.empty())
            return text;
    }

    for (const StanzaView child : error_elem)
    {
        const std::string_view name = child.name();
        if (!name.empty() && name != "text")
            return std::string(name);
    }

    return "unknown error";
}

std::string iq_error_condition(StanzaView iq, std::span<const std::string_view> candidates)
{
    const StanzaView error = iq.child("error");
    if (!error.valid())
        return "unknown";

    for (const std::string_view name : candidates)
    {
        if (const StanzaView cond = error.child(name); cond.valid())
            return std::string(cond.name());
    }

    return "unknown";
}

}  // namespace xmpp