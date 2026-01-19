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
    // Simple Unicode block rendering without image library
    // For now, just return a placeholder - full image decoding would need
    // libpng/libjpeg which aren't dependencies yet
    
    // Unicode block characters for different intensities
    const char* blocks[] = {
        " ",   // 0% (empty)
        "░",   // 25%
        "▒",   // 50%
        "▓",   // 75%
        "█"    // 100% (full)
    };
    
    // For this initial implementation, create a simple colored square
    // In a full implementation, we'd decode the image and downsample
    
    std::string result;
    for (int y = 0; y < target_height; y++) {
        for (int x = 0; x < target_width; x++) {
            // Use hash of image data to deterministically pick a block
            int idx = (image_data.size() > 0) ? 
                      ((image_data[0] + x + y) % 5) : 0;
            result += blocks[idx];
        }
        if (y < target_height - 1)
            result += "\n";
    }
    
    return result;
}

void weechat::avatar::request_metadata(account& acc, const char *jid)
{
    // Subscribe to avatar metadata PEP node
    // This is automatically done via disco caps advertising 
    // "urn:xmpp:avatar:metadata+notify"
    // Nothing needed here - just document that subscription is implicit
}

void weechat::avatar::request_data(account& acc, const char *jid, 
                                    const std::string& hash)
{
    // Generate unique IQ ID
    std::unique_ptr<char> id(xmpp_uuid_gen(acc.context));
    
    xmpp_stanza_t *iq = weechat::xep0084::request_avatar_data(acc.context, jid, id.get());
    
    acc.connection.send(iq);
    xmpp_stanza_release(iq);
    xmpp_free(acc.context, id.release());
}
