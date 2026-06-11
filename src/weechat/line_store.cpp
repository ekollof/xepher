// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "weechat/line_store.hh"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fmt/core.h>
#include <memory>
#include <ranges>
#include <vector>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "xmpp/message_line_tag.hh"
#include "xmpp/message_reactions.hh"
#include "xmpp/message_reply.hh"

namespace weechat {

namespace {

[[nodiscard]] const char *line_data_tag_at(void *line_data,
                                           int index,
                                           struct t_hdata *hdata_line_data)
{
    const std::string str_tag = fmt::format("{}|tags_array", index);
    return weechat_hdata_string(hdata_line_data, line_data, str_tag.c_str());
}

[[nodiscard]] bool line_data_has_message_id(void *line_data,
                                            std::string_view target_id,
                                            struct t_hdata *hdata_line_data)
{
    if (!line_data || target_id.empty())
        return false;

    const int tags_count = weechat_hdata_integer(hdata_line_data, line_data, "tags_count");
    return std::ranges::any_of(std::views::iota(0, tags_count), [&](int n) {
        const char *tag = line_data_tag_at(line_data, n, hdata_line_data);
        return tag && xmpp::line_tag_matches_message_id(tag, target_id);
    });
}

[[nodiscard]] bool line_data_sender_matches(void *line_data,
                                            const xmpp::LineSenderVerify &verify,
                                            struct t_hdata *hdata_line_data)
{
    if (!line_data)
        return false;

