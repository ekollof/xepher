// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <stdint.h>
#include <stdexcept>
#include <string_view>
#include <algorithm>
#include <ranges>
#include <vector>
#include <strophe.h>
#include <weechat/weechat-plugin.h>
#include <fmt/core.h>

#include "plugin.hh"
#include "weechat/runtime_port.hh"
#include "color.hh"
#include "xmpp/node.hh"
#include "account.hh"
#include "user.hh"
#include "channel.hh"
#include "nicklist.hh"
#include "avatar.hh"

namespace {

unsigned long nick_hash(std::string_view name)
{
    unsigned long hash = 5381;
    std::ranges::for_each(name, [&](char c) {
        hash = ((hash << 5) + hash) + static_cast<unsigned char>(c);
    });
    return hash;
}

[[nodiscard]] const char *nicklist_color_name(const weechat::user &user,
                                              const std::string &palette_color)
{
    if (!user.is_online)
        return "weechat.color.nicklist_offline";
    if (user.is_away)
        return "weechat.color.nicklist_away";
    // WeeChat nicklist APIs take string_view; never pass nullptr.
    return palette_color.empty() ? "weechat.color.chat_nick" : palette_color.c_str();
}

std::string compute_nick_color(std::string_view name)
{
    const char *colors_str = weechat_config_string(
        weechat_config_get("weechat.color.chat_nick_colors"));
    if (!colors_str || !colors_str[0])
        return {};

    std::vector<std::string> palette;
    std::string_view sv(colors_str);
    for (auto r : sv | std::views::split(','))
    {
        std::string s(r.begin(), r.end());
        if (!s.empty())
            palette.push_back(std::move(s));
    }

    if (palette.empty()) return {};
    return palette[nick_hash(name) % palette.size()];
}

}

std::string weechat::user::get_colour()
{
    if (cached_nick_color.empty() && !this->profile.display_name.empty())
    {
        std::string raw_color = compute_nick_color(this->profile.display_name);
        cached_nick_color = weechat::RuntimePort::default_runtime().xmpp_color(raw_color);
    }
    return cached_nick_color;
}

std::string weechat::user::get_colour(std::string_view name)
{
    return weechat::RuntimePort::default_runtime().xmpp_color(compute_nick_color(name));
}

std::string weechat::user::get_colour_for_nicklist()
{
    if (cached_nick_color_name.empty())
    {
        std::string_view color_name = this->profile.display_name;
        if (color_name.empty())
        {
            const std::string bare = jid(nullptr, this->id).bare;
            color_name = bare.empty() ? std::string_view(this->id) : bare;
        }
        if (!color_name.empty())
            cached_nick_color_name = compute_nick_color(color_name);
    }
    return cached_nick_color_name;
}

std::string weechat::user::get_colour_for_nicklist(std::string_view name)
{
    return compute_nick_color(name);
}

std::string weechat::user::as_prefix_raw()
{
    if (cached_prefix_raw.empty())
    {
        std::string prefix;
        if (!this->profile.avatar_rendered.empty())
            prefix = this->profile.avatar_rendered + " ";
        std::string color = this->get_colour();
        std::string reset = weechat::RuntimePort::default_runtime().xmpp_color("reset");
        cached_prefix_raw = fmt::format("{}{}{}{}", color, prefix, this->profile.display_name, reset);
    }
    return cached_prefix_raw;
}

std::string weechat::user::as_prefix_raw(std::string_view name)
{
    std::string color = get_colour(name);
    std::string reset = weechat::RuntimePort::default_runtime().xmpp_color("reset");
    return fmt::format("{}{}{}", color, name, reset);
}

std::string weechat::user::as_prefix()
{
    std::string prefix;
    if (!this->profile.avatar_rendered.empty())
        prefix = this->profile.avatar_rendered + " ";
    return prefix + weechat::user::as_prefix(this->profile.display_name);
}

std::string weechat::user::as_prefix(std::string_view name)
{
    return fmt::format("{}{}\t",
                       weechat::user::get_colour(name),
                       name);
}

