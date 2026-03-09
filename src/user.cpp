// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <stdint.h>
#include <stdio.h>
#include <strophe.h>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "account.hh"
#include "user.hh"
#include "channel.hh"
#include "color.hh"
#include "avatar.hh"

std::string weechat::user::get_colour()
{
    return weechat::user::get_colour(this->profile.display_name.c_str());
}

std::string weechat::user::get_colour(const char *name)
{
    // XEP-0392: Consistent Color Generation
    std::string color_code = weechat::consistent_color(name);
    if (!color_code.empty())
        return weechat_color(color_code.c_str());
    
    // Fallback to WeeChat's built-in color if generation fails
    return weechat_info_get("nick_color", name);
}

std::string weechat::user::get_colour_for_nicklist()
{
    return weechat::user::get_colour_for_nicklist(this->profile.display_name.c_str());
}

std::string weechat::user::get_colour_for_nicklist(const char *name)
{
    // XEP-0392: Consistent Color Generation (return color name for nicklist)
    std::string color_code = weechat::consistent_color(name);
    if (!color_code.empty())
        return color_code;
    
    // Fallback to WeeChat's built-in color if generation fails
    return weechat_info_get("nick_color_name", name);
}

std::string weechat::user::as_prefix_raw()
{
    std::string avatar_prefix = "";
    
    // Add avatar if available and rendered
    if (!this->profile.avatar_rendered.empty())
    {
        avatar_prefix = this->profile.avatar_rendered + " ";
    }
    
    return avatar_prefix + weechat::user::as_prefix_raw(this->profile.display_name.c_str());
}

std::string weechat::user::as_prefix_raw(const char *name)
{
    static char result[2048];

    snprintf(result, sizeof(result), "%s%s%s",
             weechat_info_get("nick_color", name),
             name, weechat_color("reset"));

    return result;
}

std::string weechat::user::as_prefix()
{
    std::string avatar_prefix = "";
    
    // Add avatar if available and rendered
    if (!this->profile.avatar_rendered.empty())
    {
        avatar_prefix = this->profile.avatar_rendered + " ";
    }
    
    return avatar_prefix + weechat::user::as_prefix(this->profile.display_name.c_str());
}

std::string weechat::user::as_prefix(const char *name)
{
    static char result[2048];

    snprintf(result, sizeof(result), "%s%s\t",
             weechat::user::get_colour(name).data(), name);

    return result;
}

weechat::user *weechat::user::bot_search(weechat::account *account,
                                         const char *pgp_id)
{
    if (!account || !pgp_id)
        return nullptr;

    for (auto& ptr_user : account->users)
    {
        if (ptr_user.second.profile.pgp_id.has_value() &&
            ptr_user.second.profile.pgp_id.value() == pgp_id)
            return &ptr_user.second;
    }

    return nullptr;
}

weechat::user *weechat::user::search(weechat::account *account,
                                     const char *id)
{
    if (!account || !id)
        return nullptr;

    if (auto user = account->users.find(id); user != account->users.end())
        return &user->second;

    return nullptr;
}

void weechat::user::nicklist_add(weechat::account *account,
                                 weechat::channel *channel)
{
    struct t_gui_nick_group *ptr_group;
    struct t_gui_buffer *ptr_buffer;
    const char *name = channel ? this->profile.display_name.c_str() : this->id.c_str();
    
    // For roster contacts (account buffer), strip resource from JID
    if (!channel)
    {
        const char *bare = xmpp_jid_bare(account->context, this->id.c_str());
        if (bare)
            name = bare;
    }
    
    if (channel && weechat_strcasecmp(xmpp_jid_bare(account->context, name),
                                      channel->id.data()) == 0)
        name = xmpp_jid_resource(account->context, name);

    ptr_buffer = channel ? channel->buffer : account->buffer;

    const char *group = "...";
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
    if (channel && weechat_strcasecmp(xmpp_jid_bare(account->context, name),
                                      channel->id.data()) == 0)
        name = xmpp_jid_resource(account->context, name);

    ptr_buffer = channel ? channel->buffer : account->buffer;

    if (name && (ptr_nick = weechat_nicklist_search_nick(ptr_buffer, nullptr, name)))
        weechat_nicklist_remove_nick(ptr_buffer, ptr_nick);
}

weechat::user::user(weechat::account *account, weechat::channel *channel,
                    const char *id, const char *display_name)
{
    if (!account || !id)
    {
        throw nullptr;
    }

    if (account->users.empty() && channel)
        channel->add_nicklist_groups();

    weechat::user *ptr_user = user::search(account, id);
    if (ptr_user)
    {
        throw nullptr;
    }

    this->id = id;

    this->profile.display_name = display_name ? display_name : "";

    // Try to load cached avatar if available
    weechat::avatar::load_for_user(*account, *this);

    // Add to nicklist:
    // - For MUC users: add to channel nicklist
    // - For roster contacts: add to account buffer nicklist (will be shown when online)
    if (channel)
        nicklist_add(account, channel);
    // Note: Roster contacts added to nicklist when they come online in presence_handler
}
