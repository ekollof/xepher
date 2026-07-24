// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <ranges>

#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "xmpp/node.hh"
#include "account.hh"
#include "channel.hh"

bool weechat::account::omemo_muc_occupant_in_eligible_room(std::string_view bare_jid) const
{
    if (bare_jid.empty())
        return false;

    const std::string normalized = ::jid(nullptr, std::string(bare_jid)).bare;
    if (normalized.empty())
        return false;

    // Room must be non-anonymous *and* have OMEMO actually enabled. Otherwise
    // we would prefetch PEP bundles for every occupant of every public MUC
    // the user is in (noise + "legacy bundle fetch … returned error" spam).
    return std::ranges::any_of(channels, [&](const auto &entry) {
        const auto &[_, ch] = entry;
        return ch.type == channel::chat_type::MUC
            && ch.omemo.enabled
            && ch.muc_supports_omemo()
            && ch.omemo_recipient_jids.contains(normalized);
    });
}