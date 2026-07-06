// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <sys/stat.h>
#include <sys/types.h>
#include <fstream>
#include <algorithm>
#include <span>
#include <ranges>
#include <expected>
#include <string>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <weechat/weechat-plugin.h>
#include <fmt/format.h>

#include "avatar.hh"
#include "account.hh"
#include "user.hh"
#include "plugin.hh"
#include "weechat/runtime_port.hh"
#include "xmpp/stanza.hh"
#include "xmpp/node.hh"
#include "xmpp/xep-0084.inl"
#include "weechat/ui_port.hh"

std::string weechat::avatar::calculate_hash(std::span<const uint8_t> data)
{
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    EVP_Digest(data.data(), data.size(), hash, &hash_len, EVP_sha1(), nullptr);

    std::string hex;
    hex.reserve(hash_len * 2);
    std::ranges::for_each(std::span<const unsigned char>(hash, hash_len), [&](unsigned char b) {
        hex += fmt::format("{:02x}", b);
    });
    
    return hex;
}

std::string weechat::avatar::get_cache_dir(const account& acc)
{
    const char *weechat_dir = weechat_info_get("weechat_data_dir", nullptr);
    if (!weechat_dir)
        return "";
    
    std::string cache_dir = fmt::format("{}/xmpp_avatars/{}", weechat_dir, acc.name.data());
    
    // Create directory if it doesn't exist
    mkdir(fmt::format("{}/xmpp_avatars", weechat_dir).c_str(), 0755);
    mkdir(cache_dir.c_str(), 0755);
    
    return cache_dir;
}

std::expected<weechat::avatar::data, std::string> weechat::avatar::load_from_cache(
    const account& acc, std::string_view hash)
{
    std::string cache_dir = get_cache_dir(acc);
    if (cache_dir.empty())
        return std::unexpected("no cache dir");
    
    std::string filepath = fmt::format("{}/{}.dat", cache_dir, hash);
    std::ifstream file(filepath, std::ios::binary);
    if (!file)
        return std::unexpected("open failed");
    
    data avatar_data;
    
    // Read metadata
    file.read(reinterpret_cast<char*>(&avatar_data.meta.bytes), sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(&avatar_data.meta.width), sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(&avatar_data.meta.height), sizeof(uint32_t));
    
    uint32_t type_len;
    file.read(reinterpret_cast<char*>(&type_len), sizeof(uint32_t));
    avatar_data.meta.type.resize(type_len);
    file.read(&avatar_data.meta.type[0], type_len);
    
    avatar_data.meta.id = std::string(hash);
    
    // Read image data
    avatar_data.image_data.resize(avatar_data.meta.bytes);
    file.read(reinterpret_cast<char*>(avatar_data.image_data.data()), avatar_data.meta.bytes);
    
    return avatar_data;
}

bool weechat::avatar::save_to_cache(const account& acc,
                                     std::string_view hash,
                                     const data& avatar_data)
{
    std::string cache_dir = get_cache_dir(acc);
    if (cache_dir.empty())
        return false;
    
    std::string filepath = fmt::format("{}/{}.dat", cache_dir, hash);
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
    std::span<const uint8_t> image_data,
    std::string_view mime_type,
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
    auto view = image_data | std::views::take(std::min(image_data.size(), size_t(64)));
    std::ranges::for_each(view, [&](auto b) {
        hash = ((hash << 5) + hash) + b;
    });
    
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
    return fmt::format("\x19{:03d}{}", colors[color_idx], avatar_chars[char_idx]);
}

void weechat::avatar::request_metadata(account& /*acc*/, const char * /*jid*/)
{
    // Subscribe to avatar metadata PEP node
    // This is automatically done via disco caps advertising
    // "urn:xmpp:avatar:metadata+notify"
    // Nothing needed here - just document that subscription is implicit
}

