// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "test_export.hh"
#include "node.hh"
#include "xmpp/stanza_view.hh"

namespace weechat {
class account;
}

struct t_gui_buffer;

namespace xmpp {

inline constexpr std::string_view k_bob_ns = "urn:xmpp:bob";
inline constexpr std::size_t k_bob_max_payload_bytes = 8192;
inline constexpr std::size_t k_bob_inline_payload_bytes = 1024;
inline constexpr std::string_view k_xhtml_im_ns = "http://jabber.org/protocol/xhtml-im";
inline constexpr std::string_view k_xhtml_ns = "http://www.w3.org/1999/xhtml";

struct BobImageRef {
    std::string cid;
    std::string mime;
    std::string inline_b64;
};

struct BobHostedPayload {
    std::string mime;
    std::vector<std::uint8_t> data;
};

[[nodiscard]] XMPP_TEST_EXPORT bool is_bob_cid_url(std::string_view url);
[[nodiscard]] XMPP_TEST_EXPORT std::optional<std::string>
bob_cid_from_url(std::string_view url);

// Inline <data xmlns='urn:xmpp:bob'/> plus XHTML-IM <img src='cid:…@bob.xmpp.org'/>.
[[nodiscard]] XMPP_TEST_EXPORT std::vector<BobImageRef>
collect_bob_image_refs(StanzaView msg);

[[nodiscard]] XMPP_TEST_EXPORT bool message_has_xhtml_bob_images(StanzaView msg);

[[nodiscard]] XMPP_TEST_EXPORT std::vector<std::uint8_t>
bob_decode_base64(std::string_view encoded);

[[nodiscard]] XMPP_TEST_EXPORT std::string
bob_encode_base64(std::span<const std::uint8_t> data);

[[nodiscard]] XMPP_TEST_EXPORT std::string
bob_make_cid(std::span<const std::uint8_t> data);

[[nodiscard]] XMPP_TEST_EXPORT bool
bob_payload_size_ok(std::size_t nbytes);

[[nodiscard]] XMPP_TEST_EXPORT std::optional<std::string>
bob_cache_lookup(weechat::account &acct, std::string_view cid);

[[nodiscard]] XMPP_TEST_EXPORT std::optional<std::string>
bob_cache_store_bytes(weechat::account &acct,
                      std::string_view cid,
                      std::string_view mime,
                      std::span<const std::uint8_t> data);

void bob_host_store(weechat::account &acct,
                    std::string_view cid,
                    std::string_view mime,
                    std::span<const std::uint8_t> data);

[[nodiscard]] XMPP_TEST_EXPORT std::optional<BobHostedPayload>
bob_host_lookup(weechat::account &acct, std::string_view cid);

[[nodiscard]] XMPP_TEST_EXPORT stanza::message
build_bob_image_message(std::string_view to,
                        std::string_view msg_type,
                        std::string_view msg_id,
                        std::string_view cid,
                        std::string_view mime,
                        std::span<const std::uint8_t> data,
                        std::string_view alt);

[[nodiscard]] XMPP_TEST_EXPORT bool is_bob_iq_get(StanzaView iq);

[[nodiscard]] XMPP_TEST_EXPORT std::optional<stanza::iq>
handle_bob_iq_get(StanzaView request,
                  std::string_view local_jid,
                  const BobHostedPayload *hosted);

void bob_start_fetch(weechat::account &acct,
                     std::string_view to_jid,
                     std::string_view cid,
                     std::string_view mime,
                     struct t_gui_buffer *buffer,
                     std::string_view channel_jid,
                     std::string_view stable_id,
                     bool mam_replay);

void bob_complete_fetch_iq(weechat::account &acct,
                           std::string_view iq_id,
                           std::string_view mime,
                           std::span<const std::uint8_t> data);

}  // namespace xmpp