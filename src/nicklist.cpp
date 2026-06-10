// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <string>
#include <string_view>

#include <fmt/core.h>

#include "nicklist.hh"
#include "user.hh"
#include "weechat/buffer_port.hh"

namespace
{

constexpr int k_online_tier = 0;
constexpr int k_offline_tier = 500;

[[nodiscard]] int muc_prefix_rank(char prefix)
{
    return weechat::user::muc_nicklist_prefix_rank(prefix);
}

[[nodiscard]] weechat::BufferPort &buffer_port()
{
    return weechat::BufferPort::default_port_ref();
}

void add_group_if_missing(struct t_gui_buffer *buffer, std::string_view name)
{
    if (!buffer_port().nicklist_search_group(buffer, nullptr, name))
    {
        (void)buffer_port().nicklist_add_group(buffer, nullptr, name);
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

    if (struct t_gui_nick_group *group =
            buffer_port().nicklist_search_group(buffer, nullptr, group_name))
    {
        return group;
    }

    return buffer_port().nicklist_add_group(buffer, nullptr, group_name);
}

struct t_gui_nick *weechat::nicklist::search_nick(
    struct t_gui_buffer *const buffer,
    const std::string_view name)
{
    if (!buffer || name.empty())
        return nullptr;
    return buffer_port().nicklist_search_nick(buffer, nullptr, name);
}

void weechat::nicklist::add_nick(
    struct t_gui_buffer *const buffer,
    struct t_gui_nick_group *const group,
    const std::string_view name,
    const std::string_view color,
    const std::string_view prefix,
    const std::string_view prefix_color)
{
    buffer_port().nicklist_add_nick(buffer, group, name, color, prefix, prefix_color, 1);
}

void weechat::nicklist::set_nick_property(
    struct t_gui_buffer *const buffer,
    struct t_gui_nick *const nick,
    const std::string_view property,
    const std::string_view value)
{
    buffer_port().nicklist_nick_set(buffer, nick, property, value);
}

void weechat::nicklist::remove_nick(
    struct t_gui_buffer *const buffer,
    const std::string_view name)
{
    buffer_port().nicklist_remove_nick(buffer, name);
}

void weechat::nicklist::refresh_separator(struct t_gui_buffer *buffer,
                                          const bool has_online,
                                          const bool has_offline)
{
    if (!buffer)
        return;

    struct t_gui_nick *existing = search_nick(buffer, k_separator_nick);

    if (has_online && has_offline)
    {
        if (!existing)
        {
            struct t_gui_nick_group *group =
                find_or_add_group(buffer, k_separator_group);
            if (group)
            {
                add_nick(buffer, group, k_separator_nick, "gray", "", "bar_fg");
            }
        }
        return;
    }

    if (existing)
        buffer_port().nicklist_remove_nick(buffer, k_separator_nick);
}