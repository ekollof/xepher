// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "test_export.hh"
#include "xmpp/stanza_view.hh"

namespace xmpp {

// XEP-0449: <sticker xmlns='urn:xmpp:stickers:0'/> present.
[[nodiscard]] XMPP_TEST_EXPORT bool stanza_has_sticker(StanzaView msg);

struct CustomEmojiPreview {
    std::string url;
    std::string mime;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string name;
};

// XEP-0514: resolve <markup><span><emoji/></span></markup> hashes against
// sibling SFS/SIMS media elements.
[[nodiscard]] XMPP_TEST_EXPORT std::vector<CustomEmojiPreview>
collect_custom_emoji_previews(StanzaView msg);

// Hash keys (algo:value_b64) referenced by XEP-0514 emoji markup — used to
// suppress generic file-share suffixes when icat displays the images inline.
[[nodiscard]] XMPP_TEST_EXPORT std::unordered_set<std::string>
collect_emoji_markup_hash_keys(StanzaView msg);

[[nodiscard]] XMPP_TEST_EXPORT std::string file_hash_key(std::string_view algo,
                                                         std::string_view value_b64);

}  // namespace xmpp