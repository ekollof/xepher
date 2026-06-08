// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "message_body.hh"

#include "util.hh"

namespace xmpp {

bool stanza_has_unstyled_hint(StanzaView msg)
{
    return msg.child("unstyled", k_styling_ns).valid();
}

bool stanza_has_markup(StanzaView msg)
{
    return msg.child("markup", k_markup_ns).valid();
}

std::string format_inbound_message_body(xmpp_stanza_t *stanza, std::string_view text)
{
    if (text.empty() || !stanza)
        return std::string(text);

    if (stanza_has_unstyled_hint(StanzaView(stanza)))
        return std::string(text);

    std::string styled = apply_xep394_markup(stanza, text);
    if (styled.empty())
        styled = apply_xep393_styling(text);
    return styled;
}

}  // namespace xmpp