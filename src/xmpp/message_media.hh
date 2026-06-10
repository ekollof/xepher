// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "test_export.hh"
#include "xmpp/stanza_view.hh"

namespace xmpp {

inline constexpr std::string_view k_reference_ns = "urn:xmpp:reference:0";
inline constexpr std::string_view k_sims_ns = "urn:xmpp:sims:1";
inline constexpr std::string_view k_sfs_ns = "urn:xmpp:sfs:0";
inline constexpr std::string_view k_jingle_file_ns = "urn:xmpp:jingle:apps:file-transfer:5";
inline constexpr std::string_view k_file_metadata_ns = "urn:xmpp:file:metadata:0";
inline constexpr std::string_view k_esfs_ns = "urn:xmpp:esfs:0";
inline constexpr std::string_view k_hashes_ns = "urn:xmpp:hashes:2";
inline constexpr std::string_view k_stickers_ns = "urn:xmpp:stickers:0";
inline constexpr std::string_view k_emoji_markup_ns = "urn:xmpp:markup:emoji:0";
inline constexpr std::string_view k_aes_gcm_cipher =
    "urn:xmpp:ciphers:aes-256-gcm-nopadding:0";

struct FileHash {
    std::string algo;
    std::string value_b64;
};

struct FileMetadata {
    std::string name;
    std::string mime;
    std::string size_raw;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<FileHash> hashes;
};

struct PlainMediaShare {
    FileMetadata meta;
    std::string url;
};

struct EncryptedMediaShare {
    FileMetadata meta;
    std::string ciphertext_url;
    std::string key_b64;
    std::string iv_b64;
};

struct SfsShare {
    FileMetadata meta;
    std::optional<std::string> plain_url;
    std::optional<EncryptedMediaShare> encrypted;
};

[[nodiscard]] XMPP_TEST_EXPORT FileMetadata parse_file_metadata(StanzaView file_elem);
[[nodiscard]] XMPP_TEST_EXPORT std::vector<PlainMediaShare> collect_sims_shares(StanzaView msg);
[[nodiscard]] XMPP_TEST_EXPORT std::vector<SfsShare> collect_sfs_shares(StanzaView msg);

[[nodiscard]] XMPP_TEST_EXPORT bool is_supported_esfs_cipher(std::string_view cipher);
[[nodiscard]] XMPP_TEST_EXPORT std::string format_byte_size(std::string_view size_str);
[[nodiscard]] XMPP_TEST_EXPORT std::string format_file_share_suffix(std::string_view name,
                                                                    std::string_view mime,
                                                                    std::string_view size_raw,
                                                                    std::string_view url);
[[nodiscard]] XMPP_TEST_EXPORT std::string format_encrypted_file_suffix(std::string_view name,
                                                                         std::string_view size_raw);
[[nodiscard]] XMPP_TEST_EXPORT std::string format_encrypted_file_saved_suffix(
    std::string_view name, std::string_view saved_path);

}  // namespace xmpp