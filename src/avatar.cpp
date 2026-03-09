// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <sys/stat.h>
#include <sys/types.h>
#include <fstream>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <weechat/weechat-plugin.h>

#include "avatar.hh"
#include "account.hh"
#include "plugin.hh"
#include "xmpp/stanza.hh"
#include "xmpp/xep-0084.inl"

std::string weechat::avatar::calculate_hash(const std::vector<uint8_t>& data)
{
    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(data.data(), data.size(), hash);
    
    char hex[SHA_DIGEST_LENGTH * 2 + 1];
    for (int i = 0; i < SHA_DIGEST_LENGTH; i++)
        sprintf(hex + (i * 2), "%02x", hash[i]);
    hex[SHA_DIGEST_LENGTH * 2] = '\0';
    
    return std::string(hex);
}

std::string weechat::avatar::get_cache_dir(const account& acc)
{
    const char *weechat_dir = weechat_info_get("weechat_data_dir", nullptr);
    if (!weechat_dir)
        return "";
    
    std::string cache_dir = std::string(weechat_dir) + "/xmpp_avatars/" + acc.name.data();
    
    // Create directory if it doesn't exist
    mkdir((std::string(weechat_dir) + "/xmpp_avatars").c_str(), 0755);
    mkdir(cache_dir.c_str(), 0755);
    
    return cache_dir;
}

std::optional<weechat::avatar::data> weechat::avatar::load_from_cache(
    const account& acc, const std::string& hash)
{
    std::string cache_dir = get_cache_dir(acc);
    if (cache_dir.empty())
        return std::nullopt;
    
    std::string filepath = cache_dir + "/" + hash + ".dat";
    std::ifstream file(filepath, std::ios::binary);
    if (!file)
        return std::nullopt;
    
    data avatar_data;
    
    // Read metadata
    file.read(reinterpret_cast<char*>(&avatar_data.meta.bytes), sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(&avatar_data.meta.width), sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(&avatar_data.meta.height), sizeof(uint32_t));
    
    uint32_t type_len;
    file.read(reinterpret_cast<char*>(&type_len), sizeof(uint32_t));
    avatar_data.meta.type.resize(type_len);
    file.read(&avatar_data.meta.type[0], type_len);
    
    avatar_data.meta.id = hash;
    
    // Read image data
    avatar_data.image_data.resize(avatar_data.meta.bytes);
    file.read(reinterpret_cast<char*>(avatar_data.image_data.data()), avatar_data.meta.bytes);
    
    return avatar_data;
}

bool weechat::avatar::save_to_cache(const account& acc,
                                     const std::string& hash,
                                     const data& avatar_data)
{
    std::string cache_dir = get_cache_dir(acc);
    if (cache_dir.empty())
        return false;
    
    std::string filepath = cache_dir + "/" + hash + ".dat";
    std::ofstream file(filepath, std::ios::binary);
    if (!file)
        return false;
    
    // Write metadata
    file.write(reinterpret_cast<const char*>(&avatar_data.meta.bytes), sizeof(uint32_t));
    file.write(reinterpret_cast<const char*>(&avatar_data.meta.width), sizeof(uint32_t));
    file.write(reinterpret_cast<const char*>(&avatar_data.meta.height), sizeof(uint32_t));
    
    uint32_t type_len = avatar_data.meta.type.size();
    file.write(reinterpret_cast<const char*>(&type_len), sizeof(uint32_t));
    file.write(avatar_data.meta.type.data(), type_len);
    
    // Write image data
    file.write(reinterpret_cast<const char*>(avatar_data.image_data.data()), 
               avatar_data.meta.bytes);
    
    return file.good();
}

std::string weechat::avatar::render_unicode_blocks(
    const std::vector<uint8_t>& image_data,
    const std::string& mime_type,
    int target_width,
    int target_height)
{
    (void)mime_type;       // Would need image decoder library for full rendering
    (void)target_width;    // Currently rendering single symbol
    (void)target_height;   // Currently rendering single symbol
    
    // WeeChat plugins can only output text - we generate a colored Unicode symbol
    // that's deterministic based on the avatar's image data hash.
    // This works in all terminals and provides visual distinction between users.
    
    if (image_data.empty())
        return "";
    
    // Calculate a hash value from the image data
    uint32_t hash = 0;
    for (size_t i = 0; i < std::min(image_data.size(), size_t(64)); i++)
    {
        hash = ((hash << 5) + hash) + image_data[i];
    }
    
    // Unicode geometric shapes and symbols for avatars
    const char* avatar_chars[] = {
        "●", // Circle
        "■", // Square
        "▲", // Triangle
        "◆", // Diamond
        "★", // Star
        "♦", // Diamond (card)
        "◉", // Large circle
        "◈", // Pattern
        "⬢", // Hexagon
        "⬟", // Pentagon
    };
    
    // WeeChat color codes (16-255 for extended colors)
    // Use bright, distinguishable colors
    const int colors[] = {
        196, // Red
        202, // Orange  
        226, // Yellow
        046, // Green
        051, // Cyan
        021, // Blue
        93,  // Purple
        201, // Magenta
        208, // Orange-red
        118, // Light green
    };
    
    // Pick character and color based on hash
    int char_idx = hash % (sizeof(avatar_chars) / sizeof(avatar_chars[0]));
    int color_idx = (hash >> 8) % (sizeof(colors) / sizeof(colors[0]));
    
    // Format: <color><char><reset>
    char result[128];
    snprintf(result, sizeof(result), "\x19%03d%s", 
             colors[color_idx], avatar_chars[char_idx]);
    
    return std::string(result);
}

void weechat::avatar::request_metadata(account& /*acc*/, const char * /*jid*/)
{
    // Subscribe to avatar metadata PEP node
    // This is automatically done via disco caps advertising
    // "urn:xmpp:avatar:metadata+notify"
    // Nothing needed here - just document that subscription is implicit
}

void weechat::avatar::request_data(account& acc, const char *jid,
                                    const std::string& /*hash*/)
{
    // Generate unique IQ ID
    std::unique_ptr<char> id(xmpp_uuid_gen(acc.context));
    
    xmpp_stanza_t *iq = weechat::xep0084::request_avatar_data(acc.context, jid, id.get());
    
    acc.connection.send(iq);
    xmpp_stanza_release(iq);
    xmpp_free(acc.context, id.release());
}

void weechat::avatar::load_for_user(account& acc, user& user)
{
    // If user has an avatar hash, try to load it from cache
    if (user.profile.avatar_hash.empty())
        return;
    
    const std::string& hash = user.profile.avatar_hash;
    auto cached = load_from_cache(acc, hash);
    
    if (cached)
    {
        // Load successful - update user profile
        user.profile.avatar_data = cached->image_data;
        user.profile.avatar_rendered = render_unicode_blocks(
            cached->image_data, 
            cached->meta.type
        );
        
        weechat_printf_date_tags(acc.buffer, 0, "xmpp_avatar",
                                "%sLoaded cached avatar for %s (hash: %.8s...)",
                                weechat_prefix("network"),
                                user.id.c_str(),
                                hash.c_str());
    }
}
