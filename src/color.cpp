// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <ranges>
#include <algorithm>
#include <array>
#include <map>
#include <numbers>
#include <openssl/evp.h>
#include <weechat/weechat-plugin.h>

#include "color.hh"
#include "plugin.hh"

namespace weechat {

// XEP-0392: Consistent Color Generation
// Generate a hue angle from a string using SHA-1
static double generate_angle(std::string_view input)
{
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    EVP_Digest(input.data(), input.size(), hash, &hash_len, EVP_sha1(), nullptr);
    
    // Extract least-significant 16 bits (first two bytes, little-endian)
    uint16_t value = hash[0] | (hash[1] << 8);
    
    // Map to 0-360 degrees
    return (static_cast<double>(value) / 65536.0) * 360.0;
}

// HSLuv preserves the CIELUV hue and lightness. Saturation is not needed
// for XEP-0392's palette algorithm, so RGB -> XYZ -> Luv is sufficient.
namespace {
struct palette_color {
    double lightness;
    int index;
};

auto hue_palette() -> std::map<int, palette_color>
{
    std::map<int, palette_color> palette;
    std::ranges::for_each(std::views::iota(0, 216), [&](int index) {
        const int r = index / 36;
        const int g = index / 6 % 6;
        const int b = index % 6;
        if (r == g && g == b)
            return;
        const auto linear = [](int component) {
            const double value = component / 5.0;
            return value > 0.04045 ? std::pow((value + 0.055) / 1.055, 2.4)
                                   : value / 12.92;
        };
        const std::array rgb{linear(r), linear(g), linear(b)};
        const double x = 0.41239079926595948 * rgb[0]
                       + 0.35758433938387796 * rgb[1]
                       + 0.18048078840183429 * rgb[2];
        const double y = 0.21263900587151036 * rgb[0]
                       + 0.71516867876775593 * rgb[1]
                       + 0.072192315360733715 * rgb[2];
        const double z = 0.01933081871559185 * rgb[0]
                       + 0.11919477979462599 * rgb[1]
                       + 0.95053215224966058 * rgb[2];
        const double denominator = x + 15.0 * y + 3.0 * z;
        const double lightness = y <= 0.00885645167903563
            ? y * 903.2962962962963 : 116.0 * std::cbrt(y) - 16.0;
        const double u = 4.0 * x / denominator - 0.19783000664283681;
        const double v = 9.0 * y / denominator - 0.46831999493879100;
        const double hue = std::atan2(v, u) * 180.0 / std::numbers::pi;
        const int angle = static_cast<int>(std::round(hue < 0 ? hue + 360.0 : hue));
        const auto existing = palette.find(angle);
        if (existing == palette.end()
            || std::abs(lightness - 73.2) < std::abs(existing->second.lightness - 73.2))
            palette.insert_or_assign(angle, palette_color{lightness, index + 16});
    });
    return palette;
}
}

// XEP-0392 sections 5.3 and 5.4: web-safe RGB cube, rounded HSLuv
// hues, preferred lightness 73.2, then nearest circular hue distance.
XMPP_TEST_EXPORT std::string angle_to_weechat_color(double angle)
{
    static const auto palette = hue_palette();
    if (!std::isfinite(angle))
        return {};
    angle = std::fmod(std::fmod(angle, 360.0) + 360.0, 360.0);
    if (const auto exact = palette.find(static_cast<int>(std::round(angle)));
        exact != palette.end())
        return std::to_string(exact->second.index);
    const auto distance = [angle](const auto &entry) {
        const auto &[hue, color] = entry;
        const double delta = std::abs(angle - hue);
        return std::ranges::min(std::array{delta, 360.0 - delta});
    };
    return std::to_string(std::ranges::min_element(palette, {}, distance)->second.index);
}

// Main function: generate consistent color for a string (JID or nickname)
XMPP_TEST_EXPORT std::string consistent_color(std::string_view input)
{
    if (input.empty())
        return "";

    // Nicknames are case-sensitive. JID callers must supply a prepared JID.
    return angle_to_weechat_color(generate_angle(input));
}

XMPP_TEST_EXPORT std::string xmpp_color(std::string_view name)
{
    if (name.empty())
        return "";
    if (auto color_ptr = weechat_color(std::string(name).c_str()))
        return std::string(color_ptr);
    return "";
}

} // namespace weechat
