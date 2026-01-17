// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <regex>
#include <fmt/core.h>
#include <optional>
#include <strophe.h>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "account.hh"
#include "omemo.hh"
#include "user.hh"
#include "channel.hh"
#include "input.hh"
#include "buffer.hh"
#include "pgp.hh"
#include "util.hh"
#include "xmpp/node.hh"

void weechat::channel::set_transport(enum weechat::channel::transport transport, int force)
{
    if (force)
        switch (transport)
        {
            case weechat::channel::transport::PLAIN:
                omemo.enabled = 0;
                pgp.enabled = 0;
                break;
            case weechat::channel::transport::OMEMO:
                omemo.enabled = 1;
                pgp.enabled = 0;
                break;
            case weechat::channel::transport::PGP:
                omemo.enabled = 0;
                pgp.enabled = 1;
                break;
            default:
                break;
        }

    if (this->transport != transport)
    {
        this->transport = transport;
        weechat_printf_date_tags(buffer, 0, NULL, "%s%sTransport: %s",
                                 weechat_prefix("network"), weechat_color("gray"),
                                 weechat::channel::transport_name(this->transport));
    }
}

struct t_gui_buffer *weechat::channel::search_buffer(weechat::channel::chat_type type,
                                                     const char *name)
{
    struct t_hdata *hdata_buffer;
    struct t_gui_buffer *ptr_buffer;
    const char *ptr_type, *ptr_account_name, *ptr_remote_jid;

    hdata_buffer = weechat_hdata_get("buffer");
    ptr_buffer = (struct t_gui_buffer*)weechat_hdata_get_list(hdata_buffer, "gui_buffers");

    while (ptr_buffer)
    {
        if (weechat_buffer_get_pointer(ptr_buffer, "plugin") == weechat_plugin)
        {
            ptr_type = weechat_buffer_get_string(ptr_buffer, "localvar_type");
            ptr_account_name = weechat_buffer_get_string(ptr_buffer,
                                                           "localvar_account");
            ptr_remote_jid = weechat_buffer_get_string(ptr_buffer,
                                                         "localvar_remote_jid");
            if (ptr_type && ptr_type[0]
                && ptr_account_name && ptr_account_name[0]
                && ptr_remote_jid && ptr_remote_jid[0]
                && (   ((  (type == weechat::channel::chat_type::MUC))
                        && (strcmp(ptr_type, "room") == 0))
                    || ((  (type == weechat::channel::chat_type::PM))
                        && (strcmp(ptr_type, "private") == 0)))
                && (ptr_account_name == account.name)
                && (weechat_strcasecmp(ptr_remote_jid, name) == 0))
            {
                return ptr_buffer;
            }
        }
        ptr_buffer = (struct t_gui_buffer*)weechat_hdata_move(hdata_buffer, ptr_buffer, 1);
    }

    return NULL;
}

struct t_gui_buffer *weechat::channel::create_buffer(weechat::channel::chat_type type,
                                                     const char *name)
{
    struct t_gui_buffer *ptr_buffer;
    int buffer_created;
    const char *short_name = NULL, *localvar_remote_jid = NULL;

    buffer_created = 0;

    std::string buffer_name = fmt::format("{}.{}", account.name, name);

    ptr_buffer = weechat::channel::search_buffer(type, name);
    if (ptr_buffer)
    {
        weechat_nicklist_remove_all(ptr_buffer);
    }
    else
    {
        ptr_buffer = weechat_buffer_new(buffer_name.data(),
                                        &input__data_cb, NULL, NULL,
                                        &buffer__close_cb, NULL, NULL);
        if (!ptr_buffer)
            return NULL;

        buffer_created = 1;
    }

    if (buffer_created)
    {
        char *res = (char*)strrchr(name, '/');
        if (!weechat_buffer_get_integer(ptr_buffer, "short_name_is_set"))
            weechat_buffer_set(ptr_buffer, "short_name",
                               res ? res + 1 : name);
    }
    else
    {
        short_name = weechat_buffer_get_string(ptr_buffer, "short_name");
        localvar_remote_jid = weechat_buffer_get_string(ptr_buffer,
                                                     "localvar_remote_jid");

        if (!short_name ||
            (localvar_remote_jid && (strcmp(localvar_remote_jid, short_name) == 0)))
        {
            weechat_buffer_set(ptr_buffer, "short_name",
                               xmpp_jid_node(account.context, name));
        }
    }
    if(!(account.nickname().size()))
        account.nickname(xmpp_jid_node(account.context, account.jid().data()));

    // Set notify level for buffer: "0" = never add to hotlist
    //                              "1" = add for highlights only
    //                              "2" = add for highlights and messages
    //                              "3" = add for all messages.
    weechat_buffer_set(ptr_buffer, "notify",
                       (type == weechat::channel::chat_type::PM) ? "3" : "2");
    weechat_buffer_set(ptr_buffer, "localvar_set_type",
                       (type == weechat::channel::chat_type::PM) ? "private" : "channel");
    weechat_buffer_set(ptr_buffer, "localvar_set_nick",
                       account.nickname().data());
    weechat_buffer_set(ptr_buffer, "localvar_set_account", account.name.data());
    weechat_buffer_set(ptr_buffer, "localvar_set_remote_jid", name);
    weechat_buffer_set(ptr_buffer, "input_multiline", "1");

    if (buffer_created)
    {
        (void) weechat_hook_signal_send("logger_backlog",
                                        WEECHAT_HOOK_SIGNAL_POINTER,
                                        ptr_buffer);
        weechat_buffer_set(ptr_buffer, "input_get_unknown_commands", "1");
        if (type != weechat::channel::chat_type::PM)
        {
            weechat_buffer_set(ptr_buffer, "nicklist", "1");
            weechat_buffer_set(ptr_buffer, "nicklist_display_groups", "0");
            weechat_buffer_set_pointer(ptr_buffer, "nicklist_callback",
                                       (void*)&buffer__nickcmp_cb);
            weechat_buffer_set_pointer(ptr_buffer, "nicklist_callback_pointer",
                                       &account);
        }

        weechat_buffer_set(ptr_buffer, "highlight_words_add",
                           account.nickname().data());
        weechat_buffer_set(ptr_buffer, "highlight_tags_restrict",
                           "message");
    }

    return ptr_buffer;
}

