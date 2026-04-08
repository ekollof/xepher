// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <regex>
#include <sstream>
#include <iomanip>
#include <fmt/core.h>
#include <optional>
#include <strophe.h>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "account.hh"
#include "config.hh"
#include "omemo.hh"
#include "user.hh"
#include "channel.hh"
#include "input.hh"
#include "buffer.hh"
#include "debug.hh"
#include "pgp.hh"
#include "util.hh"
#include "xmpp/node.hh"
#include "xmpp/stanza.hh"

namespace {
std::string channel_short_name(weechat::channel::chat_type type, std::string_view name)
{
    if (name.empty())
        return {};

    // FEED buffers: derive a human-readable short name from the feed_key
    // which has the form "<service_jid>/<node>".
    if (type == weechat::channel::chat_type::FEED)
    {
        // Split into service_jid and node at the first '/'.
        auto slash = name.find('/');
        std::string_view service = (slash != std::string_view::npos) ? name.substr(0, slash) : name;
        std::string_view node    = (slash != std::string_view::npos) ? name.substr(slash + 1) : name;

        // Extract the local part of the service JID (everything before '@',
        // or the full string if there is no '@').
        std::string_view local = service;
        auto at = service.find('@');
        if (at != std::string_view::npos)
            local = service.substr(0, at);

        // Well-known PEP node → append a readable hint.
        if (node == "urn:xmpp:microblog:0")
            return fmt::format("={} (blog)", local);

        // Named node (no ':' in it, e.g. "lunduke") → use the node name directly.
        if (node.find(':') == std::string_view::npos)
            return fmt::format("={}", node);

        // Generic URN node → use the last ':'-delimited segment.
        auto colon = node.rfind(':');
        std::string_view suffix = node.substr(colon + 1);
        return fmt::format("={} ({})", local, suffix);
    }

    const char prefix =
        (type == weechat::channel::chat_type::MUC) ? '#' : '@';
    if (name[0] == prefix)
        return std::string(name);

    std::string prefixed;
    prefixed.reserve(name.size() + 1);
    prefixed.push_back(prefix);
    prefixed.append(name);
    return prefixed;
}
} // namespace

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
        weechat_printf_date_tags(buffer, 0, nullptr, "%s%sTransport: %s",
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
                         && (std::string_view(ptr_type) == "channel"))
                     || ((  (type == weechat::channel::chat_type::PM))
                         && (std::string_view(ptr_type) == "private"))
                     || ((  (type == weechat::channel::chat_type::FEED))
                         && (std::string_view(ptr_type) == "feed")))
                && (ptr_account_name == account.name)
                && (weechat_strcasecmp(ptr_remote_jid, name) == 0))
            {
                return ptr_buffer;
            }
        }
        ptr_buffer = (struct t_gui_buffer*)weechat_hdata_move(hdata_buffer, ptr_buffer, 1);
    }

    return nullptr;
}

struct t_gui_buffer *weechat::channel::create_buffer(weechat::channel::chat_type type,
                                                     const char *name)
{
    struct t_gui_buffer *ptr_buffer;
    int buffer_created;
    const char *short_name = nullptr, *localvar_remote_jid = nullptr;

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
                                        &input__data_cb, nullptr, nullptr,
                                        &buffer__close_cb, nullptr, nullptr);
        if (!ptr_buffer)
            return nullptr;

        buffer_created = 1;
    }

    if (buffer_created)
    {
        std::string_view name_sv(name);
        auto slash_pos = name_sv.rfind('/');
        const char *res = (slash_pos != std::string_view::npos) ? name + slash_pos + 1 : nullptr;
        if (!weechat_buffer_get_integer(ptr_buffer, "short_name_is_set"))
        {
            // For FEED buffers pass the full feed_key so channel_short_name
            // can derive a readable name from both the JID and the node.
            auto short_name_value = (type == weechat::channel::chat_type::FEED)
                ? channel_short_name(type, name)
                : channel_short_name(type, res ? res + 1 : name);
            weechat_buffer_set(ptr_buffer, "short_name", short_name_value.c_str());
        }
    }
    else
    {
        short_name = weechat_buffer_get_string(ptr_buffer, "short_name");
        localvar_remote_jid = weechat_buffer_get_string(ptr_buffer,
                                                     "localvar_remote_jid");

        if (!short_name ||
            (localvar_remote_jid && (std::string_view(localvar_remote_jid) == short_name)))
        {
            const std::string node_s = ::jid(nullptr, name).local;
            const char *node_cstr = node_s.empty() ? name : node_s.c_str();
            auto short_name_value = (type == weechat::channel::chat_type::FEED)
                ? channel_short_name(type, name)
                : channel_short_name(type, node_cstr);
            weechat_buffer_set(ptr_buffer, "short_name", short_name_value.c_str());
        }
    }
    if(!(account.nickname().size()))
    {
        const std::string node_s = ::jid(nullptr, account.jid()).local;
        account.nickname(node_s.empty() ? account.jid() : node_s);
    }

    // Set notify level for buffer: "0" = never add to hotlist
    //                              "1" = add for highlights only
    //                              "2" = add for highlights and messages
    //                              "3" = add for all messages.
    weechat_buffer_set(ptr_buffer, "notify",
                       (type == weechat::channel::chat_type::PM) ? "3" : "2");
    weechat_buffer_set(ptr_buffer, "localvar_set_type",
                       (type == weechat::channel::chat_type::PM) ? "private"
                     : (type == weechat::channel::chat_type::FEED) ? "feed"
                     : "channel");
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
        if (type != weechat::channel::chat_type::PM
            && type != weechat::channel::chat_type::FEED)
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
    if (type == weechat::channel::chat_type::PM
        || type == weechat::channel::chat_type::FEED)
        return;

    // Sort order constants for XEP-0045 MUC role/affiliation prefixes.
    // Lower numbers appear first in the nicklist; 999 = no-role sentinel.
    static constexpr char grp_owner[]   = "000|~";
    static constexpr char grp_admin[]   = "001|&";
    static constexpr char grp_op[]      = "002|@";
    static constexpr char grp_halfop[]  = "003|%";
    static constexpr char grp_voice[]   = "004|+";
    static constexpr char grp_unknown[] = "005|?";
    static constexpr char grp_novoice[] = "006|!";
    static constexpr char grp_norole[]  = "999|.";

    weechat_nicklist_add_group(buffer, nullptr, grp_owner,
                               "weechat.color.nicklist_group", 1);
    weechat_nicklist_add_group(buffer, nullptr, grp_admin,
                               "weechat.color.nicklist_group", 1);
    weechat_nicklist_add_group(buffer, nullptr, grp_op,
                               "weechat.color.nicklist_group", 1);
    weechat_nicklist_add_group(buffer, nullptr, grp_halfop,
                               "weechat.color.nicklist_group", 1);
    weechat_nicklist_add_group(buffer, nullptr, grp_voice,
                               "weechat.color.nicklist_group", 1);
    weechat_nicklist_add_group(buffer, nullptr, grp_unknown,
                               "weechat.color.nicklist_group", 1);
    weechat_nicklist_add_group(buffer, nullptr, grp_novoice,
                               "weechat.color.nicklist_group", 1);
    weechat_nicklist_add_group(buffer, nullptr, grp_norole,
                               "weechat.color.nicklist_group", 1);
}

