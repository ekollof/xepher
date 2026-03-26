// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <stdint.h>
#include <string_view>
#include <strophe.h>
#include <weechat/weechat-plugin.h>
#include <fmt/core.h>

#include "plugin.hh"
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
    return weechat_info_get("nick_color", std::string(name).c_str());
}

std::string weechat::user::get_colour_for_nicklist()
{
    return weechat::user::get_colour_for_nicklist(this->profile.display_name);
}

std::string weechat::user::get_colour_for_nicklist(std::string_view name)
{
    return weechat_info_get("nick_color_name", std::string(name).c_str());
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
    return fmt::format("{}{}{}",
                       weechat_info_get("nick_color", std::string(name).c_str()),
                       name,
                       weechat_color("reset"));
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
        xmpp_string_guard bare_g(account->context,
            xmpp_jid_bare(account->context, this->id.c_str()));
        if (bare_g) { bare_buf = bare_g.str(); name = bare_buf.c_str(); }
    }

    {
        xmpp_string_guard jid_bare_g(account->context,
            channel ? xmpp_jid_bare(account->context, name) : nullptr);
        if (channel && jid_bare_g &&
            weechat_strcasecmp(jid_bare_g.c_str(), channel->id.data()) == 0)
        {
            xmpp_string_guard resource_g(account->context,
                xmpp_jid_resource(account->context, name));
            if (resource_g) { resource_buf = resource_g.str(); name = resource_buf.c_str(); }
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
    weechat_nicklist_add_nick(ptr_buffer, ptr_group,
                              name,
                              this->is_away ?
                              "weechat.color.nicklist_away" :
                              get_colour_for_nicklist().data(),
                              group,
                              "bar_fg",
                              1);
}

void weechat::user::nicklist_remove(weechat::account *account,
                                    weechat::channel *channel)
{
    struct t_gui_nick *ptr_nick;
    struct t_gui_buffer *ptr_buffer;
    const char *name = this->profile.display_name.c_str();
    std::string resource_buf;
    {
        xmpp_string_guard bare_g(account->context,
            channel ? xmpp_jid_bare(account->context, name) : nullptr);
        if (channel && bare_g &&
            weechat_strcasecmp(bare_g.c_str(), channel->id.data()) == 0)
        {
            xmpp_string_guard resource_g(account->context,
                xmpp_jid_resource(account->context, name));
            if (resource_g) { resource_buf = resource_g.str(); name = resource_buf.c_str(); }
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
