// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "test_export.hh"
#include "xmpp/stanza_view.hh"

namespace xmpp {

inline constexpr std::string_view k_fallback_ns = "urn:xmpp:fallback:0";
inline constexpr std::string_view k_reactions_ns = "urn:xmpp:reactions:0";
inline constexpr std::string_view k_retract_ns = "urn:xmpp:message-retract:1";
inline constexpr std::string_view k_reply_ns = "urn:xmpp:reply:0";
inline constexpr std::string_view k_fasten_ns = "urn:xmpp:fasten:0";

enum class FallbackBodyDisposition {
    Unchanged,
    Cleared,
    Trimmed,
};

struct FallbackBodyResult {
    FallbackBodyDisposition disposition = FallbackBodyDisposition::Unchanged;
    std::string trimmed;
};

[[nodiscard]] XMPP_TEST_EXPORT bool stanza_has_fallback(StanzaView msg);

// XEP-0428 / XEP-0461: strip embedded fallback quote ranges from body text.
[[nodiscard]] XMPP_TEST_EXPORT FallbackBodyResult apply_fallback_body_trim(
    StanzaView msg, std::string_view body_text, bool has_message_correction);

}  // namespace xmpp