void weechat::channel::add_nicklist_groups()
{
    if (type == weechat::channel::chat_type::PM)
        return;

    weechat_nicklist_add_group(buffer, NULL, fmt::format("%03d|%s", 000, "~").data(),
                               "weechat.color.nicklist_group", 1);
    weechat_nicklist_add_group(buffer, NULL, fmt::format("%03d|%s", 001, "&").data(),
                               "weechat.color.nicklist_group", 1);
    weechat_nicklist_add_group(buffer, NULL, fmt::format("%03d|%s", 002, "@").data(),
                               "weechat.color.nicklist_group", 1);
    weechat_nicklist_add_group(buffer, NULL, fmt::format("%03d|%s", 003, "%").data(),
                               "weechat.color.nicklist_group", 1);
    weechat_nicklist_add_group(buffer, NULL, fmt::format("%03d|%s", 004, "+").data(),
                               "weechat.color.nicklist_group", 1);
    weechat_nicklist_add_group(buffer, NULL, fmt::format("%03d|%s", 005, "?").data(),
                               "weechat.color.nicklist_group", 1);
    weechat_nicklist_add_group(buffer, NULL, fmt::format("%03d|%s", 006, "!").data(),
                               "weechat.color.nicklist_group", 1);
    weechat_nicklist_add_group(buffer, NULL, fmt::format("%03d|%s", 999, "...").data(),
                               "weechat.color.nicklist_group", 1);
}

weechat::channel::channel(weechat::account& account,
                          weechat::channel::chat_type type,
                          const char *id, const char *name) : id(id), name(name), type(type), account(account)
{
    if (!id || !name || !name[0])
        throw std::invalid_argument("channel()");

    //if (weechat::channel::search(&account, id))
    //    throw std::invalid_argument("duplicate");

    buffer = weechat::channel::create_buffer(type, name);
    if (!buffer)
        throw std::invalid_argument("buffer fail");
    else if (type == weechat::channel::chat_type::PM)
    {
        auto muc_channel = account.channels.find(jid(account.context,
                                                                               id).bare.data());
        if (muc_channel != account.channels.end())
        {
            weechat_buffer_merge(buffer, muc_channel->second.buffer);
        }
    }

    typing_hook_timer = weechat_hook_timer(1 * 1000, 0, 0,
                                           &weechat::channel::typing_cb,
                                           this, nullptr);

    self_typing_hook_timer = weechat_hook_timer(1 * 1000, 0, 0,
                                                &weechat::channel::self_typing_cb,
                                                this, nullptr);

    omemo.enabled = 0;
    omemo.devicelist_requests = weechat_hashtable_new(64,
            WEECHAT_HASHTABLE_STRING, WEECHAT_HASHTABLE_POINTER, nullptr, nullptr);
    omemo.bundle_requests = weechat_hashtable_new(64,
            WEECHAT_HASHTABLE_STRING, WEECHAT_HASHTABLE_POINTER, nullptr, nullptr);

    add_nicklist_groups();

    if (type != weechat::channel::chat_type::MUC)
    {
        time_t now = time(NULL);
        time_t start;
        
        weechat_printf(buffer, "%s[DEBUG] PM channel created, checking MAM...",
                      weechat_prefix("network"));
        
        // Load last fetch timestamp from cache
        if (last_mam_fetch == 0)
        {
            last_mam_fetch = account.mam_cache_get_last_timestamp(id);
            weechat_printf(buffer, "%s[DEBUG] Last MAM fetch from cache: %ld",
                          weechat_prefix("network"), last_mam_fetch);
        }
        
        // Skip MAM entirely if channel was deliberately closed (timestamp == -1)
        if (last_mam_fetch == -1)
        {
            // User closed this channel, don't auto-fetch history
            weechat_printf(buffer, "%s[DEBUG] Channel was closed, skipping MAM",
                          weechat_prefix("network"));
            return;
        }
        
        // Load and display cached messages
        if (last_mam_fetch > 0)
        {
            weechat_printf(buffer, "%s[DEBUG] Loading cached MAM messages...",
                          weechat_prefix("network"));
            account.mam_cache_load_messages(id, buffer);
        }
        
        // If we've fetched recently, only get new messages since last fetch
        if (last_mam_fetch > 0 && (now - last_mam_fetch) < 300)  // Less than 5 minutes
        {
            // Fetch only messages since last fetch
            start = last_mam_fetch;
            weechat_printf(buffer, "%sFetching new MAM messages since last check (%ld seconds ago)...",
                          weechat_prefix("network"),
                          now - last_mam_fetch);
        }
        else
        {
            // Fetch last 7 days
            start = now - (7 * 86400);
            if (last_mam_fetch > 0)
            {
                weechat_printf(buffer, "%sFetching MAM messages (last check was %ld seconds ago, fetching 7 days)...",
                              weechat_prefix("network"),
                              now - last_mam_fetch);
            }
            else
            {
                weechat_printf(buffer, "%sFetching MAM messages (7 days)...",
                              weechat_prefix("network"));
            }
        }
        
        time_t end = now;
        fetch_mam(xmpp_uuid_gen(account.context), &start, &end, nullptr);
    }
}

void weechat::channel::member_speaking_add_to_list(const char *nick, int highlight)
{
    int size, to_remove, i;
    struct t_weelist_item *ptr_item;

    /* create list if it does not exist */
    if (!members_speaking[highlight])
        members_speaking[highlight] = weechat_list_new();

    /* remove item if it was already in list */
    ptr_item = weechat_list_casesearch(members_speaking[highlight], nick);
    if (ptr_item)
        weechat_list_remove(members_speaking[highlight], ptr_item);

    /* add nick in list */
    weechat_list_add(members_speaking[highlight], nick,
                     WEECHAT_LIST_POS_END, NULL);

    /* reduce list size if it's too big */
    size = weechat_list_size(members_speaking[highlight]);
    if (size > CHANNEL_MEMBERS_SPEAKING_LIMIT)
    {
        to_remove = size - CHANNEL_MEMBERS_SPEAKING_LIMIT;
        for (i = 0; i < to_remove; i++)
        {
            weechat_list_remove(
                members_speaking[highlight],
                weechat_list_get(members_speaking[highlight], 0));
        }
    }
}

void weechat::channel::member_speaking_add(const char *nick, int highlight)
{
    if (highlight < 0)
        highlight = 0;
    if (highlight > 1)
        highlight = 1;
    if (highlight)
        weechat::channel::member_speaking_add_to_list(nick, 1);

    weechat::channel::member_speaking_add_to_list(nick, 0);
}

void weechat::channel::member_speaking_rename(const char *old_nick, const char *new_nick)
{
    struct t_weelist_item *ptr_item;
    int i;

    for (i = 0; i < 2; i++)
    {
        if (members_speaking[i])
        {
            ptr_item = weechat_list_search(members_speaking[i], old_nick);
            if (ptr_item)
                weechat_list_set(ptr_item, new_nick);
        }
    }
}

