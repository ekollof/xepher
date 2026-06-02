// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <stdint.h>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <strophe.h>
#include <weechat/weechat-plugin.h>
#include <fmt/core.h>

#include "plugin.hh"
#include "color.hh"
#include "xmpp/node.hh"
#include "account.hh"
#include "user.hh"
#include "channel.hh"
#include "avatar.hh"

namespace {

unsigned long nick_hash(std::string_view name)
{
    unsigned long hash = 5381;
    for (char c : name)
        hash = ((hash << 5) + hash) + static_cast<unsigned char>(c);
    return hash;
}

std::string compute_nick_color(std::string_view name)
{
    const char *colors_str = weechat_config_string(
        weechat_config_get("weechat.color.chat_nick_colors"));
    if (!colors_str || !colors_str[0])
        return {};

    std::vector<std::string> palette;
    std::string token;
    for (const char *p = colors_str; *p; ++p)
    {
        if (*p == ',')
        {
            if (!token.empty()) palette.push_back(std::move(token));
            token.clear();
        }
        else
            token += *p;
    }
    if (!token.empty()) palette.push_back(std::move(token));

    if (palette.empty()) return {};
    return palette[nick_hash(name) % palette.size()];
}

}

std::string weechat::user::get_colour()
{
    if (cached_nick_color.empty() && !this->profile.display_name.empty())
    {
        std::string raw_color = compute_nick_color(this->profile.display_name);
        cached_nick_color = weechat::xmpp_color(raw_color);
    }
    return cached_nick_color;
}

std::string weechat::user::get_colour(std::string_view name)
{
    return std::string(weechat::xmpp_color(compute_nick_color(name)));
}

std::string weechat::user::get_colour_for_nicklist()
{
    if (cached_nick_color_name.empty() && !this->profile.display_name.empty())
        cached_nick_color_name = compute_nick_color(this->profile.display_name);
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
        std::string reset = weechat::xmpp_color("reset");
        cached_prefix_raw = fmt::format("{}{}{}{}", color, prefix, this->profile.display_name, reset);
    }
    return cached_prefix_raw;
}

std::string weechat::user::as_prefix_raw(std::string_view name)
{
    std::string color = get_colour(name);
    std::string reset = weechat::xmpp_color("reset");
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

    if (auto it = account->users.find(std::string(id)); it != account->users.end()) {
        auto& [_, u] = *it;
        return &u;
    }

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
                              (colour.empty() ? nullptr : colour.c_str()),
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
            : (colour.empty() ? nullptr : colour.c_str());
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

    // Add to nicklist:
    // - For MUC users: add to channel nicklist
    // - For roster contacts: add to account buffer nicklist (shown when online)
    if (channel)
        nicklist_add(account, channel);
    // Note: Roster contacts added to nicklist when they come online in presence_handler
}