void weechat::avatar::request_data(account& acc, const char *jid,
                                    std::string_view hash)
{
    // If we already have this hash cached, load it directly — no network needed.
    if (!hash.empty())
    {
        auto cached = load_from_cache(acc, hash);
        if (cached)
        {
            weechat::user *user = weechat::user::search(&acc, jid);
            if (user)
            {
                user->profile.avatar_data     = cached->image_data;
                user->profile.avatar_rendered = render_unicode_blocks(
                    cached->image_data, cached->meta.type);
                user->cached_prefix_raw.clear();
            }
            return;  // Cache hit — no IQ needed
        }
    }

    // Cache miss — fetch from server
    std::string raw_id = stanza::uuid(acc.context);
    xmpp_stanza_t *iq = weechat::xep0084::request_avatar_data(acc.context, jid, raw_id.c_str());
    acc.connection.send(iq);
    xmpp_stanza_release(iq);
}

bool weechat::avatar::apply_pep_data_b64(account& acc,
                                         const char *jid,
                                         const std::string_view b64_data)
{
    if (b64_data.empty() || !jid || !*jid)
        return false;

    BIO *bio = BIO_new_mem_buf(b64_data.data(), static_cast<int>(b64_data.size()));
    BIO *b64 = BIO_new(BIO_f_base64());
    if (!bio || !b64)
    {
        BIO_free_all(bio);
        BIO_free_all(b64);
        return false;
    }

    bio = BIO_push(b64, bio);
    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);

    std::vector<uint8_t> image_data(b64_data.size());
    const int actual_len = BIO_read(bio, image_data.data(), static_cast<int>(image_data.size()));
    BIO_free_all(bio);
    if (actual_len <= 0)
        return false;

    image_data.resize(static_cast<size_t>(actual_len));
    const std::string hash = calculate_hash(image_data);

    data avatar_data;
    avatar_data.image_data = image_data;
    avatar_data.meta.id = hash;
    avatar_data.meta.bytes = static_cast<uint32_t>(actual_len);
    avatar_data.meta.type = "image/png";

    save_to_cache(acc, hash, avatar_data);

    if (weechat::user *user = weechat::user::search(&acc, jid))
    {
        user->profile.avatar_data = image_data;
        user->profile.avatar_rendered = render_unicode_blocks(image_data, avatar_data.meta.type);
        user->cached_prefix_raw.clear();
    }

    return true;
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
        user.cached_prefix_raw.clear();
        
        weechat::UiPort::for_buffer(acc.buffer)->printf_date_tags_network(0, "xmpp_avatar",
            fmt::format("Loaded cached avatar for {} (hash: {:.8}...)", user.id, hash));
    }
}