void weechat::channel::member_speaking_rename_if_present(const char *nick)
{
    struct t_weelist_item *ptr_item;
    int i, j, list_size;

    for (i = 0; i < 2; i++)
    {
        if (members_speaking[i])
        {
            list_size = weechat_list_size(members_speaking[i]);
            for (j = 0; j < list_size; j++)
            {
                ptr_item = weechat_list_get(members_speaking[i], j);
                if (ptr_item && (weechat_strcasecmp(weechat_list_string(ptr_item),
                                                    nick) == 0))
                    weechat_list_set(ptr_item, nick);
            }
        }
    }
}

int weechat::channel::typing_cb(const void *pointer, void *data, int remaining_calls)
{
    weechat::channel *channel;
    const char *localvar;
    unsigned typecount;
    time_t now;

    (void) data;
    (void) remaining_calls;

    if (!pointer)
        return WEECHAT_RC_ERROR;

    channel = (weechat::channel *)pointer;
    
    // Safety check: don't access channel if hook was already cleared
    if (!channel->typing_hook_timer)
        return WEECHAT_RC_OK;

    now = time(NULL);

    typecount = 0;

    for (auto ptr_typing = channel->typings.begin();
         ptr_typing != channel->typings.end();)
    {
        if (now - ptr_typing->ts > 5)
        {
            ptr_typing = channel->typings.erase(ptr_typing);
        }
        else
            ptr_typing++;

        typecount++;
    }

    localvar = weechat_buffer_get_string(channel->buffer, "localvar_typing");
    if (!localvar || strncmp(localvar, typecount > 0 ? "1" : "0", 1) != 0)
        weechat_buffer_set(channel->buffer, "localvar_set_typing",
                           typecount > 0 ? "1" : "0");
    weechat_bar_item_update("typing");

    return WEECHAT_RC_OK;
}

std::optional<weechat::channel::typing*> weechat::channel::typing_search(weechat::user *user)
{
    if (!user)
        return std::nullopt;

    for (auto& ptr_typing : typings)
    {
        if (user == ptr_typing.user)
            return &ptr_typing;
    }

    return std::nullopt;
}

int weechat::channel::remove_typing(weechat::user *user)
{
    if (!user)
        return 0;
    
    for (auto ptr_typing = typings.begin(); ptr_typing != typings.end(); ptr_typing++)
    {
        if (user == ptr_typing->user)
        {
            typings.erase(ptr_typing);
            typing_cb(this, nullptr, 0);  // Update bar
            return 1;
        }
    }
    
    return 0;
}

int weechat::channel::add_typing(weechat::user *user)
{
    weechat::channel::typing *the_typing = nullptr;
    int ret = 0;

    auto typing_opt = weechat::channel::typing_search(user);
    if (!typing_opt)
    {
        weechat::channel::typing& new_typing = typings.emplace_back();
        new_typing.user = user;
        // For MUC, show nickname (resource); for PM, show bare JID
        if (user->id)
        {
            if (type == chat_type::MUC)
            {
                char *nick = xmpp_jid_resource(account.context, user->id);
                new_typing.name = nick ? nick : user->id;
                if (nick)
                    xmpp_free(account.context, nick);
            }
            else
            {
                char *bare_jid = xmpp_jid_bare(account.context, user->id);
                new_typing.name = bare_jid ? bare_jid : user->id;
                if (bare_jid)
                    xmpp_free(account.context, bare_jid);
            }
        }
        else
        {
            new_typing.name = "";
        }

        the_typing = &new_typing;
        ret = 1;
    }
    else
    {
        the_typing = *typing_opt;
    }
    the_typing->ts = time(nullptr);

    weechat::channel::typing_cb(this, nullptr, 0);

    return ret;
}

int weechat::channel::self_typing_cb(const void *pointer, void *data, int remaining_calls)
{
    time_t now;

    (void) data;
    (void) remaining_calls;

    if (!pointer)
        return WEECHAT_RC_ERROR;

    weechat::channel *channel = (weechat::channel *)pointer;

    now = time(NULL);

    for (auto ptr_typing = channel->self_typings.begin();
         ptr_typing != channel->self_typings.end();)
    {
        if (now - ptr_typing->ts > 10)
        {
            channel->send_paused(ptr_typing->user);
            ptr_typing = channel->self_typings.erase(ptr_typing);
        }
        else
            ptr_typing++;
    }

    return WEECHAT_RC_OK;
}

std::optional<weechat::channel::typing*> weechat::channel::self_typing_search(weechat::user *user)
{
    for (auto& ptr_typing : self_typings)
    {
        if (user == ptr_typing.user)
            return &ptr_typing;
    }

    return std::nullopt;
}

int weechat::channel::add_self_typing(weechat::user *user)
{
    weechat::channel::typing *the_typing = nullptr;
    int ret = 0;

    auto typing_opt = weechat::channel::self_typing_search(user);
    if (!typing_opt)
    {
        weechat::channel::typing& new_typing = self_typings.emplace_back();
        new_typing.user = user;
        // Extract bare JID (without resource) for display
        if (user && user->id)
        {
            char *bare_jid = xmpp_jid_bare(account.context, user->id);
            new_typing.name = bare_jid ? bare_jid : user->id;
            if (bare_jid)
                xmpp_free(account.context, bare_jid);
        }
        else
        {
            new_typing.name = "";
        }

        the_typing = &new_typing;
        ret = 1;
    }
    else
    {
        the_typing = *typing_opt;
    }

    (void)the_typing;  // Used for side effects, suppress warning
    self_typing_cb(this, nullptr, 0);

    return ret;
}