    const int tags_count = weechat_hdata_integer(hdata_line_data, line_data, "tags_count");
    std::vector<std::string_view> tags;
    tags.reserve(static_cast<std::size_t>(tags_count));
    for (int n : std::views::iota(0, tags_count))
    {
        if (const char *tag = line_data_tag_at(line_data, n, hdata_line_data))
            tags.emplace_back(tag);
    }
    return xmpp::line_tags_verify_sender(tags, verify);
}

void line_data_set_message(void *line_data,
                           std::string_view message,
                           std::optional<std::string_view> replacement_tags,
                           struct t_hdata *hdata_line_data)
{
    struct t_hashtable *ht = weechat_hashtable_new(
        8, WEECHAT_HASHTABLE_STRING, WEECHAT_HASHTABLE_STRING, nullptr, nullptr);
    weechat_hashtable_set(ht, "message", std::string(message).c_str());
    if (replacement_tags)
        weechat_hashtable_set(ht, "tags", std::string(*replacement_tags).c_str());
    weechat_hdata_update(hdata_line_data, line_data, ht);
    weechat_hashtable_free(ht);
}

struct ParsedMessageBody {
    bool encrypted = false;
    std::string text;
};

[[nodiscard]] std::size_t utf8_advance(std::string_view text, std::size_t pos)
{
    if (pos >= text.size())
        return 0;
    const unsigned char lead = static_cast<unsigned char>(text[pos]);
    if (lead < 0x80)
        return 1;
    if ((lead & 0xE0) == 0xC0)
        return 2;
    if ((lead & 0xF0) == 0xE0)
        return 3;
    if ((lead & 0xF8) == 0xF0)
        return 4;
    return 1;
}

[[nodiscard]] std::size_t status_glyph_display_width(std::string_view glyph)
{
    if (glyph == k_glyph_delivered)
        return 1;
    if (glyph == k_glyph_pending || glyph == k_glyph_seen || glyph == k_encrypted_glyph)
        return 2;
    return 0;
}

[[nodiscard]] std::size_t text_display_width(std::string_view text)
{
    std::size_t width = 0;
    for (std::size_t pos = 0; pos < text.size();)
    {
        const std::size_t adv = utf8_advance(text, pos);
        const std::string_view run = text.substr(pos, adv);
        if (const std::size_t known = status_glyph_display_width(run); known > 0)
            width += known;
        else if (adv == 1 && static_cast<unsigned char>(text[pos]) < 0x80)
            width += 1;
        else
            width += 2;
        pos += adv;
    }
    return width;
}

[[nodiscard]] std::string pad_status_field(std::string_view content, const std::size_t target_width)
{
    std::string out(content);
    if (const std::size_t width = text_display_width(content); width < target_width)
        out.append(target_width - width, ' ');
    return out;
}

[[nodiscard]] std::optional<ParsedMessageBody>
try_parse_fixed_status_prefix(const std::string_view raw)
{
    if (raw.empty())
        return std::nullopt;

    std::size_t pos = 0;
    bool matched_delivery = false;

    constexpr std::array delivery_glyphs{
        k_glyph_seen, k_glyph_pending, k_glyph_delivered,
    };
    for (const std::string_view glyph : delivery_glyphs)
    {
        if (!raw.substr(pos).starts_with(glyph))
            continue;
        pos += glyph.size();
        matched_delivery = true;
        break;
    }

    if (!matched_delivery)
    {
        if (raw.size() < k_status_delivery_col_width
            || raw[0] != ' ' || raw[1] != ' ')
        {
            return std::nullopt;
        }
        pos = k_status_delivery_col_width;
    }
    else
    {
        const std::size_t field_start = 0;
        while (pos < raw.size() && raw[pos] == ' '
               && text_display_width(raw.substr(field_start, pos - field_start))
                      < k_status_delivery_col_width)
        {
            ++pos;
        }
        if (text_display_width(raw.substr(field_start, pos - field_start))
            != k_status_delivery_col_width)
        {
            return std::nullopt;
        }
    }

    if (pos >= raw.size() || raw[pos] != ' ')
        return std::nullopt;
    ++pos;

    ParsedMessageBody parsed;
    if (raw.substr(pos).starts_with(k_encrypted_glyph))
    {
        parsed.encrypted = true;
        pos += k_encrypted_glyph.size();
        const std::size_t lock_start = pos - k_encrypted_glyph.size();
        while (pos < raw.size() && raw[pos] == ' '
               && text_display_width(raw.substr(lock_start, pos - lock_start))
                      < k_status_lock_col_width)
        {
            ++pos;
        }
        if (text_display_width(raw.substr(lock_start, pos - lock_start))
            != k_status_lock_col_width)
        {
            return std::nullopt;
        }
        if (pos < raw.size() && raw[pos] == ' ')
            ++pos;
    }

    parsed.text = std::string(raw.substr(pos));
    return parsed;
}

void trim_leading_ascii_ws(std::string &message);

std::size_t glyph_prefix_length(std::string_view message, std::string_view glyph);

void trim_leading_ascii_ws(std::string &message)
{
    while (!message.empty()
           && std::isspace(static_cast<unsigned char>(message.front())))
    {
        message.erase(message.begin());
    }
}

std::size_t glyph_prefix_length(const std::string_view message, const std::string_view glyph)
{
    if (message.starts_with(glyph))
        return glyph.size();
    if (message.size() > glyph.size()
        && message[0] == ' '
        && message.substr(1).starts_with(glyph))
    {
        return glyph.size() + 1;
    }
    return 0;
}

}  // namespace

std::string strip_status_glyph_prefix(std::string message)
{
    while (!message.empty()
           && std::isspace(static_cast<unsigned char>(message.front())))
    {
        message.erase(message.begin());
    }

    constexpr std::array glyphs{ k_glyph_seen, k_glyph_delivered, k_glyph_pending };
    for (const std::string_view glyph : glyphs)
    {
        if (const std::size_t len = glyph_prefix_length(message, glyph); len > 0)
        {
            message.erase(0, len);
            break;
        }
    }
    return message;
}

std::string strip_status_glyph_suffix(std::string message)
{
    constexpr std::array glyphs{ k_glyph_seen, k_glyph_delivered, k_glyph_pending };
    for (const std::string_view suffix : glyphs)
    {
        if (message.size() > suffix.size() && message[message.size() - suffix.size() - 1] == ' ')
        {
            if (message.substr(message.size() - suffix.size()) == suffix)
                return message.substr(0, message.size() - suffix.size() - 1);
        }
        if (message.ends_with(suffix))
            return message.substr(0, message.size() - suffix.size());
    }
    return message;
}

std::string strip_delivery_glyphs(std::string message)
{
    message = strip_status_glyph_prefix(std::move(message));
    return strip_status_glyph_suffix(std::move(message));
}

std::string format_message_status_prefix(const std::string_view delivery_glyph,
                                         const bool encrypted)
{
    std::string out;
    if (delivery_glyph.empty())
        out.assign(k_status_delivery_col_width, ' ');
    else
        out = pad_status_field(delivery_glyph, k_status_delivery_col_width);
    out += ' ';
    if (encrypted)
    {
        out += pad_status_field(k_encrypted_glyph, k_status_lock_col_width);
        out += ' ';
    }
    return out;
}

namespace {

ParsedMessageBody parse_message_body_status(const std::string_view raw)
{
    if (auto fixed = try_parse_fixed_status_prefix(raw))
        return *fixed;

    ParsedMessageBody parsed;
    std::string work = strip_delivery_glyphs(std::string(raw));
    trim_leading_ascii_ws(work);

    constexpr std::array delivery_glyphs{
        k_glyph_seen, k_glyph_delivered, k_glyph_pending,
    };
    for (const std::string_view delivery_glyph : delivery_glyphs)
    {
        if (const std::size_t len = glyph_prefix_length(work, delivery_glyph); len > 0)
        {
            work.erase(0, len);
            break;
        }
    }
    trim_leading_ascii_ws(work);

    if (work.starts_with(k_encrypted_glyph))
    {
        parsed.encrypted = true;
        work.erase(0, k_encrypted_glyph.size());
    }
    trim_leading_ascii_ws(work);
    parsed.text = std::move(work);
    return parsed;
}

std::string rebuild_message_body_status(const std::string_view delivery_glyph,
                                          const ParsedMessageBody &parsed)
{
    return format_message_status_prefix(delivery_glyph, parsed.encrypted) + parsed.text;
}

}  // namespace

std::string strip_message_status_prefix(std::string message)
{
    return parse_message_body_status(message).text;
}

std::string clean_editable_line_body(const std::string_view raw)
{
    if (raw.empty())
        return {};

    std::string_view body_sv = raw;
    if (const auto tab = body_sv.find('\t'); tab != std::string_view::npos)
        body_sv = body_sv.substr(tab + 1);
    else if (!body_sv.empty() && body_sv.front() == '\t')
        body_sv.remove_prefix(1);

    std::string body(body_sv);
    body = strip_message_status_prefix(std::move(body));

    if (weechat::plugin::instance)
    {
        std::unique_ptr<char, decltype(&free)> stripped(
            weechat_string_remove_color(body.c_str(), nullptr), &free);
        if (stripped)
            body = strip_message_status_prefix(stripped.get());
    }

    trim_leading_ascii_ws(body);
    return body;
}

std::string format_self_pm_line(const std::string_view prefix,
                                const std::string_view body,
                                const std::string_view glyph,
                                const bool encrypted)
{
    return fmt::format("{}\t{}{}",
                       prefix,
                       format_message_status_prefix(glyph, encrypted),
                       body);
}

std::string apply_delivery_glyph_to_line(std::string line, const std::string_view glyph)
{
    const auto tab = line.find('\t');
    if (tab == std::string::npos)
    {
        const ParsedMessageBody parsed = parse_message_body_status(line);
        return rebuild_message_body_status(glyph, parsed);
    }

    const std::string prefix = strip_status_glyph_suffix(line.substr(0, tab));
    const ParsedMessageBody parsed = parse_message_body_status(line.substr(tab + 1));
    return fmt::format("{}\t{}", prefix, rebuild_message_body_status(glyph, parsed));
}

bool line_store_update_line_glyph_by_tag(struct t_gui_buffer *buffer,
                                         std::string_view acked_id,
                                         std::string_view new_glyph)
{
    if (!buffer || acked_id.empty())
        return false;

    static struct t_hdata *hdata_buffer = weechat_hdata_get("buffer");
    static struct t_hdata *hdata_lines = weechat_hdata_get("lines");
    static struct t_hdata *hdata_line = weechat_hdata_get("line");
    static struct t_hdata *hdata_line_data = weechat_hdata_get("line_data");

    void *lines = weechat_hdata_pointer(hdata_buffer, buffer, "lines");
    if (!lines)
        return false;

    const std::string target_tag = fmt::format("id_{}", acked_id);
    void *last_line = weechat_hdata_pointer(hdata_lines, lines, "last_line");
    while (last_line)
    {
        void *line_data = weechat_hdata_pointer(hdata_line, last_line, "data");
        if (line_data)
        {
            const int tags_count = weechat_hdata_integer(hdata_line_data, line_data, "tags_count");
            const bool found = std::ranges::any_of(std::views::iota(0, tags_count), [&](int n) {
                const char *tag = line_data_tag_at(line_data, n, hdata_line_data);
                return tag && weechat_strcasecmp(tag, target_tag.c_str()) == 0;
            });
            if (found)
            {
                const char *cur_msg = weechat_hdata_string(hdata_line_data, line_data, "message");
                std::string new_msg = apply_delivery_glyph_to_line(
                    cur_msg ? cur_msg : "", new_glyph);

                struct t_hashtable *ht = weechat_hashtable_new(
                    4, WEECHAT_HASHTABLE_STRING, WEECHAT_HASHTABLE_STRING, nullptr, nullptr);
                weechat_hashtable_set(ht, "message", new_msg.c_str());
                weechat_hdata_update(hdata_line_data, line_data, ht);
                weechat_hashtable_free(ht);
                return true;
            }
        }
        last_line = weechat_hdata_pointer(hdata_line, last_line, "prev_line");
    }
    return false;
}

bool line_store_buffer_contains_any_tag(struct t_gui_buffer *buffer,
                                        std::initializer_list<std::string_view> needles,
                                        int max_scan)
{
    if (!buffer || needles.size() == 0 || max_scan <= 0)
        return false;

    static struct t_hdata *hdata_buffer = weechat_hdata_get("buffer");
    static struct t_hdata *hdata_lines = weechat_hdata_get("lines");
    static struct t_hdata *hdata_line = weechat_hdata_get("line");
    static struct t_hdata *hdata_line_data = weechat_hdata_get("line_data");

    void *lines = weechat_hdata_pointer(hdata_buffer, buffer, "lines");
    if (!lines)
        return false;

    void *line = weechat_hdata_pointer(hdata_lines, lines, "last_line");
    for (int scan = 0; line && scan < max_scan; ++scan)
    {
        void *line_data = weechat_hdata_pointer(hdata_line, line, "data");
        if (line_data)
        {
            const char *tags = weechat_hdata_string(hdata_line_data, line_data, "tags");
            if (tags)
            {
                const std::string_view tag_view(tags);
                if (std::ranges::any_of(needles, [&](std::string_view needle) {
                        return !needle.empty() && tag_view.contains(needle);
                    }))
                    return true;
            }
        }
        line = weechat_hdata_pointer(hdata_line, line, "prev_line");
    }
    return false;
}

LineStoreLookupResult line_store_find_message_line_for_sender(
    struct t_gui_buffer *buffer,
    std::string_view target_id,
    std::string_view sender_key,
    int max_scan)
{
    if (!buffer || target_id.empty() || sender_key.empty() || max_scan <= 0)
        return LineStoreLookupResult::NotFound;

    const xmpp::LineSenderVerify verify{ .sender_key = std::string(sender_key) };

    static struct t_hdata *hdata_buffer = weechat_hdata_get("buffer");
    static struct t_hdata *hdata_lines = weechat_hdata_get("lines");
    static struct t_hdata *hdata_line = weechat_hdata_get("line");
    static struct t_hdata *hdata_line_data = weechat_hdata_get("line_data");

    void *lines = weechat_hdata_pointer(hdata_buffer, buffer, "lines");
    if (!lines)
        return LineStoreLookupResult::NotFound;

    void *last_line = weechat_hdata_pointer(hdata_lines, lines, "last_line");
    for (int scan = 0; last_line && scan < max_scan; ++scan)
    {
        void *line_data = weechat_hdata_pointer(hdata_line, last_line, "data");
        if (line_data && line_data_has_message_id(line_data, target_id, hdata_line_data))
        {
            if (line_data_sender_matches(line_data, verify, hdata_line_data))
                return LineStoreLookupResult::Found;
            return LineStoreLookupResult::SenderRejected;
        }
        last_line = weechat_hdata_pointer(hdata_line, last_line, "prev_line");
    }
    return LineStoreLookupResult::NotFound;
}

bool line_store_update_message_by_id(struct t_gui_buffer *buffer,
                                     std::string_view target_id,
                                     std::string_view new_message,
                                     int max_scan)
{
    if (!buffer || target_id.empty() || new_message.empty() || max_scan <= 0)
        return false;

    static struct t_hdata *hdata_buffer = weechat_hdata_get("buffer");
    static struct t_hdata *hdata_lines = weechat_hdata_get("lines");
    static struct t_hdata *hdata_line = weechat_hdata_get("line");
    static struct t_hdata *hdata_line_data = weechat_hdata_get("line_data");

    void *lines = weechat_hdata_pointer(hdata_buffer, buffer, "lines");
    if (!lines)
        return false;

    void *last_line = weechat_hdata_pointer(hdata_lines, lines, "last_line");
    for (int scan = 0; last_line && scan < max_scan; ++scan)
    {
        void *line_data = weechat_hdata_pointer(hdata_line, last_line, "data");
        if (line_data && line_data_has_message_id(line_data, target_id, hdata_line_data))
        {
            line_data_set_message(line_data, new_message, std::nullopt, hdata_line_data);
            return true;
        }
        last_line = weechat_hdata_pointer(hdata_line, last_line, "prev_line");
    }
    return false;
}

LineStoreLookupResult line_store_tombstone_message_by_id(
    struct t_gui_buffer *buffer,
    std::string_view target_id,
    std::string_view tombstone_message,
    std::string_view replacement_tags,
    int max_scan)
{
    if (!buffer || target_id.empty() || max_scan <= 0)
        return LineStoreLookupResult::NotFound;

    static struct t_hdata *hdata_buffer = weechat_hdata_get("buffer");
    static struct t_hdata *hdata_lines = weechat_hdata_get("lines");
    static struct t_hdata *hdata_line = weechat_hdata_get("line");
    static struct t_hdata *hdata_line_data = weechat_hdata_get("line_data");

    void *lines = weechat_hdata_pointer(hdata_buffer, buffer, "lines");
    if (!lines)
        return LineStoreLookupResult::NotFound;

    void *last_line = weechat_hdata_pointer(hdata_lines, lines, "last_line");
    for (int scan = 0; last_line && scan < max_scan; ++scan)
    {
        void *line_data = weechat_hdata_pointer(hdata_line, last_line, "data");
        if (line_data && line_data_has_message_id(line_data, target_id, hdata_line_data))
        {
            line_data_set_message(line_data, tombstone_message, replacement_tags, hdata_line_data);
            return LineStoreLookupResult::Found;
        }
        last_line = weechat_hdata_pointer(hdata_line, last_line, "prev_line");
    }
    return LineStoreLookupResult::NotFound;
}

LineStoreLookupResult line_store_tombstone_retraction_by_id(
    struct t_gui_buffer *buffer,
    std::string_view target_id,
    std::string_view tombstone_message,
    std::string_view replacement_tags,
    std::string_view sender_key,
    std::string_view occupant_id,
    bool prefer_occupant_id,
    int max_scan)
{
    if (!buffer || target_id.empty() || max_scan <= 0)
        return LineStoreLookupResult::NotFound;

    xmpp::LineSenderVerify verify{ .sender_key = std::string(sender_key) };
    if (!occupant_id.empty())
        verify.occupant_id = std::string(occupant_id);
    verify.prefer_occupant_id = prefer_occupant_id;

    static struct t_hdata *hdata_buffer = weechat_hdata_get("buffer");
    static struct t_hdata *hdata_lines = weechat_hdata_get("lines");
    static struct t_hdata *hdata_line = weechat_hdata_get("line");
    static struct t_hdata *hdata_line_data = weechat_hdata_get("line_data");

    void *lines = weechat_hdata_pointer(hdata_buffer, buffer, "lines");
    if (!lines)
        return LineStoreLookupResult::NotFound;

    void *last_line = weechat_hdata_pointer(hdata_lines, lines, "last_line");
    for (int scan = 0; last_line && scan < max_scan; ++scan)
    {
        void *line_data = weechat_hdata_pointer(hdata_line, last_line, "data");
        if (line_data && line_data_has_message_id(line_data, target_id, hdata_line_data))
        {
            if (!line_data_sender_matches(line_data, verify, hdata_line_data))
                return LineStoreLookupResult::SenderRejected;

            line_data_set_message(line_data, tombstone_message, replacement_tags, hdata_line_data);
            return LineStoreLookupResult::Found;
        }
        last_line = weechat_hdata_pointer(hdata_line, last_line, "prev_line");
    }
    return LineStoreLookupResult::NotFound;
}

std::optional<ReplyQuoteLookup> line_store_lookup_reply_quote(
    struct t_gui_buffer *buffer, std::string_view target_id, int max_scan)
{
    if (!buffer || target_id.empty() || max_scan <= 0)
        return std::nullopt;

