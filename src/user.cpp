// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <stdint.h>
#include <string_view>
#include <strophe.h>
#include <weechat/weechat-plugin.h>
#include <fmt/core.h>

#include "plugin.hh"
#include "xmpp/node.hh"
#include "account.hh"
#include "user.hh"
#include "channel.hh"
#include "avatar.hh"

std::string weechat::user::get_colour()
{
    return weechat::user::get_colour(this->profile.display_name);
}

std::string weechat::user::get_colour(std::string_view name)
{
    auto result = weechat_info_get("nick_color", std::string(name).c_str());
    return result ? result : std::string{};
}

std::string weechat::user::get_colour_for_nicklist()
{
    return weechat::user::get_colour_for_nicklist(this->profile.display_name);
}

std::string weechat::user::get_colour_for_nicklist(std::string_view name)
{
    auto result = weechat_info_get("nick_color_name", std::string(name).c_str());
    return result ? result : std::string{};
}

std::string weechat::user::as_prefix_raw()
{
    std::string prefix;
    if (!this->profile.avatar_rendered.empty())
        prefix = this->profile.avatar_rendered + " ";
    return prefix + weechat::user::as_prefix_raw(this->profile.display_name);
}

std::string weechat::user::as_prefix_raw(std::string_view name)
{
    auto color_ptr = weechat_info_get("nick_color", std::string(name).c_str());
    auto reset_ptr = weechat_color("reset");
    std::string color = color_ptr ? color_ptr : std::string{};
    std::string reset = reset_ptr ? reset_ptr : std::string{};
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

    if (auto it = account->users.find(std::string(id)); it != account->users.end())
        return &it->second;

    return nullptr;
}

void weechat::user::nicklist_add(weechat::account *account,
                                 weechat::channel *channel)
{
    struct t_gui_nick_group *ptr_group;
    struct t_gui_buffer *ptr_buffer;
    const char *name = channel ? this->profile.display_name.c_str() : this->id.c_str();

    // For roster contacts (account buffer), strip resource from JID
    std::string bare_buf, resource_buf;
    if (!channel)
    {
        bare_buf = jid(nullptr, this->id).bare;
        if (!bare_buf.empty()) name = bare_buf.c_str();
    }

    {
        std::string name_bare = channel ? jid(nullptr, name).bare : std::string{};
        if (channel && !name_bare.empty() &&
            weechat_strcasecmp(name_bare.c_str(), channel->id.data()) == 0)
        {
            resource_buf = jid(nullptr, name).resource;
            if (!resource_buf.empty()) name = resource_buf.c_str();
        }
    }

    ptr_buffer = channel ? channel->buffer : account->buffer;

    const char *group = ".";
    if (this->profile.affiliation.has_value() && this->profile.affiliation.value() == "outcast")
        group = "!";
    if (this->profile.role.has_value() && this->profile.role.value() == "visitor")
        group = "?";
    if (this->profile.role.has_value() && this->profile.role.value() == "participant")
        group = "+";
    if (this->profile.affiliation.has_value() && this->profile.affiliation.value() == "member")
        group = "%";
    if (this->profile.role.has_value() && this->profile.role.value() == "moderator")
        group = "@";
    if (this->profile.affiliation.has_value() && this->profile.affiliation.value() == "admin")
        group = "&";
    if (this->profile.affiliation.has_value() && this->profile.affiliation.value() == "owner")
        group = "~";
    ptr_group = weechat_nicklist_search_group(ptr_buffer, nullptr, group);
    auto colour = get_colour_for_nicklist();
    weechat_nicklist_add_nick(ptr_buffer, ptr_group,
                              name,
                              this->is_away ?
                              "weechat.color.nicklist_away" :
                              colour.data(),
                              group,
                              "bar_fg",
                              1);
}

void weechat::user::nicklist_set_color(weechat::account *account,
                                       weechat::channel *channel)
{
    // Compute the same nick name as nicklist_add uses, then update in-place.
    const char *name = channel ? this->profile.display_name.c_str() : this->id.c_str();
    std::string bare_buf, resource_buf;
    if (!channel)
    {
        bare_buf = jid(nullptr, this->id).bare;
        if (!bare_buf.empty()) name = bare_buf.c_str();
    }
    {
        std::string name_bare = channel ? jid(nullptr, name).bare : std::string{};
        if (channel && !name_bare.empty() &&
            weechat_strcasecmp(name_bare.c_str(), channel->id.data()) == 0)
        {
            resource_buf = jid(nullptr, name).resource;
            if (!resource_buf.empty()) name = resource_buf.c_str();
        }
    }

    struct t_gui_buffer *ptr_buffer = channel ? channel->buffer : account->buffer;
    struct t_gui_nick *ptr_nick = weechat_nicklist_search_nick(ptr_buffer, nullptr, name);
    if (ptr_nick)
    {
        auto colour = get_colour_for_nicklist();
        const char *color = this->is_away
            ? "weechat.color.nicklist_away"
            : colour.data();
        weechat_nicklist_nick_set(ptr_buffer, ptr_nick, "color", color);
    }
}

void weechat::user::nicklist_remove(weechat::account *account,
                                    weechat::channel *channel)
{
    struct t_gui_nick *ptr_nick;
    struct t_gui_buffer *ptr_buffer;
    const char *name = this->profile.display_name.c_str();
    std::string resource_buf;
    {
        std::string name_bare = channel ? jid(nullptr, name).bare : std::string{};
        if (channel && !name_bare.empty() &&
            weechat_strcasecmp(name_bare.c_str(), channel->id.data()) == 0)
        {
            resource_buf = jid(nullptr, name).resource;
            if (!resource_buf.empty()) name = resource_buf.c_str();
        }
    }

    ptr_buffer = channel ? channel->buffer : account->buffer;

    if (name && (ptr_nick = weechat_nicklist_search_nick(ptr_buffer, nullptr, name)))
        weechat_nicklist_remove_nick(ptr_buffer, ptr_nick);
}

weechat::user::user(weechat::account *account, weechat::channel *channel,
                    std::string_view id, std::string_view display_name)
{
    if (!account || id.empty())
        throw nullptr;

    if (account->users.empty() && channel)
        channel->add_nicklist_groups();

    weechat::user *ptr_user = user::search(account, id);
    if (ptr_user)
        throw nullptr;

    this->id = id;
    this->profile.display_name = display_name;

    // Try to load cached avatar if available
    weechat::avatar::load_for_user(*account, *this);

    // Add to nicklist:
    // - For MUC users: add to channel nicklist
    // - For roster contacts: add to account buffer nicklist (shown when online)
    if (channel)
        nicklist_add(account, channel);
    // Note: Roster contacts added to nicklist when they come online in presence_handler
}