weechat::channel::~channel()
{
    // Safety check: if plugin is destroyed, don't call weechat functions
    if (!weechat::plugin::instance || !weechat::plugin::instance->ptr())
        return;
        
    // Unhook timers to prevent callbacks firing after channel is destroyed
    if (typing_hook_timer)
    {
        weechat_unhook(typing_hook_timer);
        typing_hook_timer = nullptr;
    }
    if (self_typing_hook_timer)
    {
        weechat_unhook(self_typing_hook_timer);
        self_typing_hook_timer = nullptr;
    }
    
    // Clear MAM cache for PM channels to prevent auto-recreation on reconnect
    if (type == chat_type::PM)
    {
        account.mam_cache_clear_messages(id);
        account.mam_cache_set_last_timestamp(id, -1);  // -1 = deliberately closed
    }
    
    // NOTE: Other cleanup disabled - weechat frees these when closing buffers
    // Attempting to free them here causes memory corruption:
    // - "Unaligned fastbin chunk detected" from freeing members_speaking lists
    // - Double-free from freeing omemo hashtables
    
    /*
    if (members_speaking[0])
        weechat_list_free(members_speaking[0]);
    if (members_speaking[1])
        weechat_list_free(members_speaking[1]);
    */
    
    /*
    // Free topic
    if (topic.value)
        ::free(topic.value);
    if (topic.creator)
        ::free(topic.creator);

    // Free creator
    if (creator)
        ::free(creator);

    // Free unreads
    for (auto& unread : unreads)
    {
        if (unread.id)
            ::free(unread.id);
        if (unread.thread)
            ::free(unread.thread);
    }

    // Free members
    for (auto& member_pair : members)
    {
        auto& member = member_pair.second;
        if (member.id)
            ::free(member.id);
        if (member.role)
            ::free(member.role);
        if (member.affiliation)
            ::free(member.affiliation);
    }
    */
}

void weechat::channel::update_topic(const char* topic, const char* creator, int last_set)
{
    if (this->topic.value)
        ::free(this->topic.value);
    if (this->topic.creator)
        ::free(this->topic.creator);
    this->topic.value = (topic) ? strdup(topic) : NULL;
    this->topic.creator = (creator) ? strdup(creator) : NULL;
    this->topic.last_set = last_set;

    if (this->topic.value)
        weechat_buffer_set(buffer, "title", topic);
    else
        weechat_buffer_set(buffer, "title", "");
}

void weechat::channel::update_name(const char* name)
{
    if (name)
        weechat_buffer_set(buffer, "short_name", name);
    else
        weechat_buffer_set(buffer, "short_name", "");
}

std::optional<weechat::channel::member*> weechat::channel::add_member(const char *id, const char *client)
{
    weechat::channel::member *member;
    weechat::user *user;

    user = user::search(&account, id);

    if (this->id == id && type == weechat::channel::chat_type::MUC)
    {
        weechat_printf_date_tags(buffer, 0, "log2", "%sMUC: %s",
                                 weechat_prefix("network"), id);
        return std::nullopt;
    }

    auto member_opt = member_search(id);
    if (!member_opt)
    {
        member = new weechat::channel::member();
        member->id = strdup(id);

        member->role = NULL;
        member->affiliation = NULL;
    }
    else
    {
        member = *member_opt;
        if (user)
            user->nicklist_remove(&account, this);
    }

    if (user)
        user->nicklist_add(&account, this);
    else return member; // TODO: !!

    char *jid_bare = xmpp_jid_bare(account.context, user->id);
    char *jid_resource = xmpp_jid_resource(account.context, user->id);
    if (weechat_strcasecmp(jid_bare, id) == 0
             && type == weechat::channel::chat_type::MUC)
        weechat_printf_date_tags(buffer, 0, "xmpp_presence,enter,log4", "%s%s%s%s%s %s%s%s%s %s%s%s%s%s%s%s%s%s%s%s%s%s%s",
                                 weechat_prefix("join"),
                                 user->as_prefix_raw().data(),
                                 client ? " (" : "",
                                 client ? client : "",
                                 client ? ")" : "",
                                 user->profile.status ? "is " : "",
                                 weechat_color("irc.color.message_join"),
                                 user->profile.status ? user->profile.status : (user->profile.idle ? "idle" : "entered"),
                                 weechat_color("reset"),
                                 id,
                                 user->profile.status_text ? " [" : "",
                                 user->profile.status_text ? user->profile.status_text : "",
                                 user->profile.status_text ? "]" : "",
                                 weechat_color("yellow"), " as ", weechat_color("reset"),
                                 user->profile.affiliation ? user->profile.affiliation : "",
                                 user->profile.affiliation ? " " : "",
                                 user->profile.role,
                                 user->profile.pgp_id ? weechat_color("gray") : "",
                                 user->profile.pgp_id ? " with PGP:" : "",
                                 user->profile.pgp_id ? user->profile.pgp_id : "",
                                 user->profile.pgp_id ? weechat_color("reset") : "");
    else
        weechat_printf_date_tags(buffer, 0, "xmpp_presence,enter,log4", "%s%s (%s) %s%s%s%s %s%s%s%s%s%s%s%s%s",
                                 weechat_prefix("join"),
                                 jid_resource ? user->as_prefix_raw().data() : "You",
                                 jid_resource ? jid_resource : user->as_prefix_raw().data(),
                                 user->profile.status ? "is " : "",
                                 weechat_color("irc.color.message_join"),
                                 user->profile.status ? user->profile.status : (user->profile.idle ? "idle" : "entered"),
                                 weechat_color("reset"),
                                 user->profile.idle ? "since " : "",
                                 user->profile.idle ? user->profile.idle->data() : "",
                                 user->profile.status_text ? " [" : "",
                                 user->profile.status_text ? user->profile.status_text : "",
                                 user->profile.status_text ? "]" : "",
                                 user->profile.pgp_id || user->profile.omemo ? weechat_color("gray") : "",
                                 user->profile.pgp_id || user->profile.omemo ? " with " : "",
                                 user->profile.pgp_id ? "PGP:" : "",
                                 user->profile.pgp_id ? user->profile.pgp_id : "",
                                 user->profile.omemo && user->profile.pgp_id ? " and " : "",
                                 user->profile.omemo ? "OMEMO" : "",
                                 user->profile.pgp_id || user->profile.omemo ? weechat_color("reset") : "");

    return member;
}

std::optional<weechat::channel::member*> weechat::channel::member_search(const char *id)
{
    if (!id)
        return std::nullopt;

    for (auto& ptr_member : members)
    {
        if (weechat_strcasecmp(ptr_member.second.id, id) == 0)
            return &ptr_member.second;
    }

    return std::nullopt;
}

std::optional<weechat::channel::member*> weechat::channel::remove_member(const char *id, const char *reason)
{
    weechat::user *user;

    user = user::search(&account, id);
    if (user)
        user->nicklist_remove(&account, this);
    else return std::nullopt;

    auto member_opt = member_search(id);

    char *jid_bare = xmpp_jid_bare(account.context, user->id);
    char *jid_resource = xmpp_jid_resource(account.context, user->id);
    if (weechat_strcasecmp(jid_bare, id) == 0
        && type == weechat::channel::chat_type::MUC)
        weechat_printf_date_tags(buffer, 0, "xmpp_presence,leave,log4",
                                 "%s%s %sleft%s %s %s%s%s",
                                 weechat_prefix("quit"),
                                 jid_resource,
                                 weechat_color("irc.color.message_quit"),
                                 weechat_color("reset"),
                                 id,
                                 reason ? "[" : "",
                                 reason ? reason : "",
                                 reason ? "]" : "");
    else
        weechat_printf_date_tags(buffer, 0, "xmpp_presence,leave,log4",
                                 "%s%s (%s) %sleft%s %s %s%s%s",
                                 weechat_prefix("quit"),
                                 xmpp_jid_bare(account.context, user->id),
                                 xmpp_jid_resource(account.context, user->id),
                                 weechat_color("irc.color.message_quit"),
                                 weechat_color("reset"),
                                 id,
                                 reason ? "[" : "",
                                 reason ? reason : "",
                                 reason ? "]" : "");

    return member_opt;
}