weechat::user *weechat::user::bot_search(weechat::account *account,
                                         std::string_view pgp_id)
{
    if (!account || pgp_id.empty())
        return nullptr;

    for (auto& [key, u] : account->users)
    {
        if (u.profile.pgp_id.has_value() && u.profile.pgp_id.value() == pgp_id)
            return &u;
    }

    return nullptr;
}

weechat::user *weechat::user::search(weechat::account *account,
                                     std::string_view id)
{
    if (!account || id.empty())
        return nullptr;

    if (auto it = account->users.find(id); it != account->users.end()) {
        auto& [_, u] = *it;
        return &u;
    }

    return nullptr;
}

char weechat::user::muc_nicklist_prefix(
    std::optional<std::string_view> role,
    std::optional<std::string_view> affiliation)
{
    char group = '.';
    if (affiliation && *affiliation == "outcast")
        group = '!';
    if (role && *role == "visitor")
        group = '?';
    if (role && *role == "participant")
        group = '+';
    if (affiliation && *affiliation == "member")
        group = '%';
    if (role && *role == "moderator")
        group = '@';
    if (affiliation && *affiliation == "admin")
        group = '&';
    if (affiliation && *affiliation == "owner")
        group = '~';
    return group;
}

int weechat::user::muc_nicklist_prefix_rank(char prefix)
{
    switch (prefix)
    {
        case '~': return 0;
        case '&': return 1;
        case '@': return 2;
        case '%': return 3;
        case '+': return 4;
        case '?': return 5;
        case '!': return 6;
        default:  return 7;
    }
}

char weechat::user::muc_nicklist_prefix() const
{
    return muc_nicklist_prefix(profile.role, profile.affiliation);
}

std::string weechat::user::muc_nicklist_name(weechat::channel *channel) const
{
    std::string name = profile.display_name;
    if (name.empty())
        name = jid(nullptr, id).resource;

    std::string name_bare = jid(nullptr, name).bare;
    if (!name_bare.empty()
        && weechat_strcasecmp(name_bare.c_str(), channel->id.data()) == 0)
    {
        std::string resource = jid(nullptr, name).resource;
        if (!resource.empty())
            name = std::move(resource);
    }
    if (name.empty())
        name = jid(nullptr, id).resource;
    return name;
}

std::string weechat::user::muc_display_nick(
    weechat::channel *channel, std::string_view member_id, const user *occupant)
{
    if (occupant)
        return occupant->muc_nicklist_name(channel);

    auto slash = member_id.rfind('/');
    if (slash != std::string::npos && slash + 1 < member_id.size())
        return std::string(member_id.substr(slash + 1));
    return std::string(member_id);
}

void weechat::user::nicklist_add(weechat::account *account,
                                 weechat::channel *channel)
{
    struct t_gui_nick_group *ptr_group;
    struct t_gui_buffer *ptr_buffer;
    std::string nick_buf;
    const char *name = nullptr;

    if (channel && channel->type == weechat::channel::chat_type::MUC)
    {
        nick_buf = muc_nicklist_name(channel);
        if (nick_buf.empty())
            return;
        name = nick_buf.c_str();
    }
    else if (!channel)
    {
        nick_buf = jid(nullptr, this->id).bare;
        if (nick_buf.empty())
            nick_buf = this->id;
        name = nick_buf.c_str();
    }
    else
    {
        if (this->profile.display_name.empty())
        {
            nick_buf = jid(nullptr, this->id).bare;
            if (nick_buf.empty())
                nick_buf = this->id;
            name = nick_buf.c_str();
        }
        else
            name = this->profile.display_name.c_str();
    }

    if (!name || !*name)
        return;

    ptr_buffer = channel ? channel->buffer : account->buffer;

    const char prefix = muc_nicklist_prefix();
    const std::string group_name =
        channel && channel->type == weechat::channel::chat_type::MUC
            ? nicklist::muc_group_name(prefix, is_online)
            : nicklist::account_group_name(is_online);
    char prefix_buf[2] = {prefix, '\0'};
    const char *nick_prefix =
        channel && channel->type == weechat::channel::chat_type::MUC
            ? prefix_buf
            : ".";
    ptr_group = nicklist::find_or_add_group(ptr_buffer, group_name);
    auto colour = get_colour_for_nicklist();
    nicklist::add_nick(ptr_buffer,
                       ptr_group,
                       name,
                       nicklist_color_name(*this, colour),
                       nick_prefix,
                       "bar_fg");

    if (channel && channel->type == weechat::channel::chat_type::MUC)
    {
        int online = 0;
        int offline = 0;
        channel->count_nicklist_presence(online, offline);
        nicklist::refresh_separator(ptr_buffer, online > 0, offline > 0);
    }
    else if (!channel)
    {
        int online = 0;
        int offline = 0;
        account->count_roster_nicklist_presence(online, offline);
        nicklist::refresh_separator(ptr_buffer, online > 0, offline > 0);
    }
}

