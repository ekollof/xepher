// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "test_export.hh"
#include "xmpp/atom.hh"

namespace weechat {
class account;
class channel;
}  // namespace weechat

namespace xmpp {

[[nodiscard]] XMPP_TEST_EXPORT std::string feed_alias_prefix(int item_alias);

// Human-readable node label (no leading '='); e.g. "Phoronix", "blog", "comments".
[[nodiscard]] XMPP_TEST_EXPORT std::string feed_node_display_label(
    std::string_view service_jid,
    std::string_view node);

// Parent feed label for comments buffers (no leading '=').
[[nodiscard]] XMPP_TEST_EXPORT std::string feed_parent_display_label(
    std::string_view parent_service,
    std::string_view parent_node);

// Buffer short_name for a feed_key ("service/node"), including leading '='.
[[nodiscard]] XMPP_TEST_EXPORT std::string feed_buffer_short_name(std::string_view feed_key);

// Comments buffer short_name from a resolved parent label and optional item alias.
[[nodiscard]] XMPP_TEST_EXPORT std::string feed_comments_buffer_short_name(
    std::string_view parent_label,
    int item_alias);

[[nodiscard]] XMPP_TEST_EXPORT std::string feed_item_xmpp_link(
    std::string_view feed_service,
    std::string_view node,
    std::string_view item_id);

[[nodiscard]] XMPP_TEST_EXPORT std::string feed_reply_label(
    std::string_view reply_to,
    const std::function<int(std::string_view)> &alias_lookup);

// Render one Atom entry into a FEED channel buffer (XEP-0277 / XEP-0472).
void render_atom_entry_to_feed(
    weechat::channel &feed_ch,
    weechat::account &account,
    std::string_view feed_key,
    std::string_view feed_service,
    std::string_view node,
    std::string_view item_id,
    int item_alias,
    const atom_entry &ae);

}  // namespace xmpp