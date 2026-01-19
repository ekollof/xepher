// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <optional>

namespace weechat
{
    class account;
    
    class avatar
    {
    public:
        struct metadata
        {
            std::string id;           // SHA-1 hash of image data
            std::string type;         // MIME type (image/png, image/jpeg)
            uint32_t bytes = 0;       // Image size in bytes
            uint32_t width = 0;       // Image width
            uint32_t height = 0;      // Image height
        };
        
        struct data
        {
            std::vector<uint8_t> image_data;  // Raw image bytes
            metadata meta;
            std::string rendered;              // Cached Unicode block rendering
        };
        
        // Render avatar to colored Unicode symbol (deterministic based on image hash)
        // Returns a single colored geometric shape character (●, ★, ◆, etc.)
        static std::string render_unicode_blocks(const std::vector<uint8_t>& image_data,
                                                  const std::string& mime_type,
                                                  int target_width = 2,
                                                  int target_height = 2);
        
        // Calculate SHA-1 hash of image data (for verification)
        static std::string calculate_hash(const std::vector<uint8_t>& data);
        
        // Get avatar cache directory for account
        static std::string get_cache_dir(const account& acc);
        
        // Load avatar from cache
        static std::optional<data> load_from_cache(const account& acc, 
                                                    const std::string& hash);
        
        // Save avatar to cache
        static bool save_to_cache(const account& acc,
                                  const std::string& hash,
                                  const data& avatar_data);
        
        // Request avatar from remote JID
        static void request_metadata(account& acc, const char *jid);
        static void request_data(account& acc, const char *jid, const std::string& hash);
        
        // Load avatar for user from cache (called on user creation/presence)
        static void load_for_user(account& acc, class user& user);
    };
}