weechat::channel::channel(weechat::account& account,
                          weechat::channel::chat_type type,
                          std::string_view id, std::string_view name) : id(id), name(name), type(type), account(account)
{
    if (id.empty() || name.empty())
        throw std::invalid_argument("channel()");

    std::string name_str(name);
    buffer = weechat::channel::create_buffer(type, name_str.c_str());
    if (!buffer)
        throw std::invalid_argument("buffer fail");
    else if (type == weechat::channel::chat_type::PM)
    {
        // If this PM is with a MUC occupant, position the PM buffer right after
        // the MUC buffer in the buffer list for convenience.
        // Do NOT use weechat_buffer_merge — that merges display (shows MUC history
        // in the PM buffer). Instead, move the PM buffer to the slot after the MUC.
        auto muc_channel = account.channels.find(jid(account.context,
                                                                               std::string(id)).bare.data());
        if (muc_channel != account.channels.end())
        {
            int muc_num = weechat_buffer_get_integer(muc_channel->second.buffer, "number");
            weechat_buffer_set(buffer, "number", fmt::format("{}", muc_num + 1).c_str());
        }
    }

    self_typing_hook_timer = weechat_hook_timer(1 * 1000, 0, 0,
                                                &weechat::channel::self_typing_cb,
                                                this, nullptr);

    omemo.enabled = 0;
    omemo.devicelist_requests = weechat_hashtable_new(64,
            WEECHAT_HASHTABLE_STRING, WEECHAT_HASHTABLE_POINTER, nullptr, nullptr);
    omemo.bundle_requests = weechat_hashtable_new(64,
            WEECHAT_HASHTABLE_STRING, WEECHAT_HASHTABLE_POINTER, nullptr, nullptr);

    add_nicklist_groups();

    // Smart filter: assume we are joining (initial presence flood) for MUC channels.
    // Will be cleared when status 110 (self-presence) is received.
    if (type == weechat::channel::chat_type::MUC)
        joining = true;

    if (type != weechat::channel::chat_type::MUC)
    {
        // XEP-0085: announce <active> when opening a new PM conversation
        // Skip for FEED buffers — they are read-only pubsub buffers
        if (type == weechat::channel::chat_type::PM)
        {
            auto *self_user = weechat::user::search(&account, account.jid_device().data());
            send_active(self_user);
        }

        time_t now = time(nullptr);
        time_t start;

        // MAM fetch is only for PM channels; FEED buffers are read-only pubsub
        if (type == weechat::channel::chat_type::PM)
        {
        // Load last fetch timestamp from cache
        if (last_mam_fetch == 0)
            last_mam_fetch = account.mam_cache_get_last_timestamp(this->id);
        
        // If the channel was previously closed (-1), treat it as a fresh open:
        // reset the flag so MAM runs normally this time.
        if (last_mam_fetch == -1)
        {
            last_mam_fetch = 0;
            account.mam_cache_set_last_timestamp(this->id, 0);
        }
        
        // Load and display cached messages
        if (last_mam_fetch > 0)
            account.mam_cache_load_messages(this->id, buffer);
        
        // If we have a persisted last fetch timestamp, always fetch since then.
        // The old 5-minute window caused us to re-fetch the last 7 days on any
        // later reconnect/open, which looks like repeated/out-of-order history.
        if (last_mam_fetch > 0)
            start = last_mam_fetch;
        else
            start = now - (7 * 86400);
        
        time_t end = now;
        std::string mam_uuid = stanza::uuid(account.context);
        fetch_mam(mam_uuid.c_str(), &start, &end, nullptr);
        } // PM-only MAM block
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
                     WEECHAT_LIST_POS_END, nullptr);

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

// typing_cb removed: the native WeeChat typing plugin manages expiry via its own timer.
// We drive it via typing_set_nick / typing_reset_buffer signals instead.

// typing_search is unused now that we delegate to the native typing plugin.

int weechat::channel::set_typing_state(weechat::user *user, const char *state)
{
    if (!user || user->id.empty() || !buffer)
        return 0;

    // Build nick string (resource for MUC, bare JID for PM)
    std::string nick;
    if (type == chat_type::MUC)
        nick = ::jid(nullptr, user->id).resource;
    else
        nick = ::jid(nullptr, user->id).bare;
    if (nick.empty())
        nick = user->id;

    if (nick.empty())
        return 0;

    // Signal format: "<buf_ptr_hex>;<state>;<nick>"
    std::string signal_data = fmt::format("{:x};{};{}",
        (unsigned long)(void *)buffer, state, nick);
    weechat_hook_signal_send("typing_set_nick",
                             WEECHAT_HOOK_SIGNAL_STRING,
                             signal_data.data());
    return 1;
}

int weechat::channel::remove_typing(weechat::user *user)
{
    return set_typing_state(user, "off");
}

int weechat::channel::add_typing(weechat::user *user)
{
    return set_typing_state(user, "typing");
}

int weechat::channel::self_typing_cb(const void *pointer, void *data, int remaining_calls)
{
    time_t now;

    (void) data;
    (void) remaining_calls;

    if (!pointer)
        return WEECHAT_RC_ERROR;

    weechat::channel *channel = (weechat::channel *)pointer;

    now = time(nullptr);

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
        if (user && !user->id.empty())
        {
            const std::string bare = ::jid(nullptr, user->id).bare;
            new_typing.name = bare.empty() ? user->id : bare;
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
        the_typing->ts = time(nullptr);
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
    if (self_typing_hook_timer)
    {
        weechat_unhook(self_typing_hook_timer);
        self_typing_hook_timer = nullptr;
    }

    // Tell the native typing plugin to forget all typists for this buffer
    if (buffer)
        weechat_hook_signal_send("typing_reset_buffer",
                                 WEECHAT_HOOK_SIGNAL_POINTER,
                                 buffer);
    
    // Clear MAM cache for PM channels to prevent auto-recreation on reconnect
    if (type == chat_type::PM)
    {
        account.mam_cache_clear_messages(id);
        account.mam_cache_set_last_timestamp(id, -1);  // -1 = deliberately closed
    }
    
    // Free members_speaking lists (created via weechat_list_new, must be freed)
    if (members_speaking[0])
        weechat_list_free(members_speaking[0]);
    if (members_speaking[1])
        weechat_list_free(members_speaking[1]);
}

// Smart filter: record that `nick` spoke right now in this channel.
void weechat::channel::record_speak(const char *nick)
{
    if (!nick) return;
    last_speak[std::string(nick)] = std::time(nullptr);
}

// Smart filter: return true if this nick's presence line should get the
// `xmpp_smart_filter` tag (i.e. the user has not spoken recently).
// Also returns true during the initial MUC join flood (joining == true).
bool weechat::channel::smart_filter_nick(const char *nick) const
{
    if (!weechat::config::instance) return false;
    if (!weechat::config::instance->look.smart_filter.boolean()) return false;

    // Always suppress during initial join flood
    if (joining) return true;

    if (!nick) return true;

    auto it = last_speak.find(std::string(nick));
    if (it == last_speak.end()) return true; // never spoken

    int delay_minutes = weechat::config::instance->look.smart_filter_delay.integer();
    time_t threshold = std::time(nullptr) - static_cast<time_t>(delay_minutes) * 60;
    return it->second < threshold;
}

void weechat::channel::mark_chat_state_supported(const std::string& from_jid)
{
    chat_state_supported.insert(from_jid);
}

bool weechat::channel::is_chat_state_supported(const std::string& to_jid) const
{
    // XEP-0085 §5.1: MUST NOT send chat state notifications to MUC rooms.
    if (type == weechat::channel::chat_type::MUC)
        return false;

    // For PM: only send if the contact has previously sent us a chat state,
    // or if the full JID (with resource) or bare JID is in the support set.
    if (chat_state_supported.count(to_jid))
        return true;

    // Also check bare JID
    const auto sep = to_jid.find('/');
    if (sep != std::string::npos)
    {
        std::string bare = to_jid.substr(0, sep);
        if (chat_state_supported.count(bare))
            return true;
    }

    return false;
}

void weechat::channel::update_topic(const char* topic, const char* creator, int last_set)
{
    this->topic.value = topic ? std::optional<std::string>(topic) : std::nullopt;
    this->topic.creator = creator ? std::optional<std::string>(creator) : std::nullopt;
    this->topic.last_set = last_set;

    if (this->topic.value.has_value())
        weechat_buffer_set(buffer, "title", this->topic.value->c_str());
    else
        weechat_buffer_set(buffer, "title", "");
}

void weechat::channel::update_name(const char* name)
{
    if (name)
    {
        auto short_name_value = channel_short_name(type, name);
        weechat_buffer_set(buffer, "short_name", short_name_value.c_str());
    }
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
        auto& new_member = members[std::string(id)];
        new_member.id = id;
        member = &new_member;
    }
    else
    {
        member = *member_opt;
        if (user)
            user->nicklist_remove(&account, this);
    }

    if (user)
        user->nicklist_add(&account, this);
    else return member; // no user object yet; member was created above, return it without printing a join line

    const std::string jid_bare_s = ::jid(nullptr, user->id).bare;
    const std::string jid_resource_s = ::jid(nullptr, user->id).resource;
    const char *jid_bare = jid_bare_s.empty() ? nullptr : jid_bare_s.c_str();
    const char *jid_resource = jid_resource_s.empty() ? nullptr : jid_resource_s.c_str();

    // Determine the resource nick used for smart-filter lookup
    const char *res_nick = jid_resource ? jid_resource : id;
    std::string enter_tags = smart_filter_nick(res_nick)
        ? "xmpp_presence,enter,log4,xmpp_smart_filter"
        : "xmpp_presence,enter,log4";

    if (weechat_strcasecmp(jid_bare, id) == 0
             && type == weechat::channel::chat_type::MUC)        weechat_printf_date_tags(buffer, 0, enter_tags.c_str(), "%s%s%s%s%s %s%s%s%s %s%s%s%s%s%s%s%s%s%s%s%s%s%s",
                                 weechat_prefix("join"),
                                 user->as_prefix_raw().data(),
                                 client ? " (" : "",
                                 client ? client : "",
                                 client ? ")" : "",
                                 user->profile.status.has_value() ? "is " : "",
                                 weechat_color("irc.color.message_join"),
                                 user->profile.status.has_value() ? user->profile.status->c_str() : (user->profile.idle.has_value() ? "idle" : "entered"),
                                 weechat_color("reset"),
                                 id,
                                 user->profile.status_text.has_value() ? " [" : "",
                                 user->profile.status_text.has_value() ? user->profile.status_text->c_str() : "",
                                 user->profile.status_text.has_value() ? "]" : "",
                                 weechat_color("yellow"), " as ", weechat_color("reset"),
                                 user->profile.affiliation.has_value() ? user->profile.affiliation->c_str() : "",
                                 user->profile.affiliation.has_value() ? " " : "",
                                 user->profile.role.has_value() ? user->profile.role->c_str() : "",
                                 user->profile.pgp_id.has_value() ? weechat_color("gray") : "",
                                 user->profile.pgp_id.has_value() ? " with PGP:" : "",
                                 user->profile.pgp_id.has_value() ? user->profile.pgp_id->c_str() : "",
                                 user->profile.pgp_id.has_value() ? weechat_color("reset") : "");
    else
        weechat_printf_date_tags(buffer, 0, enter_tags.c_str(), "%s%s (%s) %s%s%s%s %s%s%s%s%s%s%s%s%s",
                                 weechat_prefix("join"),
                                 jid_resource ? user->as_prefix_raw().data() : "You",
                                 jid_resource ? jid_resource : user->as_prefix_raw().data(),
                                 user->profile.status.has_value() ? "is " : "",
                                 weechat_color("irc.color.message_join"),
                                 user->profile.status.has_value() ? user->profile.status->c_str() : (user->profile.idle.has_value() ? "idle" : "entered"),
                                 weechat_color("reset"),
                                 user->profile.idle.has_value() ? "since " : "",
                                 user->profile.idle.has_value() ? user->profile.idle->data() : "",
                                 user->profile.status_text.has_value() ? " [" : "",
                                 user->profile.status_text.has_value() ? user->profile.status_text->c_str() : "",
                                 user->profile.status_text.has_value() ? "]" : "",
                                 (user->profile.pgp_id.has_value() || user->profile.omemo) ? weechat_color("gray") : "",
                                 (user->profile.pgp_id.has_value() || user->profile.omemo) ? " with " : "",
                                 user->profile.pgp_id.has_value() ? "PGP:" : "",
                                 user->profile.pgp_id.has_value() ? user->profile.pgp_id->c_str() : "",
                                 (user->profile.omemo && user->profile.pgp_id.has_value()) ? " and " : "",
                                 user->profile.omemo ? "OMEMO" : "",
                                 (user->profile.pgp_id.has_value() || user->profile.omemo) ? weechat_color("reset") : "");

    return member;
}

std::optional<weechat::channel::member*> weechat::channel::member_search(const char *id)
{
    if (!id)
        return std::nullopt;

    for (auto& ptr_member : members)
    {
        if (weechat_strcasecmp(ptr_member.second.id.c_str(), id) == 0)
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

    const std::string jid_bare_s = ::jid(nullptr, user->id).bare;
    const std::string jid_resource_s = ::jid(nullptr, user->id).resource;
    const char *jid_bare = jid_bare_s.empty() ? nullptr : jid_bare_s.c_str();
    const char *jid_resource = jid_resource_s.empty() ? nullptr : jid_resource_s.c_str();

    const char *res_nick = jid_resource ? jid_resource : id;
    std::string leave_tags = smart_filter_nick(res_nick)
        ? "xmpp_presence,leave,log4,xmpp_smart_filter"
        : "xmpp_presence,leave,log4";

    if (weechat_strcasecmp(jid_bare, id) == 0
        && type == weechat::channel::chat_type::MUC)
        weechat_printf_date_tags(buffer, 0, leave_tags.c_str(),
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
        weechat_printf_date_tags(buffer, 0, leave_tags.c_str(),
                                 "%s%s (%s) %sleft%s %s %s%s%s",
                                 weechat_prefix("quit"),
                                 jid_bare,
                                 jid_resource,
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
    // Reuse the main send path for regular text messages so PM OMEMO logic
    // (auto-enable, capability gating, encode/decode behavior) stays consistent.
    if (!oob && !file_meta)
        return send_message(std::string_view(to), std::string_view(body), /*skip_probe=*/false);

    std::shared_ptr<xmpp_stanza_t> message {
        xmpp_message_new(account.context,
                         type == weechat::channel::chat_type::MUC
                         ? "groupchat" : "chat",
                         to.data(), nullptr),
        xmpp_stanza_release };

    std::string saved_id = stanza::uuid(account.context);
    xmpp_stanza_set_id(message.get(), saved_id.c_str());

    // XEP-0359: Add origin-id for stable message identification
    {
        auto origin_id = stanza_node(account.context, "origin-id", "urn:xmpp:sid:0",
                                     {{"id", saved_id.c_str()}});
        xmpp_stanza_add_child(message.get(), origin_id.get());
    }

    xmpp_message_set_body(message.get(), body.data());

    // Helper: make a new stanza child (optionally add to parent)
    auto make_child = [&](xmpp_stanza_t *parent,
                          const char *name, const char *ns = nullptr)
        -> std::shared_ptr<xmpp_stanza_t>
    {
        auto s = stanza_node(account.context, name, ns);
        if (parent) xmpp_stanza_add_child(parent, s.get());
        return s;
    };

    // Helper: make a text-node child and add it to parent
    auto add_text_child = [&](xmpp_stanza_t *parent, const char *name,
                               const char *ns, const char *text_content)
    {
        auto el = stanza_text_node(account.context, name, text_content, ns);
        xmpp_stanza_add_child(parent, el.get());
    };

    // XEP-0385: SIMS (Stateless Inline Media Sharing) + XEP-0066: Out of Band Data
    if (oob && file_meta)
    {
        // ── XEP-0447: Stateless File Sharing (preferred) ──
        auto file_sharing = make_child(nullptr, "file-sharing", "urn:xmpp:sfs:0");
        xmpp_stanza_set_attribute(file_sharing.get(), "disposition", "inline");

        auto sfs_file = make_child(nullptr, "file", "urn:xmpp:file:metadata:0");
        add_text_child(sfs_file.get(), "media-type", nullptr, file_meta->content_type.c_str());
        add_text_child(sfs_file.get(), "name",       nullptr, file_meta->filename.c_str());
        add_text_child(sfs_file.get(), "size",       nullptr, std::to_string(file_meta->size).c_str());

        // width / height — only for images
        if (file_meta->width > 0 && file_meta->height > 0)
        {
            add_text_child(sfs_file.get(), "width",  nullptr, std::to_string(file_meta->width).c_str());
            add_text_child(sfs_file.get(), "height", nullptr, std::to_string(file_meta->height).c_str());
        }

        // hash (SHA-256)
        if (!file_meta->sha256_hash.empty())
        {
            auto hash_elem = make_child(nullptr, "hash", "urn:xmpp:hashes:2");
            xmpp_stanza_set_attribute(hash_elem.get(), "algo", "sha-256");
            xmpp_stanza_set_text(hash_elem.get(), file_meta->sha256_hash.c_str());
            xmpp_stanza_add_child(sfs_file.get(), hash_elem.get());
        }

        xmpp_stanza_add_child(file_sharing.get(), sfs_file.get());

        // sources
        auto sfs_sources = make_child(nullptr, "sources");

        if (file_meta->esfs)
        {
            // XEP-0448: Encrypted File Sharing
            auto enc = make_child(nullptr, "encrypted", "urn:xmpp:esfs:0");
            xmpp_stanza_set_attribute(enc.get(), "cipher",
                "urn:xmpp:ciphers:aes-256-gcm-nopadding:0");

            add_text_child(enc.get(), "key", nullptr, file_meta->esfs->key_b64.c_str());
            add_text_child(enc.get(), "iv",  nullptr, file_meta->esfs->iv_b64.c_str());

            // <hash xmlns='urn:xmpp:hashes:2' algo='sha-256'>…</hash>
            {
                auto hash_el = make_child(nullptr, "hash", "urn:xmpp:hashes:2");
                xmpp_stanza_set_attribute(hash_el.get(), "algo", "sha-256");
                xmpp_stanza_set_text(hash_el.get(), file_meta->esfs->cipher_hash_b64.c_str());
                xmpp_stanza_add_child(enc.get(), hash_el.get());
            }

            // Inner <sources><url-data .../></sources>
            auto inner_sources = make_child(nullptr, "sources");
            auto url_data = make_child(nullptr, "url-data",
                "http://jabber.org/protocol/url-data");
            xmpp_stanza_set_attribute(url_data.get(), "target", oob->c_str());
            xmpp_stanza_add_child(inner_sources.get(), url_data.get());
            xmpp_stanza_add_child(enc.get(), inner_sources.get());

            xmpp_stanza_add_child(sfs_sources.get(), enc.get());
        }
        else
        {
            auto url_data = make_child(nullptr, "url-data",
                "http://jabber.org/protocol/url-data");
            xmpp_stanza_set_attribute(url_data.get(), "target", oob->c_str());
            xmpp_stanza_add_child(sfs_sources.get(), url_data.get());
        }

        xmpp_stanza_add_child(file_sharing.get(), sfs_sources.get());
        xmpp_stanza_add_child(message.get(), file_sharing.get());

        // XEP-0428: Fallback Indication — MUST be present when body contains
        // a fallback URL for clients that don't support XEP-0447/0448.
        {
            auto fallback = make_child(nullptr, "fallback", "urn:xmpp:fallback:0");
            xmpp_stanza_set_attribute(fallback.get(), "for", "urn:xmpp:sfs:0");
            auto fb_body = make_child(nullptr, "body");
            xmpp_stanza_add_child(fallback.get(), fb_body.get());
            xmpp_stanza_add_child(message.get(), fallback.get());
        }

        // ── XEP-0385: SIMS — kept for backward compat ──
        auto reference = make_child(nullptr, "reference", "urn:xmpp:reference:0");
        xmpp_stanza_set_attribute(reference.get(), "type", "data");
        xmpp_stanza_set_attribute(reference.get(), "begin", "0");
        xmpp_stanza_set_attribute(reference.get(), "end",
            std::to_string(body.size()).c_str()); // body char offset per XEP-0385 §4

        auto media_sharing = make_child(nullptr, "media-sharing", "urn:xmpp:sims:1");
        auto file_elem = make_child(nullptr, "file",
            "urn:xmpp:jingle:apps:file-transfer:5");

        add_text_child(file_elem.get(), "media-type", nullptr, file_meta->content_type.c_str());
        add_text_child(file_elem.get(), "name",       nullptr, file_meta->filename.c_str());
        add_text_child(file_elem.get(), "size",       nullptr, std::to_string(file_meta->size).c_str());

        if (!file_meta->sha256_hash.empty())
        {
            auto hash_elem = make_child(nullptr, "hash", "urn:xmpp:hashes:2");
            xmpp_stanza_set_attribute(hash_elem.get(), "algo", "sha-256");
            xmpp_stanza_set_text(hash_elem.get(), file_meta->sha256_hash.c_str());
            xmpp_stanza_add_child(file_elem.get(), hash_elem.get());
        }

        xmpp_stanza_add_child(media_sharing.get(), file_elem.get());

        auto sources = make_child(nullptr, "sources");
        auto source_ref = make_child(nullptr, "reference", "urn:xmpp:reference:0");
        xmpp_stanza_set_attribute(source_ref.get(), "type", "data");
        xmpp_stanza_set_attribute(source_ref.get(), "uri", oob->c_str());
        xmpp_stanza_add_child(sources.get(), source_ref.get());
        xmpp_stanza_add_child(media_sharing.get(), sources.get());
        xmpp_stanza_add_child(reference.get(), media_sharing.get());
        xmpp_stanza_add_child(message.get(), reference.get());

        // XEP-0066 OOB fallback — include alongside SIMS/SFS for legacy clients
        auto oob_x = make_child(nullptr, "x", "jabber:x:oob");
        add_text_child(oob_x.get(), "url", nullptr, oob->c_str());
        xmpp_stanza_add_child(message.get(), oob_x.get());
    }
    else if (oob)
    {
        // Fallback to plain XEP-0066 if no file metadata
        auto oob_x = make_child(nullptr, "x", "jabber:x:oob");
        add_text_child(oob_x.get(), "url", nullptr, oob->data());
        xmpp_stanza_add_child(message.get(), oob_x.get());
    }

    {
        auto active = make_child(nullptr, "active",
            "http://jabber.org/protocol/chatstates");
        xmpp_stanza_add_child(message.get(), active.get());
    }

    // XEP-0184 §5.4: MUST NOT include <request/> in groupchat messages.
    // XEP-0333 §4.1: MUST NOT include <markable/> in groupchat messages.
    if (type != weechat::channel::chat_type::MUC)
    {
        auto req = make_child(nullptr, "request", "urn:xmpp:receipts");
        xmpp_stanza_add_child(message.get(), req.get());

        auto markable = make_child(nullptr, "markable", "urn:xmpp:chat-markers:0");
        xmpp_stanza_add_child(message.get(), markable.get());
    }

    {
        auto store = make_child(nullptr, "store", "urn:xmpp:hints");
        xmpp_stanza_add_child(message.get(), store.get());
    }

    // XEP-0372: References — add <reference type="mention"> for each @nick in body
    {
        static const std::regex at_re("@([\\w.\\-]+)", std::regex::ECMAScript);
        auto begin_it = std::sregex_iterator(body.begin(), body.end(), at_re);
        auto end_it   = std::sregex_iterator();
        for (auto it = begin_it; it != end_it; ++it)
        {
            const std::smatch& m = *it;
            std::string nick = m[1].str();
            std::string mention_jid;

            for (auto& [member_id, mem] : members)
            {
                (void)mem;
                if (type == weechat::channel::chat_type::MUC)
                {
                    auto slash = member_id.rfind('/');
                    if (slash != std::string::npos)
                    {
                        std::string resource = member_id.substr(slash + 1);
                        if (weechat_strcasecmp(resource.c_str(), nick.c_str()) == 0)
                        {
                            mention_jid = member_id;
                            break;
                        }
                    }
                }
                else
                {
                    auto at_pos = member_id.find('@');
                    std::string node = (at_pos != std::string::npos)
                                       ? member_id.substr(0, at_pos) : member_id;
                    if (weechat_strcasecmp(node.c_str(), nick.c_str()) == 0)
                    {
                        mention_jid = member_id;
                        break;
                    }
                }
            }

            if (mention_jid.empty())
                continue;

            auto utf8_codepoints = [](const std::string& s, size_t byte_end) -> size_t {
                size_t cp = 0;
                for (size_t b = 0; b < byte_end && b < s.size(); ) {
                    unsigned char c = static_cast<unsigned char>(s[b]);
                    if      (c < 0x80)  b += 1;
                    else if (c < 0xE0)  b += 2;
                    else if (c < 0xF0)  b += 3;
                    else                b += 4;
                    ++cp;
                }
                return cp;
            };
            std::string uri = "xmpp:" + mention_jid;
            size_t byte_begin = static_cast<size_t>(m.position(0));
            size_t byte_end   = byte_begin + static_cast<size_t>(m.length(0));
            size_t ref_begin  = utf8_codepoints(body, byte_begin);
            size_t ref_end    = utf8_codepoints(body, byte_end);

            auto ref = make_child(nullptr, "reference", "urn:xmpp:reference:0");
            xmpp_stanza_set_attribute(ref.get(), "type", "mention");
            xmpp_stanza_set_attribute(ref.get(), "uri", uri.c_str());
            xmpp_stanza_set_attribute(ref.get(), "begin",
                std::to_string(ref_begin).c_str());
            xmpp_stanza_set_attribute(ref.get(), "end",
                std::to_string(ref_end).c_str());
            xmpp_stanza_add_child(message.get(), ref.get());
        }
    }

    account.connection.send(message.get());
    if (type != weechat::channel::chat_type::MUC)
    {
        auto *self_user = user::search(&account, account.jid().data());
        auto prefix = self_user ? std::string(self_user->as_prefix_raw()) : std::string(account.jid());
        std::string tag = "xmpp_message,message,private,notify_none,self_msg,log1,id_" + saved_id;
        weechat_printf_date_tags(buffer, 0,
                                 tag.c_str(),
                                 "%s\t%s ⌛",
                                 prefix.data(),
                                 body.data());
    }

    return WEECHAT_RC_OK;
}

int weechat::channel::send_message(std::string_view to, std::string_view body, bool skip_probe)
{
    send_reads();

    std::string peer_bare(to);
    if (const auto slash = peer_bare.find('/'); slash != std::string::npos)
        peer_bare.resize(slash);

    std::string to_str(to);
    std::string body_str(body);

    std::shared_ptr<xmpp_stanza_t> message {
        xmpp_message_new(account.context,
                         type == weechat::channel::chat_type::MUC
                         ? "groupchat" : "chat",
                         to_str.c_str(), nullptr),
        xmpp_stanza_release };

    std::string saved_id = stanza::uuid(account.context);
    xmpp_stanza_set_id(message.get(), saved_id.c_str());

    // XEP-0359: Add origin-id for stable message identification
    {
        auto origin_id_elem = stanza_node(account.context, "origin-id", "urn:xmpp:sid:0",
                                          {{"id", saved_id.c_str()}});
        xmpp_stanza_add_child(message.get(), origin_id_elem.get());
    }

    // XEP-0045 §7.5: for MUC private messages (chat to an occupant JID
    // room@service/nick), add <x xmlns='…muc#user'/> so that XEP-0280
    // Message Carbons can correctly synchronise the message to other clients.
    // Detection: type is PM, to has a resource, and peer_bare is a known MUC.
    if (type == weechat::channel::chat_type::PM
        && to_str.find('/') != std::string::npos
        && account.channels.count(peer_bare)
        && account.channels.at(peer_bare).type == weechat::channel::chat_type::MUC)
    {
        auto muc_x = stanza_node(account.context, "x",
                                 "http://jabber.org/protocol/muc#user");
        xmpp_stanza_add_child(message.get(), muc_x.get());
    }

    if (account.omemo && omemo.enabled)
    {
        std::shared_ptr<xmpp_stanza_t> encrypted;
        constexpr const char *eme_namespace = "eu.siacs.conversations.axolotl";

        // Use a null-safe helper: xmpp_stanza_release(nullptr) segfaults, so
        // only install the deleter when the raw pointer is non-null.
        auto make_encrypted = [](xmpp_stanza_t *raw) -> std::shared_ptr<xmpp_stanza_t> {
            if (!raw) return {};
            return { raw, xmpp_stanza_release };
        };

        encrypted = make_encrypted(
            account.omemo.encode(&account, buffer, to_str.c_str(), body_str.c_str()));

        if (!encrypted)
        {
            if (type == weechat::channel::chat_type::PM)
            {
                if (flushing_pending_omemo)
                    return WEECHAT_RC_ERROR;

                queue_pending_omemo_message(body_str);
                account.omemo.request_axolotl_devicelist(account, peer_bare);
                weechat_printf_date_tags(buffer, 0, "notify_none", "%s%s",
                                         weechat_prefix("network"),
                                         "OMEMO not ready yet; queued message and requested device/bundle updates");
                return WEECHAT_RC_OK;
            }

            weechat_printf_date_tags(buffer, 0, "notify_none", "%s%s",
                                     weechat_prefix("error"), "OMEMO Encryption Error");
            weechat_printf_date_tags(buffer, 0, "notify_none", "%s%s",
                                     weechat_prefix("error"),
                                     "Message not sent; OMEMO stays enabled for this channel");
            return WEECHAT_RC_ERROR;
        }
        xmpp_stanza_add_child(message.get(), encrypted.get());

        {
            auto enc_elem = stanza_node(account.context, "encryption", "urn:xmpp:eme:0",
                                        {{"namespace", eme_namespace}});
            xmpp_stanza_add_child(message.get(), enc_elem.get());
        }

        xmpp_message_set_body(message.get(), OMEMO_ADVICE);
        set_transport(weechat::channel::transport::OMEMO, 0);
    }
    else if (pgp.enabled && !pgp.ids.empty())
    {
        auto pgp_x = stanza_node(account.context, "x", "jabber:x:encrypted");

        auto ciphertext = account.pgp.encrypt(buffer, account.pgp_keyid().data(),
            std::vector(pgp.ids.begin(), pgp.ids.end()), body_str.c_str());
        if (ciphertext)
        {
            auto pgp_text = stanza_text_node(account.context, "", ciphertext->c_str());
            xmpp_stanza_add_child(pgp_x.get(), pgp_text.get());
        }
        else
        {
            weechat_printf_date_tags(buffer, 0, "notify_none", "%s%s",
                                     weechat_prefix("error"), "PGP Error");
            set_transport(weechat::channel::transport::PLAIN, 1);
            return WEECHAT_RC_ERROR;
        }

        xmpp_stanza_add_child(message.get(), pgp_x.get());

        {
            auto enc_elem = stanza_node(account.context, "encryption", "urn:xmpp:eme:0",
                                         {{"namespace", "jabber:x:encrypted"}});
            xmpp_stanza_add_child(message.get(), enc_elem.get());
        }

        xmpp_message_set_body(message.get(), weechat::xmpp::PGP_ADVICE);
        set_transport(weechat::channel::transport::PGP, 0);
    }
    else
    {
        xmpp_message_set_body(message.get(), body_str.c_str());
        set_transport(weechat::channel::transport::PLAIN, 0);
    }

    static const std::regex pattern("https?:[^ ]*");
    std::smatch match;
    if (!skip_probe &&
            transport == weechat::channel::transport::PLAIN &&
            std::regex_search(body_str, match, pattern)
            && match[0].matched && !match.prefix().length())
    {
        std::string url { &*match[0].first, static_cast<size_t>(match[0].length()) };

        do {
            struct t_hashtable *options = weechat_hashtable_new(8,
                    WEECHAT_HASHTABLE_STRING, WEECHAT_HASHTABLE_STRING,
                    nullptr, nullptr);
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
                std::string output;
            };
            auto task_owned = std::make_unique<message_task>(message_task { *this, to_str, body_str, url });
            auto callback = [](const void *pointer, void *,
                    const char *, int ret, const char *out, const char * /*err*/) {
                auto *task_raw = static_cast<message_task*>(const_cast<void*>(pointer));
                if (!task_raw) return WEECHAT_RC_ERROR;

                if (ret == WEECHAT_HOOK_PROCESS_RUNNING)
                {
                    if (out && *out)
                        task_raw->output += out;
                    return WEECHAT_RC_OK;
                }

                // Terminal call — take ownership for automatic cleanup
                auto task = std::unique_ptr<message_task>(task_raw);

                if (out && *out)
                    task->output += out;

                if (ret == 0)
                {
                    const std::string_view prefix = "content-type: ";
                    std::istringstream ss(task->output);
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
                        // Non-media URL: send plain message, skip the OOB probe
                        task->channel.send_message(
                                std::string_view(task->to), std::string_view(task->body),
                                /*skip_probe=*/true);
                    }
                }
                else
                {
                    // curl failed: send plain message, skip the OOB probe
                    task->channel.send_message(
                            std::string_view(task->to), std::string_view(task->body),
                            /*skip_probe=*/true);
                }
                // XEP-0511: link preview is sent by the recursive send_message()
                // call above (skip_probe=true path hits the URL scan loop).
                // Do NOT call send_link_preview() here — that would double-send.

                return WEECHAT_RC_OK;
            };
            struct t_hook *process_hook =
                weechat_hook_process_hashtable(command.data(), options, timeout,
                    callback, task_owned.release(), nullptr);
            weechat_hashtable_free(options);
            (void) process_hook;
            return WEECHAT_RC_OK;
        } while(0);
    }

    {
        auto active = stanza_node(account.context, "active",
                                  "http://jabber.org/protocol/chatstates");
        xmpp_stanza_add_child(message.get(), active.get());
    }

    // XEP-0184 §5.4: MUST NOT include <request/> in groupchat messages.
    // XEP-0333 §4.1: MUST NOT include <markable/> in groupchat messages.
    if (type != weechat::channel::chat_type::MUC)
    {
        {
            auto req = stanza_node(account.context, "request", "urn:xmpp:receipts");
            xmpp_stanza_add_child(message.get(), req.get());
        }
        {
            auto markable = stanza_node(account.context, "markable",
                                        "urn:xmpp:chat-markers:0");
            xmpp_stanza_add_child(message.get(), markable.get());
        }
    }

    {
        auto store = stanza_node(account.context, "store", "urn:xmpp:hints");
        xmpp_stanza_add_child(message.get(), store.get());
    }

    account.connection.send(message.get());

    // XEP-0511: Send outgoing link previews for URLs in the message body
    if (transport == weechat::channel::transport::PLAIN
            && weechat::config::instance
            && weechat::config::instance->look.outgoing_link_preview.boolean())
    {
        static const std::regex url_pattern("https?://[^ ]+");
        std::sregex_iterator it(body_str.begin(), body_str.end(), url_pattern);
        std::sregex_iterator end;
        for (; it != end; ++it)
        {
            std::string url = (*it)[0].str();
            send_link_preview(to_str, url);
        }
    }

    if (type != weechat::channel::chat_type::MUC)
    {
        auto *self_user = user::search(&account, account.jid().data());
        auto prefix = self_user ? std::string(self_user->as_prefix_raw()) : std::string(account.jid());
        const bool is_action = weechat_string_match(body_str.c_str(), "/me *", 0);
        std::string tag = fmt::format("xmpp_message,message,{}private,notify_none,self_msg,log1,id_{}",
            is_action ? "action," : "", saved_id);
        bool encrypted = (transport == weechat::channel::transport::OMEMO ||
                          transport == weechat::channel::transport::PGP);
        if (is_action)
        {
            weechat_printf_date_tags(buffer, 0,
                                     tag.c_str(),
                                     "%s%s %s%s ⌛",
                                     weechat_prefix("action"),
                                     prefix.data(),
                                     encrypted ? "🔒 " : "",
                                     body_str.c_str() + 4);
        }
        else
        {
            weechat_printf_date_tags(buffer, 0,
                                     tag.c_str(),
                                     "%s\t%s%s ⌛",
                                     prefix.data(),
                                     encrypted ? "🔒 " : "",
                                     body_str.c_str());
        }
    }

    return WEECHAT_RC_OK;
}

void weechat::channel::queue_pending_omemo_message(const std::string& body)
{
    if (body.empty())
        return;
    pending_omemo_messages.push_back(body);
}

void weechat::channel::flush_pending_omemo_messages()
{
    if (type != weechat::channel::chat_type::PM)
        return;

    if (flushing_pending_omemo)
        return;

    if (pending_omemo_messages.empty())
        return;

    flushing_pending_omemo = true;

    while (!pending_omemo_messages.empty())
    {
        std::string body = pending_omemo_messages.front();
        pending_omemo_messages.pop_front();

        const int rc = send_message(std::string_view(id), std::string_view(body), /*skip_probe=*/true);
        if (rc != WEECHAT_RC_OK)
        {
            // Put it back and stop; we'll retry on next session/bundle update.
            pending_omemo_messages.push_front(std::move(body));
            break;
        }
    }

    flushing_pending_omemo = false;
}

void weechat::channel::send_link_preview(const std::string& to, const std::string& url)
{
    struct t_hashtable *options = weechat_hashtable_new(8,
            WEECHAT_HASHTABLE_STRING, WEECHAT_HASHTABLE_STRING,
            nullptr, nullptr);
    if (!options) return;
    weechat_hashtable_set(options, "header", "1");
    // Full GET so we receive the HTML body for OpenGraph parsing
    const int timeout = 30000;

    struct link_preview_task {
        weechat::channel& channel;
        std::string to;
        std::string url;
        std::string output;
    };
    auto task_owned = std::make_unique<link_preview_task>(link_preview_task { *this, to, url });

    auto callback = [](const void *pointer, void *,
            const char *, int ret, const char *out, const char * /*err*/) {
        auto *task_raw = static_cast<link_preview_task*>(const_cast<void*>(pointer));
        if (!task_raw) return WEECHAT_RC_ERROR;

        if (ret == WEECHAT_HOOK_PROCESS_RUNNING)
        {
            if (out && *out)
                task_raw->output += out;
            return WEECHAT_RC_OK;
        }

        // Terminal call — take ownership for automatic cleanup
        auto task = std::unique_ptr<link_preview_task>(task_raw);

        if (out && *out)
            task->output += out;

        if (ret == 0 && !task->output.empty())
        {
            // Parse OpenGraph meta tags from HTML response.
            // We extract <meta property="og:PROP" content="VALUE"> from <head>.
            std::string html(task->output);
            std::string head_html;
            {
                auto head_end = html.find("</head>");
                if (head_end == std::string::npos)
                    head_end = std::min(html.size(), (size_t)8192);
                head_html = html.substr(0, head_end);
                std::transform(head_html.begin(), head_html.end(), head_html.begin(),
                        [](unsigned char c) { return std::tolower(c); });
            }

            // Extract content= value for a given og: property (from lowercased head).
            auto extract_og = [&](const std::string& prop) -> std::string {
                // Try property="og:PROP" and property='og:PROP'
                const char quotes[2] = {'"', '\''};
                for (int qi = 0; qi < 2; ++qi) {
                    char q = quotes[qi];
                    std::string needle = std::string("property=") + q + "og:" + prop + q;
                    auto pos = head_html.find(needle);
                    if (pos == std::string::npos) continue;
                    auto tag_start = head_html.rfind('<', pos);
                    if (tag_start == std::string::npos) continue;
                    auto tag_end = head_html.find('>', pos);
                    if (tag_end == std::string::npos) continue;
                    std::string tag_lower = head_html.substr(tag_start, tag_end - tag_start + 1);
                    std::string tag_orig  = html.substr(tag_start, tag_end - tag_start + 1);
                    // Find content= in the tag (double- or single-quoted value)
                    for (int cqi = 0; cqi < 2; ++cqi) {
                        char cq = quotes[cqi];
                        std::string cpfx = std::string("content=") + cq;
                        auto cpos = tag_lower.find(cpfx);
                        if (cpos == std::string::npos) continue;
                        cpos += cpfx.size();
                        auto cend = tag_orig.find(cq, cpos);
                        if (cend == std::string::npos) continue;
                        return tag_orig.substr(cpos, cend - cpos);
                    }
                }
                return {};
            };

            std::string og_title       = extract_og("title");
            std::string og_description = extract_og("description");
            std::string og_url         = extract_og("url");
            std::string og_image       = extract_og("image");

            // Fallback: use <title> when og:title is absent
            if (og_title.empty()) {
                // head_html is lowercased; offsets are identical to html up to head_end
                auto t0 = head_html.find("<title");
                if (t0 != std::string::npos) {
                    auto t1 = head_html.find('>', t0);          // end of opening tag
                    auto t2 = head_html.find("</title>", t0);   // closing tag
                    if (t1 != std::string::npos && t2 != std::string::npos && t2 > t1 + 1) {
                        // Use original-case slice from html (same offsets)
                        og_title = html.substr(t1 + 1, t2 - (t1 + 1));
                    }
                }
            }

            // Fallback: use the request URL when og:url is absent
            if (og_url.empty())
                og_url = task->url;

            // Only send a preview stanza if we got at least a title or url
            if (og_title.empty() && og_url.empty()) {
                return WEECHAT_RC_OK;
            }

            // Build follow-up <message> stanza with <rdf:Description> (XEP-0511)
            xmpp_ctx_t *ctx = task->channel.account.context;
            std::shared_ptr<xmpp_stanza_t> msg {
                xmpp_message_new(ctx,
                    task->channel.type == weechat::channel::chat_type::MUC
                    ? "groupchat" : "chat",
                    task->to.data(), nullptr),
                xmpp_stanza_release };

            std::string preview_id = stanza::uuid(ctx);
            xmpp_stanza_set_id(msg.get(), preview_id.c_str());

            // XEP-0511 §4.2: include an empty <body/> so clients that don't
            // understand XEP-0511 don't display an empty message bubble.
            xmpp_message_set_body(msg.get(), "");

            // <rdf:Description xmlns:rdf="..." xmlns:og="..." rdf:about="URL">
            // NOTE: xmpp_stanza_set_ns() sets the *default* namespace (xmlns=""),
            // which does NOT bind a prefix.  We must use explicit xmlns:rdf= and
            // xmlns:og= attributes so that Expat on the receiving end can resolve
            // the rdf: and og: prefixes used in element/attribute names.
            auto rdf = stanza_node(ctx, "rdf:Description", nullptr,
                {{"xmlns:rdf", "http://www.w3.org/1999/02/22-rdf-syntax-ns#"},
                 {"xmlns:og",  "https://ogp.me/ns#"},
                 {"rdf:about", task->url.data()}});

            auto add_og_child = [&](const char *elem_name, const std::string& text) {
                if (text.empty()) return;
                auto child = stanza_text_node(ctx, elem_name, text.data());
                xmpp_stanza_add_child(rdf.get(), child.get());
            };

            add_og_child("og:title",       og_title);
            add_og_child("og:description", og_description);
            add_og_child("og:url",         og_url);
            add_og_child("og:image",       og_image);

            xmpp_stanza_add_child(msg.get(), rdf.get());

            // XEP-0334: no-store + no-copy — metadata-only stanza, don't archive
            {
                auto no_store = stanza_node(ctx, "no-store", "urn:xmpp:hints");
                xmpp_stanza_add_child(msg.get(), no_store.get());
            }
            {
                auto no_copy = stanza_node(ctx, "no-copy", "urn:xmpp:hints");
                xmpp_stanza_add_child(msg.get(), no_copy.get());
            }

            task->channel.account.connection.send(msg.get());
        }

        return WEECHAT_RC_OK;
    };

    auto command = "url:" + url;
    struct t_hook *process_hook =
        weechat_hook_process_hashtable(command.data(), options, timeout,
                callback, task_owned.release(), nullptr);
    weechat_hashtable_free(options);
    (void) process_hook;
}

void weechat::channel::send_reads()
{
    // XEP-0333 §4.1: SHOULD NOT send Chat Markers to a MUC room.
    // Markers in MUC reveal presence to all participants and have no useful
    // effect.  Flush unreads silently so the queue doesn't grow unbounded.
    if (type == weechat::channel::chat_type::MUC)
    {
        unreads.clear();
        return;
    }

    auto i = std::begin(unreads);

    // Capture the last unread entry for XEP-0490 MDS PEP publish below
    std::string last_unread_id;
    std::string last_unread_stanza_id;
    std::string last_unread_stanza_id_by;
    if (!unreads.empty())
    {
        const auto &back = unreads.back();
        last_unread_id = back.id;
        if (back.stanza_id.has_value())
            last_unread_stanza_id = *back.stanza_id;
        if (back.stanza_id_by.has_value())
            last_unread_stanza_id_by = *back.stanza_id_by;
    }

    while (i != std::end(unreads))
    {
        auto* unread = &*i;

        // XEP-0333: <displayed> markers MUST use the correct message type
        // (chat for PM, groupchat for MUC) so the server routes them correctly.
        const char *marker_type = (this->type == weechat::channel::chat_type::MUC)
                                   ? "groupchat" : "chat";
        // XEP-0333 §4.3: In a MUC that supports XEP-0359, MUST use the
        // MUC-assigned stanza-id, NOT the sender's message id attribute.
        const std::string displayed_id =
            (this->type == weechat::channel::chat_type::MUC
             && unread->stanza_id.has_value())
            ? *unread->stanza_id : unread->id;
        stanza::message msg;
        msg.to(id).type(marker_type)
           .chat_marker_displayed(displayed_id)
           .no_store();  // XEP-0334: chat marker replies MUST NOT be stored
        if (unread->thread.has_value())
            msg.thread(*unread->thread);
        account.connection.send(msg.build(account.context).get());

        i = unreads.erase(i);
    }

    // XEP-0490: Message Displayed Synchronization
    // Publish to own PEP node so other devices know we've displayed up to
    // last_unread_id in this channel.  The item id is the peer's bare JID.
    //
    // <iq type='set'>
    //   <pubsub xmlns='http://jabber.org/protocol/pubsub'>
    //     <publish node='urn:xmpp:mds:displayed:0'>
    //       <item id='peer@example.org'>
    //         <displayed xmlns='urn:xmpp:mds:displayed:0'>
    //           <stanza-id xmlns='urn:xmpp:sid:0' by='peer@example.org'
    //                      id='last-stanza-id'/>
    //         </displayed>
    //       </item>
    //     </publish>
    //   </pubsub>
    // </iq>
    if (!last_unread_id.empty())
    {
        // XEP-0490: the <stanza-id> element in the MDS PEP publish MUST use:
        //   id  = server-assigned stanza-id (fall back to client id if unavailable)
        //   by  = the JID of the archiver (server domain or MUC JID) that
        //         assigned that stanza-id, NOT the peer's bare JID.
        // Derive server domain from account JID as fallback for `by`.
        const std::string server_domain = ::jid(nullptr, account.jid()).domain;
        const std::string mds_by_s  = !last_unread_stanza_id_by.empty()
                                        ? last_unread_stanza_id_by
                                        : (!server_domain.empty() ? server_domain : id);
        const std::string mds_sid_s = !last_unread_stanza_id.empty()
                                        ? last_unread_stanza_id
                                        : last_unread_id;

        // Build <stanza-id xmlns='urn:xmpp:sid:0' by='...' id='...'/>
        struct sid_spec : stanza::spec {
            sid_spec(const std::string& by, const std::string& sid) : spec("stanza-id") {
                attr("xmlns", "urn:xmpp:sid:0");
                attr("by", by);
                attr("id", sid);
            }
        } sid_s(mds_by_s, mds_sid_s);

        // Build <displayed xmlns='urn:xmpp:mds:displayed:0'>…</displayed>
        struct displayed_spec : stanza::spec {
            displayed_spec(sid_spec& sid) : spec("displayed") {
                attr("xmlns", "urn:xmpp:mds:displayed:0");
                child(sid);
            }
        } displayed_s(sid_s);

        // XEP-0490 §7: MUST publish with access_model=whitelist
        // Build the jabber:x:data form for publish-options
        struct field_spec : stanza::spec {
            field_spec(const char *var, const char *val, const char *type_attr = nullptr)
                : spec("field") {
                attr("var", var);
                if (type_attr) attr("type", type_attr);
                struct value_spec : stanza::spec {
                    value_spec(const char *v) : spec("value") { text(v); }
                } val_s(val);
                child(val_s);
            }
        };

        struct x_data_form : stanza::spec {
            x_data_form() : spec("x") {
                attr("xmlns", "jabber:x:data");
                attr("type", "submit");
                field_spec f1("FORM_TYPE",
                    "http://jabber.org/protocol/pubsub#publish-options", "hidden");
                field_spec f2("pubsub#persist_items", "true");
                field_spec f3("pubsub#max_items",     "max");
                field_spec f4("pubsub#send_last_published_item", "never");
                field_spec f5("pubsub#access_model",  "whitelist");
                child(f1); child(f2); child(f3); child(f4); child(f5);
            }
        } xform;

        stanza::xep0060::publish_options pub_opts;
        pub_opts.child_spec(xform);

        auto item_el = stanza::xep0060::item().id(id);
        item_el.payload(displayed_s);

        auto publish_el = stanza::xep0060::publish("urn:xmpp:mds:displayed:0");
        publish_el.item(item_el);

        auto iq_s = stanza::iq().type("set").id(stanza::uuid(account.context));
        static_cast<stanza::xep0060::iq&>(iq_s)
            .pubsub(stanza::xep0060::pubsub()
                .publish(publish_el)
                .publish_options(pub_opts));
        account.connection.send(iq_s.build(account.context).get());
    }
}

void weechat::channel::send_chat_state(weechat::user *user, const char *state)
{
    stanza::message msg;
    msg.to(user ? user->id : id)
       .type(type == weechat::channel::chat_type::MUC ? "groupchat" : "chat");
    msg.chatstate(state);
    msg.no_store();  // XEP-0334: don't store chat state notifications
    account.connection.send(msg.build(account.context).get());
}

void weechat::channel::send_active(weechat::user *user)
{
    if (!weechat::config::instance
            || !weechat::config::instance->look.send_chat_states.boolean())
        return;

    std::string to_jid = user ? std::string(user->id) : id;
    if (!is_chat_state_supported(to_jid)) return;

    auto &last = last_sent_chat_state[to_jid];
    if (last == "active") return;
    last = "active";

    send_chat_state(user, "active");
}

void weechat::channel::send_typing(weechat::user *user)
{
    if (!weechat::config::instance
            || !weechat::config::instance->look.send_chat_states.boolean())
        return;

    if (add_self_typing(user))
    {
        std::string to_jid = user ? std::string(user->id) : id;
        // Update dedup tracker: composing clears prior state so subsequent
        // active/paused/inactive sends are not suppressed.
        last_sent_chat_state[to_jid] = "composing";
        send_chat_state(user, "composing");
    }
}

void weechat::channel::send_paused(weechat::user *user)
{
    if (!weechat::config::instance
            || !weechat::config::instance->look.send_chat_states.boolean())
        return;

    std::string to_jid = user ? std::string(user->id) : id;
    if (!is_chat_state_supported(to_jid)) return;

    auto &last = last_sent_chat_state[to_jid];
    if (last == "paused") return;
    last = "paused";

    send_chat_state(user, "paused");
}

void weechat::channel::send_inactive(weechat::user *user)
{
    if (!weechat::config::instance
            || !weechat::config::instance->look.send_chat_states.boolean())
        return;

    std::string to_jid = user ? std::string(user->id) : id;
    if (!is_chat_state_supported(to_jid)) return;

    auto &last = last_sent_chat_state[to_jid];
    if (last == "inactive") return;
    last = "inactive";

    send_chat_state(user, "inactive");
}

void weechat::channel::send_gone(weechat::user *user)
{
    if (!weechat::config::instance
            || !weechat::config::instance->look.send_chat_states.boolean())
        return;

    // XEP-0085 §5.5: SHOULD NOT send <gone/> in a MUC
    if (type == weechat::channel::chat_type::MUC)
        return;

    std::string to_jid = user ? std::string(user->id) : id;
    if (!is_chat_state_supported(to_jid)) return;

    // XEP-0085 §5.2: don't send the same state twice in a row
    // Also clear tracking on 'gone' since the conversation ends — next
    // re-open starts fresh (any state is valid again).
    auto &last = last_sent_chat_state[to_jid];
    if (last == "gone") return;
    last_sent_chat_state.erase(to_jid);  // reset: conversation ended

    send_chat_state(user, "gone");
}

void weechat::channel::fetch_mam(const char *id, time_t *start, time_t *end, const char* after)
{
    std::string mam_id = id ? id : stanza::uuid(account.context);

    // Guard: avoid issuing overlapping top-level MAM queries for the same channel.
    // Overlapping fetches interleave <result> stanzas, which looks like duplicates
    // and out-of-order history in the buffer.
    if (!after)
    {
        for (const auto &[qid, q] : account.mam_queries)
        {
            if (!q.with.empty() && q.with == this->id)
            {
                XDEBUG("MAM: suppressing duplicate fetch for {} (query {} already in flight)",
                       this->id, qid);
                return;
            }
        }
    }

    // XEP-0313: MUC MAM is addressed to the room JID; PM MAM goes to the
    // user's own bare JID (personal archive), NOT the bare server domain.
    // Sending to the bare domain causes <service-unavailable/> errors.
    std::string mam_to;
    if (type == weechat::channel::chat_type::MUC)
    {
        mam_to = this->id;
    }
    else
    {
        mam_to = ::jid(nullptr, account.jid()).bare;
    }

    stanza::xep0313::x_filter xf;
    xf.with(this->id);
    if (start)
    {
        std::ostringstream oss;
        oss << std::put_time(gmtime(start), "%Y-%m-%dT%H:%M:%SZ");
        xf.start(oss.str());
    }
    if (end)
    {
        std::ostringstream oss;
        oss << std::put_time(gmtime(end), "%Y-%m-%dT%H:%M:%SZ");
        xf.end(oss.str());
    }

    stanza::xep0313::query q;
    q.queryid(mam_id).filter(xf);

    // Track this query so IQ <fin> and error handlers can map id -> context.
    account.add_mam_query(mam_id, this->id,
                          start ? std::optional(*start) : std::optional<time_t>(),
                          end ? std::optional(*end) : std::optional<time_t>());

    // RSM paging: send <after> token if provided.
    if (after)
    {
        stanza::xep0059::set rsm;
        rsm.after(after);
        q.rsm(rsm);
    }

    stanza::iq iq_s;
    iq_s.id(mam_id).type("set");
    if (!mam_to.empty()) iq_s.to(mam_to);
    iq_s.xep0313().query(q);
    account.connection.send(iq_s.build(account.context).get());
}
