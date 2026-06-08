// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "line_store.hh"

#include <fmt/core.h>
#include <weechat/weechat-plugin.h>

#include "../plugin.hh"

namespace weechat {

std::string strip_status_glyph_suffix(std::string message)
{
    for (const std::string_view glyph :
         { k_glyph_pending, k_glyph_delivered, k_glyph_seen })
    {
        if (message.size() >= glyph.size()
            && message.compare(message.size() - glyph.size(), glyph.size(), glyph) == 0)
        {
            message.erase(message.size() - glyph.size());
            break;
        }
    }
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
            bool found = false;
            for (int n = 0; n < tags_count && !found; ++n)
            {
                const std::string str_tag = fmt::format("{}|tags_array", n);
                const char *tag = weechat_hdata_string(hdata_line_data, line_data, str_tag.c_str());
                if (tag && weechat_strcasecmp(tag, target_tag.c_str()) == 0)
                    found = true;
            }
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
                for (const std::string_view needle : needles)
                {
                    if (!needle.empty() && tag_view.contains(needle))
                        return true;
                }
            }
        }
        line = weechat_hdata_pointer(hdata_line, line, "prev_line");
    }
    return false;
}

}  // namespace weechat