int weechat::channel::send_message(std::string to, std::string body,
                                   std::optional<std::string> oob,
                                   std::optional<file_metadata> file_meta)
{
    xmpp_stanza_t *message = xmpp_message_new(account.context,
                    type == weechat::channel::chat_type::MUC
                    ? "groupchat" : "chat",
                    to.data(), NULL);

    char *id = xmpp_uuid_gen(account.context);
    xmpp_stanza_set_id(message, id);
    
    // XEP-0359: Add origin-id for stable message identification
    xmpp_stanza_t *origin_id = xmpp_stanza_new(account.context);
    xmpp_stanza_set_name(origin_id, "origin-id");
    xmpp_stanza_set_ns(origin_id, "urn:xmpp:sid:0");
    xmpp_stanza_set_attribute(origin_id, "id", id);
    xmpp_stanza_add_child(message, origin_id);
    xmpp_stanza_release(origin_id);
    
    xmpp_free(account.context, id);
    xmpp_message_set_body(message, body.data());

    // XEP-0385: SIMS (Stateless Inline Media Sharing) + XEP-0066: Out of Band Data
    if (oob && file_meta)
    {
        // Build SIMS reference wrapper
        xmpp_stanza_t *reference = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(reference, "reference");
        xmpp_stanza_set_ns(reference, "urn:xmpp:reference:0");
        xmpp_stanza_set_attribute(reference, "type", "data");
        
        // media-sharing container
        xmpp_stanza_t *media_sharing = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(media_sharing, "media-sharing");
        xmpp_stanza_set_ns(media_sharing, "urn:xmpp:sims:1");
        
        // file element with metadata
        xmpp_stanza_t *file_elem = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(file_elem, "file");
        xmpp_stanza_set_ns(file_elem, "urn:xmpp:jingle:apps:file-transfer:5");
        
        // media-type
        xmpp_stanza_t *media_type = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(media_type, "media-type");
        xmpp_stanza_t *media_type_text = xmpp_stanza_new(account.context);
        xmpp_stanza_set_text(media_type_text, file_meta->content_type.c_str());
        xmpp_stanza_add_child(media_type, media_type_text);
        xmpp_stanza_release(media_type_text);
        xmpp_stanza_add_child(file_elem, media_type);
        xmpp_stanza_release(media_type);
        
        // name
        xmpp_stanza_t *name_elem = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(name_elem, "name");
        xmpp_stanza_t *name_text = xmpp_stanza_new(account.context);
        xmpp_stanza_set_text(name_text, file_meta->filename.c_str());
        xmpp_stanza_add_child(name_elem, name_text);
        xmpp_stanza_release(name_text);
        xmpp_stanza_add_child(file_elem, name_elem);
        xmpp_stanza_release(name_elem);
        
        // size
        xmpp_stanza_t *size_elem = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(size_elem, "size");
        xmpp_stanza_t *size_text = xmpp_stanza_new(account.context);
        xmpp_stanza_set_text(size_text, std::to_string(file_meta->size).c_str());
        xmpp_stanza_add_child(size_elem, size_text);
        xmpp_stanza_release(size_text);
        xmpp_stanza_add_child(file_elem, size_elem);
        xmpp_stanza_release(size_elem);
        
        // hash (SHA-256)
        if (!file_meta->sha256_hash.empty())
        {
            xmpp_stanza_t *hash_elem = xmpp_stanza_new(account.context);
            xmpp_stanza_set_name(hash_elem, "hash");
            xmpp_stanza_set_ns(hash_elem, "urn:xmpp:hashes:2");
            xmpp_stanza_set_attribute(hash_elem, "algo", "sha-256");
            xmpp_stanza_t *hash_text = xmpp_stanza_new(account.context);
            xmpp_stanza_set_text(hash_text, file_meta->sha256_hash.c_str());
            xmpp_stanza_add_child(hash_elem, hash_text);
            xmpp_stanza_release(hash_text);
            xmpp_stanza_add_child(file_elem, hash_elem);
            xmpp_stanza_release(hash_elem);
        }
        
        xmpp_stanza_add_child(media_sharing, file_elem);
        xmpp_stanza_release(file_elem);
        
        // sources
        xmpp_stanza_t *sources = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(sources, "sources");
        
        xmpp_stanza_t *source_ref = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(source_ref, "reference");
        xmpp_stanza_set_ns(source_ref, "urn:xmpp:reference:0");
        xmpp_stanza_set_attribute(source_ref, "type", "data");
        xmpp_stanza_set_attribute(source_ref, "uri", oob->c_str());
        xmpp_stanza_add_child(sources, source_ref);
        xmpp_stanza_release(source_ref);
        
        xmpp_stanza_add_child(media_sharing, sources);
        xmpp_stanza_release(sources);
        
        xmpp_stanza_add_child(reference, media_sharing);
        xmpp_stanza_release(media_sharing);
        
        xmpp_stanza_add_child(message, reference);
        xmpp_stanza_release(reference);
    }
    else if (oob)
    {
        // Fallback to plain XEP-0066 if no file metadata
        xmpp_stanza_t *message__x = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(message__x, "x");
        xmpp_stanza_set_ns(message__x, "jabber:x:oob");

        xmpp_stanza_t *message__x__url = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(message__x__url, "url");

        xmpp_stanza_t *message__x__url__text = xmpp_stanza_new(account.context);
        xmpp_stanza_set_text(message__x__url__text, oob->data());
        xmpp_stanza_add_child(message__x__url, message__x__url__text);
        xmpp_stanza_release(message__x__url__text);

        xmpp_stanza_add_child(message__x, message__x__url);
        xmpp_stanza_release(message__x__url);

        xmpp_stanza_add_child(message, message__x);
        xmpp_stanza_release(message__x);
    }

    xmpp_stanza_t *message__active = xmpp_stanza_new(account.context);
    xmpp_stanza_set_name(message__active, "active");
    xmpp_stanza_set_ns(message__active, "http://jabber.org/protocol/chatstates");
    xmpp_stanza_add_child(message, message__active);
    xmpp_stanza_release(message__active);

    xmpp_stanza_t *message__request = xmpp_stanza_new(account.context);
    xmpp_stanza_set_name(message__request, "request");
    xmpp_stanza_set_ns(message__request, "urn:xmpp:receipts");
    xmpp_stanza_add_child(message, message__request);
    xmpp_stanza_release(message__request);

    xmpp_stanza_t *message__markable = xmpp_stanza_new(account.context);
    xmpp_stanza_set_name(message__markable, "markable");
    xmpp_stanza_set_ns(message__markable, "urn:xmpp:chat-markers:0");
    xmpp_stanza_add_child(message, message__markable);
    xmpp_stanza_release(message__markable);

    xmpp_stanza_t *message__store = xmpp_stanza_new(account.context);
    xmpp_stanza_set_name(message__store, "store");
    xmpp_stanza_set_ns(message__store, "urn:xmpp:hints");
    xmpp_stanza_add_child(message, message__store);
    xmpp_stanza_release(message__store);

    account.connection.send( message);
    xmpp_stanza_release(message);
    if (type != weechat::channel::chat_type::MUC)
    {
        auto *self_user = user::search(&account, account.jid().data());
        auto prefix = self_user ? std::string(self_user->as_prefix_raw()) : std::string(account.jid());
        weechat_printf_date_tags(buffer, 0,
                                 "xmpp_message,message,private,notify_none,self_msg,log1",
                                 "%s\t%s",
                                 prefix.data(),
                                 body.data());
    }

    return WEECHAT_RC_OK;
}

