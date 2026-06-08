// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "line_store.hh"

#include <algorithm>
#include <array>
#include <fmt/core.h>
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

}  // namespace

std::string strip_status_glyph_suffix(std::string message)
{
    constexpr std::array glyphs{ k_glyph_pending, k_glyph_delivered, k_glyph_seen };
    const auto glyph = std::ranges::find_if(glyphs, [&](std::string_view suffix) {
        return message.ends_with(suffix);
    });
    if (glyph != glyphs.end())
        message.erase(message.size() - glyph->size());
    return message;
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
                std::string new_msg = cur_msg ? cur_msg : "";
                new_msg = strip_status_glyph_suffix(std::move(new_msg));
                new_msg += new_glyph;

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
    std::string_view sender_key)
{
    if (!buffer || target_id.empty() || sender_key.empty())
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
    while (last_line)
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
                                     std::string_view new_message)
{
    if (!buffer || target_id.empty() || new_message.empty())
        return false;

    static struct t_hdata *hdata_buffer = weechat_hdata_get("buffer");
    static struct t_hdata *hdata_lines = weechat_hdata_get("lines");
    static struct t_hdata *hdata_line = weechat_hdata_get("line");
    static struct t_hdata *hdata_line_data = weechat_hdata_get("line_data");

    void *lines = weechat_hdata_pointer(hdata_buffer, buffer, "lines");
    if (!lines)
        return false;

    void *last_line = weechat_hdata_pointer(hdata_lines, lines, "last_line");
    while (last_line)
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
    std::string_view replacement_tags)
{
    if (!buffer || target_id.empty())
        return LineStoreLookupResult::NotFound;

    static struct t_hdata *hdata_buffer = weechat_hdata_get("buffer");
    static struct t_hdata *hdata_lines = weechat_hdata_get("lines");
    static struct t_hdata *hdata_line = weechat_hdata_get("line");
    static struct t_hdata *hdata_line_data = weechat_hdata_get("line_data");

    void *lines = weechat_hdata_pointer(hdata_buffer, buffer, "lines");
    if (!lines)
        return LineStoreLookupResult::NotFound;

    void *last_line = weechat_hdata_pointer(hdata_lines, lines, "last_line");
    while (last_line)
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
    bool prefer_occupant_id)
{
    if (!buffer || target_id.empty())
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
    while (last_line)
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
    struct t_gui_buffer *buffer, std::string_view target_id)
{
    if (!buffer || target_id.empty())
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
    while (scan)
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
                                      std::string_view emojis)
{
    if (!buffer || target_id.empty())
        return false;

    static struct t_hdata *hdata_buffer = weechat_hdata_get("buffer");
    static struct t_hdata *hdata_lines = weechat_hdata_get("lines");
    static struct t_hdata *hdata_line = weechat_hdata_get("line");
    static struct t_hdata *hdata_line_data = weechat_hdata_get("line_data");

    void *lines = weechat_hdata_pointer(hdata_buffer, buffer, "lines");
    if (!lines)
        return false;

    void *last_line = weechat_hdata_pointer(hdata_lines, lines, "last_line");
    while (last_line)
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