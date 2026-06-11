// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "message_aesgcm.hh"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <openssl/evp.h>
#include <ranges>
#include <span>
#include <string>
#include <vector>

#include <fmt/core.h>

#include "iq_upload.hh"

namespace xmpp {

namespace {

inline constexpr std::string_view k_aesgcm_scheme = "aesgcm://";
inline constexpr std::size_t k_aesgcm_iv_hex_len = 24;
inline constexpr std::size_t k_aesgcm_key_hex_len = 64;
inline constexpr std::size_t k_aesgcm_fragment_hex_len =
    k_aesgcm_iv_hex_len + k_aesgcm_key_hex_len;

[[nodiscard]] bool is_hex_digit(const char c)
{
    return (c >= '0' && c <= '9')
        || (c >= 'a' && c <= 'f')
        || (c >= 'A' && c <= 'F');
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>> hex_decode(std::string_view hex)
{
    if (hex.empty() || hex.size() % 2 != 0)
        return std::nullopt;

    std::vector<std::uint8_t> out(hex.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i)
    {
        const auto [ptr, ec] = std::from_chars(
            hex.data() + static_cast<std::ptrdiff_t>(i * 2),
            hex.data() + static_cast<std::ptrdiff_t>(i * 2 + 2),
            out[i],
            16);
        if (ec != std::errc{} || ptr != hex.data() + static_cast<std::ptrdiff_t>(i * 2 + 2))
            return std::nullopt;
    }
    return out;
}

[[nodiscard]] std::string base64_encode_bytes(std::span<const std::uint8_t> data)
{
    if (data.empty())
        return {};

    const int encoded_size =
        4 * static_cast<int>((data.size() + 2) / 3) + 4;
    std::string encoded(static_cast<std::size_t>(encoded_size), '\0');
    const int written = EVP_EncodeBlock(
        reinterpret_cast<unsigned char *>(encoded.data()),
        data.data(),
        static_cast<int>(data.size()));
    if (written <= 0)
        return {};
    encoded.resize(static_cast<std::size_t>(written));
    return encoded;
}

[[nodiscard]] bool is_valid_aesgcm_thumbnail_line(std::string_view line)
{
    return line.starts_with("data:image/jpeg,")
        || line.starts_with("data:image/jpep,");  // spec typo; accept for interoperability
}

[[nodiscard]] std::string filename_from_https_url(std::string_view url)
{
    const auto scheme = url.find("://");
    if (scheme == std::string_view::npos)
        return "esfs_file";

    auto path_start = url.find('/', scheme + 3);
    if (path_start == std::string_view::npos)
        return "esfs_file";

    std::string_view path = url.substr(path_start + 1);
    const auto slash = path.find_last_of('/');
    std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);

    const auto query = name.find('?');
    if (query != std::string_view::npos)
        name = name.substr(0, query);

    if (name.empty())
        return "esfs_file";
    return std::string(name);
}

[[nodiscard]] std::optional<std::string_view> aesgcm_first_line(std::string_view body)
{
    if (body.empty())
        return std::nullopt;

    std::string_view first_line = body;
    const auto newline = body.find('\n');
    if (newline != std::string_view::npos)
    {
        first_line = body.substr(0, newline);
        std::string_view rest = body.substr(newline + 1);
        if (rest.starts_with('\r'))
            rest.remove_prefix(1);

        if (!rest.empty())
        {
            if (!is_valid_aesgcm_thumbnail_line(rest))
                return std::nullopt;
            if (rest.find('\n') != std::string_view::npos)
                return std::nullopt;
        }
    }

    if (!first_line.empty() && first_line.back() == '\r')
        first_line.remove_suffix(1);

    if (!first_line.starts_with(k_aesgcm_scheme))
        return std::nullopt;

    return first_line;
}

}  // namespace

std::optional<EncryptedMediaShare> parse_aesgcm_body_share(const std::string_view body)
{
    const auto first_line = aesgcm_first_line(body);
    if (!first_line)
        return std::nullopt;

    const auto hash = first_line->find('#');
    if (hash == std::string_view::npos || hash + 1 >= first_line->size())
        return std::nullopt;

    const std::string_view fragment = first_line->substr(hash + 1);
    if (fragment.size() != k_aesgcm_fragment_hex_len
        || !std::ranges::all_of(fragment, is_hex_digit))
        return std::nullopt;

    const std::string_view host_path =
        first_line->substr(k_aesgcm_scheme.size(), hash - k_aesgcm_scheme.size());
    if (host_path.empty())
        return std::nullopt;

    const auto iv_bytes = hex_decode(fragment.substr(0, k_aesgcm_iv_hex_len));
    const auto key_bytes = hex_decode(fragment.substr(k_aesgcm_iv_hex_len, k_aesgcm_key_hex_len));
    if (!iv_bytes || !key_bytes || iv_bytes->size() != 12 || key_bytes->size() != 32)
        return std::nullopt;

    const std::string iv_b64 = base64_encode_bytes(*iv_bytes);
    const std::string key_b64 = base64_encode_bytes(*key_bytes);
    if (iv_b64.empty() || key_b64.empty())
        return std::nullopt;

    EncryptedMediaShare share;
    share.ciphertext_url = fmt::format("https://{}", host_path);
    share.iv_b64 = iv_b64;
    share.key_b64 = key_b64;
    share.meta.name = filename_from_https_url(share.ciphertext_url);
    share.meta.mime = content_type_from_upload_filename(share.meta.name);
    return share;
}

}  // namespace xmpp