    static struct t_hdata *hdata_buffer = weechat_hdata_get("buffer");
    static struct t_hdata *hdata_lines = weechat_hdata_get("lines");
    static struct t_hdata *hdata_line = weechat_hdata_get("line");
    static struct t_hdata *hdata_line_data = weechat_hdata_get("line_data");

    void *lines = weechat_hdata_pointer(hdata_buffer, buffer, "lines");
    if (!lines)
        return std::nullopt;

    void *body_line = nullptr;
    void *scan = weechat_hdata_pointer(hdata_lines, lines, "last_line");
    bool in_group = false;
    for (int n = 0; scan && n < max_scan; ++n)
    {
        void *ld = weechat_hdata_pointer(hdata_line, scan, "data");
        const bool has_tag = ld && line_data_has_message_id(ld, target_id, hdata_line_data);
        if (has_tag)
        {
            in_group = true;
            const char *msg = weechat_hdata_string(hdata_line_data, ld, "message");
            if (!xmpp::extract_line_body_text(msg ? std::string_view(msg) : std::string_view{})
                    .empty())
                body_line = scan;
        }
        else if (in_group)
            break;

        scan = weechat_hdata_pointer(hdata_line, scan, "prev_line");
    }

    if (!body_line)
        return std::nullopt;

    void *line_data = weechat_hdata_pointer(hdata_line, body_line, "data");
    if (!line_data)
        return std::nullopt;