int weechat::channel::send_message(const char *to, const char *body)
{
    send_reads();

    xmpp_stanza_t *message = xmpp_message_new(account.context,
                    type == weechat::channel::chat_type::MUC
                    ? "groupchat" : "chat",
                    to, NULL);

    char *id = xmpp_uuid_gen(account.context);
    xmpp_stanza_set_id(message, id);
    
    // XEP-0359: Add origin-id for stable message identification
    xmpp_stanza_t *origin_id_elem = xmpp_stanza_new(account.context);
    xmpp_stanza_set_name(origin_id_elem, "origin-id");
    xmpp_stanza_set_ns(origin_id_elem, "urn:xmpp:sid:0");
    xmpp_stanza_set_attribute(origin_id_elem, "id", id);
    xmpp_stanza_add_child(message, origin_id_elem);
    xmpp_stanza_release(origin_id_elem);

    xmpp_free(account.context, id);

    if (account.omemo && omemo.enabled)
    {
        xmpp_stanza_t *encrypted = account.omemo.encode(&account, to, body);
        if (!encrypted)
        {
            weechat_printf_date_tags(buffer, 0, "notify_none", "%s%s",
                                     weechat_prefix("error"), "OMEMO Encryption Error");
            set_transport(weechat::channel::transport::PLAIN, 1);
            xmpp_stanza_release(message);
            return WEECHAT_RC_ERROR;
        }
        xmpp_stanza_add_child(message, encrypted);
        xmpp_stanza_release(encrypted);

        xmpp_stanza_t *message__encryption = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(message__encryption, "encryption");
        xmpp_stanza_set_ns(message__encryption, "urn:xmpp:eme:0");
        xmpp_stanza_set_attribute(message__encryption, "namespace",
                "eu.siacs.conversations.axolotl");
        xmpp_stanza_set_attribute(message__encryption, "name", "OMEMO");
        xmpp_stanza_add_child(message, message__encryption);
        xmpp_stanza_release(message__encryption);

        xmpp_message_set_body(message, OMEMO_ADVICE);

        set_transport(weechat::channel::transport::OMEMO, 0);
    }
    else if (pgp.enabled && !pgp.ids.empty())
    {
        xmpp_stanza_t *message__x = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(message__x, "x");
        xmpp_stanza_set_ns(message__x, "jabber:x:encrypted");

        xmpp_stanza_t *message__x__text = xmpp_stanza_new(account.context);
        char *ciphertext = account.pgp.encrypt(buffer, account.pgp_keyid().data(), std::vector(pgp.ids.begin(), pgp.ids.end()), body);
        if (ciphertext)
            xmpp_stanza_set_text(message__x__text, ciphertext);
        else
        {
            weechat_printf_date_tags(buffer, 0, "notify_none", "%s%s",
                                     weechat_prefix("error"), "PGP Error");
            set_transport(weechat::channel::transport::PLAIN, 1);
            xmpp_stanza_release(message);
            return WEECHAT_RC_ERROR;
        }
        ::free(ciphertext);

        xmpp_stanza_add_child(message__x, message__x__text);
        xmpp_stanza_release(message__x__text);

        xmpp_stanza_add_child(message, message__x);
        xmpp_stanza_release(message__x);

        xmpp_stanza_t *message__encryption = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(message__encryption, "encryption");
        xmpp_stanza_set_ns(message__encryption, "urn:xmpp:eme:0");
        xmpp_stanza_set_attribute(message__encryption, "namespace", "jabber:x:encryption");

        xmpp_stanza_add_child(message, message__encryption);
        xmpp_stanza_release(message__encryption);

        xmpp_message_set_body(message, weechat::xmpp::PGP_ADVICE);

        set_transport(weechat::channel::transport::PGP, 0);
    }
    else
    {
        xmpp_message_set_body(message, body);

        set_transport(weechat::channel::transport::PLAIN, 0);
    }

    static const std::regex pattern("https?:[^ ]*");
    std::cmatch match;
    if (transport == weechat::channel::transport::PLAIN &&
            std::regex_search(body, match, pattern)
            && match[0].matched && !match.prefix().length())
    {
        std::string url { &*match[0].first, static_cast<size_t>(match[0].length()) };

        do {
            struct t_hashtable *options = weechat_hashtable_new(8,
                    WEECHAT_HASHTABLE_STRING, WEECHAT_HASHTABLE_STRING,
                    NULL, NULL);
            if (!options) { return WEECHAT_RC_ERROR; };
            weechat_hashtable_set(options, "header", "1");
            weechat_hashtable_set(options, "nobody", "1");
            auto command = "url:" + url;
            const int timeout = 30000;
            struct message_task {
                weechat::channel& channel;
                std::string to;
                std::string body;
                std::string url;
            };
            auto *task = new message_task { *this, to, body, url };
            auto callback = [](const void *pointer, void *,
                    const char *, int ret, const char *out, const char *err) {
                auto task = static_cast<const message_task*>(pointer);
                if (!task) return WEECHAT_RC_ERROR;

                if (ret == 0)
                {
                    const std::string_view prefix = "content-type: ";
                    std::istringstream ss(out ? out : "");
                    std::string line, mime;
                    while (std::getline(ss, line)) {
                        std::transform(line.begin(), line.end(), line.begin(),
                                [](char c) -> char { return std::tolower(c); });
                        if (line.starts_with(prefix)) {
                            mime = line.substr(prefix.size());
                            break;
                        }
                    }
                    if (mime.starts_with("image") || mime.starts_with("video"))
                    {
                        weechat_printf_date_tags(task->channel.buffer, 0,
                                "notify_none,no_log", "[oob]\t%s%s",
                                weechat_color("gray"), mime.data());
                        task->channel.send_message(task->to, task->body, { task->url });
                    }
                    else
                    {
                        weechat_printf_date_tags(task->channel.buffer, 0,
                                "notify_none,no_log", "[curl]\t%s%s",
                                weechat_color("red"), err);
                        task->channel.send_message(task->to.data(), task->body.data());
                    }
                }
                else
                {
                    task->channel.send_message(task->to.data(), task->body.data());
                }

                delete task;
                return WEECHAT_RC_OK;
            };
            struct t_hook *process_hook =
                weechat_hook_process_hashtable(command.data(), options, timeout,
                    callback, task, nullptr);
            weechat_hashtable_free(options);
            (void) process_hook;
            return WEECHAT_RC_OK;
        } while(0);
    }

    xmpp_stanza_t *message__active = xmpp_stanza_new(account.context);
    xmpp_stanza_set_name(message__active, "active");
    xmpp_stanza_set_ns(message__active, "http://jabber.org/protocol/chatstates");
    xmpp_stanza_add_child(message, message__active);
    xmpp_stanza_release(message__active);

    xmpp_stanza_t *message__request = xmpp_stanza_new(account.context);
    xmpp_stanza_set_name(message__request, "request");
    xmpp_stanza_set_ns(message__request, "urn:xmpp:receipts");
    xmpp_stanza_add_child(message, message__request);
    xmpp_stanza_release(message__request);

    xmpp_stanza_t *message__markable = xmpp_stanza_new(account.context);
    xmpp_stanza_set_name(message__markable, "markable");
    xmpp_stanza_set_ns(message__markable, "urn:xmpp:chat-markers:0");
    xmpp_stanza_add_child(message, message__markable);
    xmpp_stanza_release(message__markable);

    xmpp_stanza_t *message__store = xmpp_stanza_new(account.context);
    xmpp_stanza_set_name(message__store, "store");
    xmpp_stanza_set_ns(message__store, "urn:xmpp:hints");
    xmpp_stanza_add_child(message, message__store);
    xmpp_stanza_release(message__store);

    account.connection.send( message);
    xmpp_stanza_release(message);
    if (type != weechat::channel::chat_type::MUC)
    {
        auto *self_user = user::search(&account, account.jid().data());
        auto prefix = self_user ? std::string(self_user->as_prefix_raw()) : std::string(account.jid());
        weechat_printf_date_tags(buffer, 0,
                                 "xmpp_message,message,private,notify_none,self_msg,log1",
                                 "%s\t%s",
                                 prefix.data(),
                                 body);
    }

    return WEECHAT_RC_OK;
}

