// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "iq_pubsub_feed.hh"

namespace xmpp {

bool is_skipped_non_atom_feed_item_id(const std::string_view item_id)
{
    return item_id.starts_with("urn:xmpp:avatar:")
        || item_id.starts_with("urn:xmpp:omemo:")
        || item_id.starts_with("urn:xmpp:bookmarks:");
}

bool is_microblog_comments_node(const std::string_view node)
{
    return node.starts_with(k_microblog_comments_prefix);
}

}  // namespace xmpp