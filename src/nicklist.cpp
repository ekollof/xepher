// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <string>
#include <string_view>

#include <fmt/core.h>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "nicklist.hh"
#include "user.hh"

namespace
{

constexpr int k_online_tier = 0;
constexpr int k_offline_tier = 500;

[[nodiscard]] int muc_prefix_rank(char prefix)
{
    return weechat::user::muc_nicklist_prefix_rank(prefix);
}

void add_group_if_missing(struct t_gui_buffer *buffer, std::string_view name)
{
    if (!weechat_nicklist_search_group(buffer, nullptr, std::string(name).c_str()))
    {
        weechat_nicklist_add_group(buffer, nullptr, std::string(name).c_str(),
                                   "weechat.color.nicklist_group", 1);
    }
}

void add_muc_prefix_groups(struct t_gui_buffer *buffer, const char prefix)
{
    add_group_if_missing(buffer,
                         weechat::nicklist::muc_group_name(prefix, true));
    add_group_if_missing(buffer,
                         weechat::nicklist::muc_group_name(prefix, false));
}

} // namespace

std::string weechat::nicklist::muc_group_name(const char prefix, const bool online)
{
    const int tier = online ? k_online_tier + muc_prefix_rank(prefix)
                            : k_offline_tier + muc_prefix_rank(prefix);
    return fmt::format("{:03d}|{}", tier, prefix);
}

std::string weechat::nicklist::account_group_name(const bool online)
{
    const int tier = online ? k_online_tier : k_offline_tier;
    return fmt::format("{:03d}|.", tier);
}

void weechat::nicklist::ensure_muc_groups(struct t_gui_buffer *buffer)
{
    if (!buffer)
        return;

    add_muc_prefix_groups(buffer, '~');
    add_muc_prefix_groups(buffer, '&');
    add_muc_prefix_groups(buffer, '@');
    add_muc_prefix_groups(buffer, '%');
    add_muc_prefix_groups(buffer, '+');
    add_muc_prefix_groups(buffer, '?');
    add_muc_prefix_groups(buffer, '!');
    add_muc_prefix_groups(buffer, '.');
    add_group_if_missing(buffer, k_separator_group);
}

void weechat::nicklist::ensure_account_groups(struct t_gui_buffer *buffer)
{
    if (!buffer)
        return;

    add_group_if_missing(buffer, account_group_name(true));
    add_group_if_missing(buffer, account_group_name(false));
    add_group_if_missing(buffer, k_separator_group);
}

struct t_gui_nick_group *weechat::nicklist::find_or_add_group(
    struct t_gui_buffer *buffer,
    const std::string_view group_name)
{
    if (!buffer || group_name.empty())
        return nullptr;

    const std::string name(group_name);
    if (struct t_gui_nick_group *group =
            weechat_nicklist_search_group(buffer, nullptr, name.c_str()))
    {
        return group;
    }

    return weechat_nicklist_add_group(buffer, nullptr, name.c_str(),
                                      "weechat.color.nicklist_group", 1);
}

void weechat::nicklist::refresh_separator(struct t_gui_buffer *buffer,
                                          const bool has_online,
                                          const bool has_offline)
{
    if (!buffer)
        return;

    struct t_gui_nick *existing =
        weechat_nicklist_search_nick(buffer, nullptr, k_separator_nick);

    if (has_online && has_offline)
    {
        if (!existing)
        {
            struct t_gui_nick_group *group =
                find_or_add_group(buffer, k_separator_group);
            if (group)
            {
                weechat_nicklist_add_nick(buffer, group, k_separator_nick,
                                          "gray", "", "bar_fg", 1);
            }
        }
        return;
    }

    if (existing)
        weechat_nicklist_remove_nick(buffer, existing);
}