void weechat::channel::send_reads()
{
    auto i = std::begin(unreads);

    while (i != std::end(unreads))
    {
        auto* unread = &*i;

        xmpp_stanza_t *message = xmpp_message_new(account.context, NULL,
                                                    id.data(), NULL);

        xmpp_stanza_t *message__displayed = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(message__displayed, "displayed");
        xmpp_stanza_set_ns(message__displayed, "urn:xmpp:chat-markers:0");
        xmpp_stanza_set_id(message__displayed, unread->id);
        if (unread->thread)
        {
            xmpp_stanza_t *message__thread = xmpp_stanza_new(account.context);
            xmpp_stanza_set_name(message__thread, "thread");

            xmpp_stanza_t *message__thread__text = xmpp_stanza_new(account.context);
            xmpp_stanza_set_text(message__thread__text, unread->thread);
            xmpp_stanza_add_child(message__thread, message__thread__text);
            xmpp_stanza_release(message__thread__text);

            xmpp_stanza_add_child(message, message__thread);
            xmpp_stanza_release(message__thread);
        }

        xmpp_stanza_add_child(message, message__displayed);
        xmpp_stanza_release(message__displayed);

        account.connection.send( message);
        xmpp_stanza_release(message);

        i = unreads.erase(i);
    }
}

void weechat::channel::send_typing(weechat::user *user)
{
    if (add_self_typing(user))
    {
        xmpp_stanza_t *message = xmpp_message_new(account.context,
                                                  type == weechat::channel::chat_type::MUC
                                                  ? "groupchat" : "chat",
                                                  (user ? user->id : id).data(), NULL);

        xmpp_stanza_t *message__composing = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(message__composing, "composing");
        xmpp_stanza_set_ns(message__composing, "http://jabber.org/protocol/chatstates");

        xmpp_stanza_add_child(message, message__composing);
        xmpp_stanza_release(message__composing);
        
        // XEP-0334: Don't store typing notifications
        xmpp_stanza_t *no_store = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(no_store, "no-store");
        xmpp_stanza_set_ns(no_store, "urn:xmpp:hints");
        xmpp_stanza_add_child(message, no_store);
        xmpp_stanza_release(no_store);

        account.connection.send( message);
        xmpp_stanza_release(message);
    }
}

void weechat::channel::send_paused(weechat::user *user)
{
    xmpp_stanza_t *message = xmpp_message_new(account.context,
                                              type == weechat::channel::chat_type::MUC
                                              ? "groupchat" : "chat",
                                              (user ? user->id : id).data(), NULL);

    xmpp_stanza_t *message__paused = xmpp_stanza_new(account.context);
    xmpp_stanza_set_name(message__paused, "paused");
    xmpp_stanza_set_ns(message__paused, "http://jabber.org/protocol/chatstates");

    xmpp_stanza_add_child(message, message__paused);
    xmpp_stanza_release(message__paused);
    
    // XEP-0334: Don't store chat state notifications
    xmpp_stanza_t *no_store = xmpp_stanza_new(account.context);
    xmpp_stanza_set_name(no_store, "no-store");
    xmpp_stanza_set_ns(no_store, "urn:xmpp:hints");
    xmpp_stanza_add_child(message, no_store);
    xmpp_stanza_release(no_store);

    account.connection.send( message);
    xmpp_stanza_release(message);
}