bool weechat::avatar::publish(account& acc, std::string_view filepath)
{
    const std::string filepath_str(filepath);

    // Read file bytes
    std::ifstream file(filepath_str, std::ios::binary | std::ios::ate);
    if (!file)
    {
        weechat::UiPort::for_buffer(acc.buffer)->printf_error(
            fmt::format("xmpp: /setavatar: cannot open file: {}", filepath_str));
        return false;
    }

    std::streamsize file_size = file.tellg();
    if (file_size <= 0 || file_size > 8 * 1024 * 1024)  // 8 MB sanity limit
    {
        weechat::UiPort::for_buffer(acc.buffer)->printf_error(
            fmt::format("xmpp: /setavatar: file too large or empty: {}", filepath_str));
        return false;
    }
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> image_bytes(static_cast<size_t>(file_size));
    if (!file.read(reinterpret_cast<char*>(image_bytes.data()), file_size))
    {
        weechat::UiPort::for_buffer(acc.buffer)->printf_error(
            fmt::format("xmpp: /setavatar: failed to read file: {}", filepath_str));
        return false;
    }

    // Detect MIME type from file extension
    std::string mime_type = "image/png";
    {
        auto dot = filepath.rfind('.');
        if (dot != std::string_view::npos)
        {
            std::string ext(filepath.substr(dot + 1));
            std::ranges::for_each(ext, [](char &c) { c = static_cast<char>(tolower(static_cast<unsigned char>(c))); });
            if (ext == "jpg" || ext == "jpeg")
                mime_type = "image/jpeg";
            else if (ext == "gif")
                mime_type = "image/gif";
            else if (ext == "webp")
                mime_type = "image/webp";
        }
    }

    // SHA-1 hash (hex) of raw bytes — used as PEP item id
    std::string hash = calculate_hash(image_bytes);

    // Read image dimensions from header bytes
    uint32_t img_width = 0, img_height = 0;
    if (mime_type == "image/png" && image_bytes.size() >= 24)
    {
        // PNG: width at offset 16, height at 20 (big-endian uint32)
        img_width  = (uint32_t(image_bytes[16]) << 24) | (uint32_t(image_bytes[17]) << 16)
                   | (uint32_t(image_bytes[18]) <<  8) |  uint32_t(image_bytes[19]);
        img_height = (uint32_t(image_bytes[20]) << 24) | (uint32_t(image_bytes[21]) << 16)
                   | (uint32_t(image_bytes[22]) <<  8) |  uint32_t(image_bytes[23]);
    }
    else if ((mime_type == "image/jpeg") && image_bytes.size() > 4)
    {
        // JPEG: scan for SOF0/SOF2 marker (0xFF 0xC0 / 0xFF 0xC2)
        for (size_t i = 2; i + 8 < image_bytes.size(); )
        {
            if (image_bytes[i] != 0xFF) { i++; continue; }
            uint8_t marker = image_bytes[i + 1];
            if (marker == 0xC0 || marker == 0xC2)
            {
                img_height = (uint32_t(image_bytes[i + 5]) << 8) | image_bytes[i + 6];
                img_width  = (uint32_t(image_bytes[i + 7]) << 8) | image_bytes[i + 8];
                break;
            }
            // Skip segment: length is big-endian uint16 at i+2
            if (i + 3 >= image_bytes.size()) break;
            uint16_t seg_len = (uint16_t(image_bytes[i + 2]) << 8) | image_bytes[i + 3];
            i += 2 + seg_len;
        }
    }

    // Base64-encode via OpenSSL BIO (no newlines)
    std::string b64;
    {
        BIO *bio_b64 = BIO_new(BIO_f_base64());
        BIO_set_flags(bio_b64, BIO_FLAGS_BASE64_NO_NL);
        BIO *bio_mem = BIO_new(BIO_s_mem());
        bio_b64 = BIO_push(bio_b64, bio_mem);
        std::span<const uint8_t> image_span = image_bytes;
        BIO_write(bio_b64, image_span.data(), static_cast<int>(image_span.size()));
        BIO_flush(bio_b64);
        BUF_MEM *bptr = nullptr;
        BIO_get_mem_ptr(bio_b64, &bptr);
        if (bptr && bptr->length > 0)
            b64.assign(bptr->data, bptr->length);
        BIO_free_all(bio_b64);
    }

    if (b64.empty())
    {
        weechat::UiPort::for_buffer(acc.buffer)->printf_error(
            "xmpp: /setavatar: base64 encoding failed");
        return false;
    }

    // Publish data node
    {
        std::shared_ptr<xmpp_stanza_t> iq {
            weechat::xep0084::publish_avatar_data(acc.context, b64.c_str(), hash.c_str()),
            xmpp_stanza_release};
        acc.connection.send(iq.get());
    }

    // Publish metadata node
    {
        std::shared_ptr<xmpp_stanza_t> iq {
            weechat::xep0084::publish_avatar_metadata(acc.context, hash.c_str(),
                mime_type.c_str(), image_bytes.size(), img_width, img_height),
            xmpp_stanza_release};
        acc.connection.send(iq.get());
    }

    // Save to local cache so we don't re-fetch what we just published
    {
        data avatar_data;
        avatar_data.image_data = image_bytes;
        avatar_data.meta.id     = hash;
        avatar_data.meta.type   = mime_type;
        avatar_data.meta.bytes  = static_cast<uint32_t>(image_bytes.size());
        avatar_data.meta.width  = img_width;
        avatar_data.meta.height = img_height;
        save_to_cache(acc, hash, avatar_data);
    }

    // Update own user profile so subsequent XEP-0153 presences carry the hash
    weechat::user *self = weechat::user::search(&acc, acc.jid().data());
    if (self)
    {
        self->profile.avatar_hash     = hash;
        self->profile.avatar_data     = image_bytes;
        self->profile.avatar_rendered = render_unicode_blocks(image_bytes, mime_type);
        self->cached_prefix_raw.clear();
    }

    weechat::UiPort::for_buffer(acc.buffer)->printf_network(
        fmt::format("Avatar published: {} ({} bytes, {}x{}, hash {:.8}...)",
                    mime_type, image_bytes.size(), img_width, img_height, hash));
    return true;
}