void weechat::user::nicklist_set_color(weechat::account *account,
                                       weechat::channel *channel)
{
    std::string nick_buf;
    const char *name = nullptr;
    if (channel && channel->type == weechat::channel::chat_type::MUC)
    {
        nick_buf = muc_nicklist_name(channel);
        if (nick_buf.empty())
            return;
        name = nick_buf.c_str();
    }
    else if (!channel)
    {
        nick_buf = jid(nullptr, this->id).bare;
        if (nick_buf.empty())
            nick_buf = this->id;
        name = nick_buf.c_str();
    }
    else
    {
        name = this->profile.display_name.c_str();
    }

    struct t_gui_buffer *ptr_buffer = channel ? channel->buffer : account->buffer;
    if (struct t_gui_nick *ptr_nick = nicklist::search_nick(ptr_buffer, name))
    {
        auto colour = get_colour_for_nicklist();
        nicklist::set_nick_property(ptr_buffer,
                                    ptr_nick,
                                    "color",
                                    nicklist_color_name(*this, colour));
    }
}

void weechat::user::nicklist_remove(weechat::account *account,
                                    weechat::channel *channel)
{
    struct t_gui_buffer *ptr_buffer;

    ptr_buffer = channel ? channel->buffer : account->buffer;

    auto try_remove = [&](const char *candidate) {
        if (!candidate)
            return;
        nicklist::remove_nick(ptr_buffer, candidate);
    };

    if (channel && channel->type == weechat::channel::chat_type::MUC)
    {
        // Remove the canonical nick and any legacy duplicate (empty label or
        // stale prefix group) left by the pre-role constructor nicklist_add.
        try_remove(muc_nicklist_name(channel).c_str());
        try_remove("");
        if (!profile.display_name.empty())
            try_remove(profile.display_name.c_str());
        return;
    }

    std::string nick_buf = jid(nullptr, this->id).bare;
    if (nick_buf.empty())
        nick_buf = this->id;
    try_remove(nick_buf.c_str());

    if (channel && channel->type == weechat::channel::chat_type::MUC)
    {
        int online = 0;
        int offline = 0;
        channel->count_nicklist_presence(online, offline);
        nicklist::refresh_separator(ptr_buffer, online > 0, offline > 0);
    }
    else if (!channel)
    {
        int online = 0;
        int offline = 0;
        account->count_roster_nicklist_presence(online, offline);
        nicklist::refresh_separator(ptr_buffer, online > 0, offline > 0);
    }
}

weechat::user::user(weechat::account *account, weechat::channel *channel,
                    std::string_view id, std::string_view display_name)
{
    if (!account || id.empty())
        throw std::invalid_argument("user: invalid account or empty id");

    if (account->users.empty() && channel)
        channel->add_nicklist_groups();

    weechat::user *ptr_user = user::search(account, id);
    if (ptr_user)
        throw std::invalid_argument("user: duplicate id in account");

    this->id = id;
    this->profile.display_name = display_name;

    // Try to load cached avatar if available
    weechat::avatar::load_for_user(*account, *this);

    // MUC nicklist updates are deferred to channel::add_member() once role and
    // affiliation are known. Non-MUC channel buffers still add here.
    if (channel && channel->type != weechat::channel::chat_type::MUC)
        nicklist_add(account, channel);
    // Note: Roster contacts added to nicklist when they come online in presence_handler
}