void weechat::channel::send_inactive(weechat::user *user)
{
    xmpp_stanza_t *message = xmpp_message_new(account.context,
                                              type == weechat::channel::chat_type::MUC
                                              ? "groupchat" : "chat",
                                              (user ? user->id : id).data(), NULL);

    xmpp_stanza_t *message__inactive = xmpp_stanza_new(account.context);
    xmpp_stanza_set_name(message__inactive, "inactive");
    xmpp_stanza_set_ns(message__inactive, "http://jabber.org/protocol/chatstates");

    xmpp_stanza_add_child(message, message__inactive);
    xmpp_stanza_release(message__inactive);
    
    // XEP-0334: Don't store chat state notifications
    xmpp_stanza_t *no_store = xmpp_stanza_new(account.context);
    xmpp_stanza_set_name(no_store, "no-store");
    xmpp_stanza_set_ns(no_store, "urn:xmpp:hints");
    xmpp_stanza_add_child(message, no_store);
    xmpp_stanza_release(no_store);

    account.connection.send( message);
    xmpp_stanza_release(message);
}

void weechat::channel::send_gone(weechat::user *user)
{
    xmpp_stanza_t *message = xmpp_message_new(account.context,
                                              type == weechat::channel::chat_type::MUC
                                              ? "groupchat" : "chat",
                                              (user ? user->id : id).data(), NULL);

    xmpp_stanza_t *message__gone = xmpp_stanza_new(account.context);
    xmpp_stanza_set_name(message__gone, "gone");
    xmpp_stanza_set_ns(message__gone, "http://jabber.org/protocol/chatstates");

    xmpp_stanza_add_child(message, message__gone);
    xmpp_stanza_release(message__gone);
    
    // XEP-0334: Don't store chat state notifications
    xmpp_stanza_t *no_store = xmpp_stanza_new(account.context);
    xmpp_stanza_set_name(no_store, "no-store");
    xmpp_stanza_set_ns(no_store, "urn:xmpp:hints");
    xmpp_stanza_add_child(message, no_store);
    xmpp_stanza_release(no_store);

    account.connection.send( message);
    xmpp_stanza_release(message);
}

void weechat::channel::fetch_mam(const char *id, time_t *start, time_t *end, const char* after)
{
    xmpp_stanza_t *iq = xmpp_iq_new(account.context, "set", "juliet1");
    xmpp_stanza_set_id(iq, id ? id : xmpp_uuid_gen(account.context));

    xmpp_stanza_t *query = xmpp_stanza_new(account.context);
    xmpp_stanza_set_name(query, "query");
    xmpp_stanza_set_ns(query, "urn:xmpp:mam:2");

    xmpp_stanza_t *x = xmpp_stanza_new(account.context);
    xmpp_stanza_set_name(x, "x");
    xmpp_stanza_set_ns(x, "jabber:x:data");
    xmpp_stanza_set_attribute(x, "type", "submit");

    xmpp_stanza_t *field, *value, *text;

    {
        field = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(field, "field");
        xmpp_stanza_set_attribute(field, "var", "FORM_TYPE");
        xmpp_stanza_set_attribute(field, "type", "hidden");

        value = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(value, "value");

        text = xmpp_stanza_new(account.context);
        xmpp_stanza_set_text(text, "urn:xmpp:mam:2");
        xmpp_stanza_add_child(value, text);
        xmpp_stanza_release(text);

        xmpp_stanza_add_child(field, value);
        xmpp_stanza_release(value);

        xmpp_stanza_add_child(x, field);
        xmpp_stanza_release(field);
    }

    {
        field = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(field, "field");
        xmpp_stanza_set_attribute(field, "var", "with");

        value = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(value, "value");

        text = xmpp_stanza_new(account.context);
        xmpp_stanza_set_text(text, this->id.data());  // Use channel JID, not query ID
        xmpp_stanza_add_child(value, text);
        xmpp_stanza_release(text);

        xmpp_stanza_add_child(field, value);
        xmpp_stanza_release(value);

        xmpp_stanza_add_child(x, field);
        xmpp_stanza_release(field);
    }

    if (start)
    {
        field = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(field, "field");
        xmpp_stanza_set_attribute(field, "var", "start");

        value = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(value, "value");

        text = xmpp_stanza_new(account.context);
        char time[256] = {0};
        strftime(time, sizeof(time), "%Y-%m-%dT%H:%M:%SZ", gmtime(start));
        xmpp_stanza_set_text(text, time);
        
        xmpp_stanza_add_child(value, text);
        xmpp_stanza_release(text);

        xmpp_stanza_add_child(field, value);
        xmpp_stanza_release(value);

        xmpp_stanza_add_child(x, field);
        xmpp_stanza_release(field);
    }

    if (end)
    {
        field = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(field, "field");
        xmpp_stanza_set_attribute(field, "var", "end");

        value = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(value, "value");

        text = xmpp_stanza_new(account.context);
        char time[256] = {0};
        strftime(time, sizeof(time), "%Y-%m-%dT%H:%M:%SZ", gmtime(end));
        xmpp_stanza_set_text(text, time);
        
        xmpp_stanza_add_child(value, text);
        xmpp_stanza_release(text);

        xmpp_stanza_add_child(field, value);
        xmpp_stanza_release(value);

        xmpp_stanza_add_child(x, field);
        xmpp_stanza_release(field);
    }

    xmpp_stanza_add_child(query, x);
    xmpp_stanza_release(x);

    if (after)
    {
        xmpp_stanza_t *set, *set__after, *set__after__text;

        set = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(set, "set");
        xmpp_stanza_set_ns(set, "http://jabber.org/protocol/rsm");

        set__after = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(set__after, "after");

        set__after__text = xmpp_stanza_new(account.context);
        xmpp_stanza_set_text(set__after__text, after);
        xmpp_stanza_add_child(set__after, set__after__text);
        xmpp_stanza_release(set__after__text);

        xmpp_stanza_add_child(set, set__after);
        xmpp_stanza_release(set__after);

        xmpp_stanza_add_child(query, set);
        xmpp_stanza_release(set);
    }
    else
    {
        weechat_printf(buffer, "Storing MAM query: id=%s, with=%s", id, this->id.data());
        account.add_mam_query(id, this->id,
                start ? std::optional(*start) : std::optional<time_t>(),
                end ? std::optional(*end) : std::optional<time_t>());
    }

    xmpp_stanza_add_child(iq, query);
    xmpp_stanza_release(query);

    account.connection.send(iq);
    xmpp_stanza_release(iq);
}