    const char *orig_message = weechat_hdata_string(hdata_line_data, line_data, "message");
    if (!orig_message)
        return std::nullopt;

    std::string clean_body =
        xmpp::extract_line_body_text(orig_message ? std::string_view(orig_message)
                                                  : std::string_view{});
    clean_body = xmpp::strip_leading_reply_chain(clean_body);

    ReplyQuoteLookup lookup;
    lookup.excerpt = xmpp::build_reply_excerpt(clean_body);

    const int tags_count = weechat_hdata_integer(hdata_line_data, line_data, "tags_count");
    std::vector<std::string_view> tags;
    tags.reserve(static_cast<std::size_t>(tags_count));
    for (int n : std::views::iota(0, tags_count))
    {
        if (const char *tag = line_data_tag_at(line_data, n, hdata_line_data))
            tags.emplace_back(tag);
    }
    if (auto nick = xmpp::nick_from_line_tags(tags))
        lookup.quote_nick = std::move(*nick);

    return lookup;
}

bool line_store_apply_reactions_by_id(struct t_gui_buffer *buffer,
                                      std::string_view target_id,
                                      std::string_view emojis,
                                      int max_scan)
{
    if (!buffer || target_id.empty() || max_scan <= 0)
        return false;

    static struct t_hdata *hdata_buffer = weechat_hdata_get("buffer");
    static struct t_hdata *hdata_lines = weechat_hdata_get("lines");
    static struct t_hdata *hdata_line = weechat_hdata_get("line");
    static struct t_hdata *hdata_line_data = weechat_hdata_get("line_data");

    void *lines = weechat_hdata_pointer(hdata_buffer, buffer, "lines");
    if (!lines)
        return false;

    void *last_line = weechat_hdata_pointer(hdata_lines, lines, "last_line");
    for (int scan = 0; last_line && scan < max_scan; ++scan)
    {
        void *line_data = weechat_hdata_pointer(hdata_line, last_line, "data");
        if (line_data && line_data_has_message_id(line_data, target_id, hdata_line_data))
        {
            const char *orig_message = weechat_hdata_string(hdata_line_data, line_data, "message");
            const std::string new_message = xmpp::format_message_with_reactions(
                orig_message ? orig_message : "", emojis);
            line_data_set_message(line_data, new_message, std::nullopt, hdata_line_data);
            return true;
        }
        last_line = weechat_hdata_pointer(hdata_line, last_line, "prev_line");
    }
    return false;
}

}  // namespace weechat