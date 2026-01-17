// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <openssl/sha.h>

#include "color.hh"

namespace weechat {

// XEP-0392: Consistent Color Generation
// Generate a hue angle from a string using SHA-1
static double generate_angle(const std::string& input)
{
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(input.c_str()), input.length(), hash);
    
    // Extract least-significant 16 bits (first two bytes, little-endian)
    uint16_t value = hash[0] | (hash[1] << 8);
    
    // Map to 0-360 degrees
    return (static_cast<double>(value) / 65536.0) * 360.0;
}

// Map hue angle to WeeChat color code (256-color palette)
// This uses a simplified mapping to WeeChat's extended colors (16-255)
std::string angle_to_weechat_color(double angle)
{
    // Map angle to one of WeeChat's 216 colors (6x6x6 RGB cube)
    // These are colors 16-231 in the 256-color palette
    
    // Normalize angle to 0-360
    while (angle < 0) angle += 360.0;
    while (angle >= 360.0) angle -= 360.0;
    
    // Simple hue-to-RGB mapping (saturation=100%, lightness=50%)
    // We'll map to the nearest color in WeeChat's 6-level RGB cube
    double h = angle / 60.0; // 0-6
    double x = 1.0 - std::abs(std::fmod(h, 2.0) - 1.0);
    
    double r, g, b;
    int hi = static_cast<int>(h) % 6;
    
    switch (hi) {
        case 0: r = 1.0; g = x;   b = 0.0; break;  // Red to Yellow
        case 1: r = x;   g = 1.0; b = 0.0; break;  // Yellow to Green
        case 2: r = 0.0; g = 1.0; b = x;   break;  // Green to Cyan
        case 3: r = 0.0; g = x;   b = 1.0; break;  // Cyan to Blue
        case 4: r = x;   g = 0.0; b = 1.0; break;  // Blue to Magenta
        case 5: r = 1.0; g = 0.0; b = x;   break;  // Magenta to Red
        default: r = 0.0; g = 0.0; b = 0.0; break;
    }
    
    // Map to 6-level RGB cube (0-5 for each component)
    int r_idx = static_cast<int>(r * 5.0 + 0.5);
    int g_idx = static_cast<int>(g * 5.0 + 0.5);
    int b_idx = static_cast<int>(b * 5.0 + 0.5);
    
    // WeeChat color index: 16 + 36*r + 6*g + b
    int color = 16 + (36 * r_idx) + (6 * g_idx) + b_idx;
    
    return std::to_string(color);
}

// Main function: generate consistent color for a string (JID or nickname)
std::string consistent_color(const std::string& input)
{
    if (input.empty())
        return "";
    
    // Normalize input to lowercase for consistency (per XEP-0392)
    std::string normalized = input;
    for (char& c : normalized)
        c = std::tolower(static_cast<unsigned char>(c));
    
    double angle = generate_angle(normalized);
    return angle_to_weechat_color(angle);
}

} // namespace weechat
