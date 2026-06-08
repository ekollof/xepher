// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "iq_upload.hh"

#include <algorithm>
#include <cctype>
#include <ranges>
#include <fmt/core.h>

#include "iq_error.hh"

namespace xmpp {

namespace {

[[nodiscard]] bool header_name_iequals(std::string_view a, std::string_view b)
{
    return a.size() == b.size()
        && std::ranges::equal(a, b, [](unsigned char x, unsigned char y) {
            return std::tolower(x) == std::tolower(y);
        });
}

}  // namespace

bool is_allowed_http_upload_put_header(const std::string_view name)
{
    static constexpr std::string_view k_allowed[] = {
        "authorization", "cookie", "expires",
    };
    return std::ranges::any_of(k_allowed, [&](const std::string_view allowed) {
        return header_name_iequals(name, allowed);
    });
}

std::string sanitize_http_header_value(const std::string_view value)
{
    std::string safe(value);
    std::erase_if(safe, [](const char c) { return c == '\r' || c == '\n'; });
    return safe;
}

std::string content_type_from_upload_filename(const std::string_view filename)
{
    const auto dot = filename.find_last_of('.');
    if (dot == std::string_view::npos || dot + 1 >= filename.size())
        return "application/octet-stream";

    std::string ext(filename.substr(dot + 1));
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "png") return "image/png";
    if (ext == "gif") return "image/gif";
    if (ext == "webp") return "image/webp";
    if (ext == "mp4") return "video/mp4";
    if (ext == "webm") return "video/webm";
    if (ext == "pdf") return "application/pdf";
    if (ext == "txt") return "text/plain";
    return "application/octet-stream";
}

std::string format_upload_slot_error_message(const StanzaView error_elem)
{
    if (!error_elem.valid())
        return "Upload slot request failed";

    const std::string detail = iq_error_text(error_elem);
    if (detail.empty() || detail == "unknown error")
        return "Upload slot request failed";
    return fmt::format("Upload slot request failed: {}", detail);
}

}  // namespace xmpp