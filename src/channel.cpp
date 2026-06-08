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
#include <ranges>
#include <algorithm>
#include <span>
#include <expected>
#include <iterator>
#include <strophe.h>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "color.hh"
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
#include "message.hh"
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
        if (!node.contains(':'))
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
    }
}

struct t_gui_buffer *weechat::channel::search_buffer(weechat::channel::chat_type /*type*/,
                                                     const char *name)
{
    std::string buffer_name = fmt::format("{}.{}" , account.name, name);
    struct t_gui_buffer *ptr_buffer = weechat_buffer_search("xmpp", buffer_name.c_str());
    if (ptr_buffer && weechat_buffer_get_pointer(ptr_buffer, "plugin") == weechat_plugin)
        return ptr_buffer;
    return nullptr;
}

struct t_gui_buffer *weechat::channel::create_buffer(weechat::channel::chat_type type,
                                                     const char *name)
{
    struct t_gui_buffer *ptr_buffer;
    int buffer_created;
    const char *short_name = nullptr;

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
                : channel_short_name(type, res ? res : name);
            weechat_buffer_set(ptr_buffer, "short_name", short_name_value.c_str());
        }
    }
    else
    {
        short_name = weechat_buffer_get_string(ptr_buffer, "short_name");

        if (!short_name)
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

    weechat_buffer_set(ptr_buffer, "input_multiline", "1");
    weechat_buffer_set(ptr_buffer, "localvar_set_type",
                       (type == weechat::channel::chat_type::PM) ? "query"
                     : (type == weechat::channel::chat_type::FEED) ? "feed"
                     : "channel");
    weechat_buffer_set(ptr_buffer, "localvar_set_server", account.name.data());

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

        // Only add account nick as a highlight word for non-MUC buffers
        // (MUC buffers use WeeChat's native nick highlighting). The feature
        // can be disabled via xmpp.look.highlight_words.
        if (type != weechat::channel::chat_type::MUC
            && weechat::config::instance
            && weechat::config::instance->look.highlight_words.boolean())
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
        if (auto muc_channel = account.channels.find(jid(account.context,
                                                                               std::string(id)).bare.data()); muc_channel != account.channels.end())
        {
            auto& [_, ch] = *muc_channel;
            int muc_num = weechat_buffer_get_integer(ch.buffer, "number");
            weechat_buffer_set(buffer, "number", fmt::format("{}", muc_num + 1).c_str());
        }
    }

    self_typing_hook_timer = nullptr;  // created lazily on first self-typing event

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
        else {
            time_t fetch_days = weechat::config::instance
                ? static_cast<time_t>(weechat::config::instance->look.mam_fetch_days.integer())
                : 3;
            start = now - (fetch_days * 86400);
        }
        
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

    if (!self_typing_hook_timer)
        self_typing_hook_timer = (struct t_hook *)weechat_hook_timer(
            1 * 1000, 0, 0, &weechat::channel::self_typing_cb,
            this, nullptr);

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
        account.pm_open_unregister(id);                // remove from tertiary restore list
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
    auto& [_, last] = *it;

    int delay_minutes = weechat::config::instance->look.smart_filter_delay.integer();
    time_t threshold = std::time(nullptr) - static_cast<time_t>(delay_minutes) * 60;
    return last < threshold;
}

void weechat::channel::mark_chat_state_supported(std::string_view from_jid)
{
    chat_state_supported.insert(std::string(from_jid));
}

bool weechat::channel::is_chat_state_supported(std::string_view to_jid) const
{
    // XEP-0085 §5.1: MUST NOT send chat state notifications to MUC rooms.
    if (type == weechat::channel::chat_type::MUC)
        return false;

    // For PM: only send if the contact has previously sent us a chat state,
    // or if the full JID (with resource) or bare JID is in the support set.
    if (chat_state_supported.contains(std::string(to_jid)))
        return true;

    // Also check bare JID
    const auto sep = to_jid.find('/');
    if (sep != std::string_view::npos)
    {
        if (chat_state_supported.contains(std::string(to_jid.substr(0, sep))))
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

void weechat::channel::apply_muc_info(const muc_info &incoming)
{
    // Disco#info refresh is authoritative for mode flags (XEP-0045 §6.4 lists
    // both positive and negative muc_* feature vars). Optional metadata and
    // anonymity are overwritten when the server supplies them.
    muc_info_.moderated    = incoming.moderated;
    muc_info_.members_only = incoming.members_only;
    muc_info_.persistent   = incoming.persistent;
    muc_info_.password     = incoming.password;
    muc_info_.hidden       = incoming.hidden;

    if (incoming.anon != muc_info::anonymity::unknown)
        muc_info_.anon = incoming.anon;

    if (incoming.description)       muc_info_.description       = incoming.description;
    if (incoming.language)          muc_info_.language          = incoming.language;
    if (incoming.subject)           muc_info_.subject           = incoming.subject;
    if (incoming.logs_url)          muc_info_.logs_url          = incoming.logs_url;
    if (incoming.occupants)         muc_info_.occupants         = incoming.occupants;
    if (incoming.max_users)         muc_info_.max_users         = incoming.max_users;
    if (!incoming.subject_modifiable)
        muc_info_.subject_modifiable = false;

    update_modes();
}

void weechat::channel::update_modes()
{
    if (type != chat_type::MUC || !buffer)
        return;

    // XEP-0045 §16.3 feature vars mapped to IRC-style mode letters. Negative
    // variants (muc_unmoderated, muc_open, muc_public, muc_temporary,
    // muc_unsecured) are defaults and never displayed.
    std::string s = "+";
    if (muc_info_.moderated)    s += 'm';
    if (muc_info_.members_only) s += 'i';
    if (muc_info_.password)     s += 'k';
    if (muc_info_.hidden)       s += 'p';
    if (muc_info_.persistent)   s += 'P';
    if (muc_info_.anon == muc_info::anonymity::nonanonymous)  s += 'N';
    else if (muc_info_.anon == muc_info::anonymity::semianonymous) s += 'S';

    if (s == "+")
        s.clear();

    weechat_buffer_set(buffer, "modes", s.c_str());
}

void weechat::channel::store_config_form(room_config_form form)
{
    form.fetched_at = ::time(nullptr);
    last_config_form = std::move(form);
}

void weechat::channel::prepare_room_config_submit(room_config_form &form)
{
    for (auto &f : form.fields)
    {
        if (f.type == "boolean" && f.values.empty())
            f.values = { "0" };
    }
}

bool weechat::channel::include_room_config_field_in_submit(
    const room_config_field &f)
{
    if (f.var.empty() || f.var == "FORM_TYPE" || f.type == "fixed")
        return false;
    return !f.values.empty();
}

std::optional<weechat::channel::member*> weechat::channel::add_member(const char *id, const char *client,
                                                                       std::optional<std::string_view> real_jid,
                                                                       weechat::user *known_user)
{
    weechat::channel::member *member;
    weechat::user *user;

    user = known_user ? known_user : user::search(&account, id);

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
        if (real_jid)
            new_member.real_jid = std::string(*real_jid);
        member = &new_member;
    }
    else
    {
        member = *member_opt;
        if (real_jid)
            member->real_jid = std::string(*real_jid);
        if (user)
            user->nicklist_remove(&account, this);
    }

    // docs/planning-muc-omemo.md §2.3: Central place — whenever a real_jid becomes
    // known for a MUC occupant (via presence, future admin affiliation results, etc.),
    // kick off their devicelist fetch so bundles can be requested next. The request
    // path is idempotent.
    if (real_jid && type == weechat::channel::chat_type::MUC && account.omemo)
    {
        account.omemo.request_axolotl_devicelist(account, *real_jid);
        inc_pending_omemo_bundles();  // §2.4: we will soon request bundles for this occupant
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
        ? "xmpp_presence,enter,log4,xmpp_smart_filter,no_trigger"
        : "xmpp_presence,enter,log4,no_trigger";

    std::string user_prefix = user->as_prefix_raw();

    if (weechat_strcasecmp(jid_bare, id) == 0
             && type == weechat::channel::chat_type::MUC)
    {
        std::string msg = fmt::format("{}{}{}{}{} {}{}{}{} {}{}{}{}{}{}{}{}{}{}{}{}{}{}",
                                      weechat_prefix("join"),
                                      user_prefix,
                                      client ? " (" : "",
                                      client ? client : "",
                                      client ? ")" : "",
                                      user->profile.status.has_value() ? "is " : "",
                                      weechat::xmpp_color("irc.color.message_join").c_str(),
                                      user->profile.status.has_value() ? user->profile.status->c_str() : (user->profile.idle.has_value() ? "idle" : "entered"),
                                      weechat::xmpp_color("reset").c_str(),
                                      id,
                                      user->profile.status_text.has_value() ? " [" : "",
                                      user->profile.status_text.has_value() ? user->profile.status_text->c_str() : "",
                                      user->profile.status_text.has_value() ? "]" : "",
                                      weechat::xmpp_color("yellow").c_str(), " as ", weechat::xmpp_color("reset").c_str(),
                                      user->profile.affiliation.has_value() ? user->profile.affiliation->c_str() : "",
                                      user->profile.affiliation.has_value() ? " " : "",
                                      user->profile.role.has_value() ? user->profile.role->c_str() : "",
                                      user->profile.pgp_id.has_value() ? weechat::xmpp_color("gray").c_str() : "",
                                      user->profile.pgp_id.has_value() ? " with PGP:" : "",
                                      user->profile.pgp_id.has_value() ? user->profile.pgp_id->c_str() : "",
                                      user->profile.pgp_id.has_value() ? weechat::xmpp_color("reset").c_str() : "");
        weechat_printf_date_tags(buffer, 0, enter_tags.c_str(), "%s", msg.c_str());
    }
    else
    {
        std::string msg = fmt::format("{}{} ({}) {}{}{}{} {}{}{}{}{}{}{}{}{}{}{}{}",
                                      weechat_prefix("join"),
                                      jid_resource ? user_prefix : "You",
                                      jid_resource ? jid_resource : user_prefix,
                                      user->profile.status.has_value() ? "is " : "",
                                      weechat::xmpp_color("irc.color.message_join").c_str(),
                                      user->profile.status.has_value() ? user->profile.status->c_str() : (user->profile.idle.has_value() ? "idle" : "entered"),
                                      weechat::xmpp_color("reset").c_str(),
                                      user->profile.idle.has_value() ? "since " : "",
                                      user->profile.idle.has_value() ? user->profile.idle->data() : "",
                                      user->profile.status_text.has_value() ? " [" : "",
                                      user->profile.status_text.has_value() ? user->profile.status_text->c_str() : "",
                                      user->profile.status_text.has_value() ? "]" : "",
                                      (user->profile.pgp_id.has_value() || user->profile.omemo) ? weechat::xmpp_color("gray").c_str() : "",
                                      (user->profile.pgp_id.has_value() || user->profile.omemo) ? " with " : "",
                                      user->profile.pgp_id.has_value() ? "PGP:" : "",
                                      user->profile.pgp_id.has_value() ? user->profile.pgp_id->c_str() : "",
                                      (user->profile.omemo && user->profile.pgp_id.has_value()) ? " and " : "",
                                      user->profile.omemo ? "OMEMO" : "",
                                      (user->profile.pgp_id.has_value() || user->profile.omemo) ? weechat::xmpp_color("reset").c_str() : "");
        weechat_printf_date_tags(buffer, 0, enter_tags.c_str(), "%s", msg.c_str());
    }

    return member;
}

std::optional<weechat::channel::member*> weechat::channel::member_search(const char *id)
{
    if (!id)
        return std::nullopt;

    for (auto& [_, m] : members)
    {
        if (weechat_strcasecmp(m.id.c_str(), id) == 0)
            return &m;
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
        ? "xmpp_presence,leave,log4,xmpp_smart_filter,no_trigger"
        : "xmpp_presence,leave,log4,no_trigger";

    if (weechat_strcasecmp(jid_bare, id) == 0
        && type == weechat::channel::chat_type::MUC)
    {
        std::string msg = fmt::format("{}{} {}left{} {} {}{}{}",
                                      weechat_prefix("quit"),
                                      jid_resource ? jid_resource : "",
                                      weechat::xmpp_color("irc.color.message_quit").c_str(),
                                      weechat::xmpp_color("reset").c_str(),
                                      id,
                                      reason ? "[" : "",
                                      reason ? reason : "",
                                      reason ? "]" : "");
        weechat_printf_date_tags(buffer, 0, leave_tags.c_str(), "%s", msg.c_str());
    }
    else
    {
        std::string msg = fmt::format("{}{} ({}) {}left{} {} {}{}{}",
                                      weechat_prefix("quit"),
                                      jid_bare ? jid_bare : "",
                                      jid_resource ? jid_resource : "",
                                      weechat::xmpp_color("irc.color.message_quit").c_str(),
                                      weechat::xmpp_color("reset").c_str(),
                                      id,
                                      reason ? "[" : "",
                                      reason ? reason : "",
                                      reason ? "]" : "");
        weechat_printf_date_tags(buffer, 0, leave_tags.c_str(), "%s", msg.c_str());
    }

    return member_opt;
}

std::string weechat::channel::find_member_by_nick(std::string_view nick) const
{
    for (const auto& [id, member_info] : members)
    {
        auto slash = id.rfind('/');
        if (slash != std::string::npos
            && slash + 1 < id.size()
            && weechat_strcasecmp(id.c_str() + slash + 1, std::string(nick).c_str()) == 0)
            return id;
    }
    return {};
}

bool weechat::channel::all_occupants_have_real_jid() const
{
    if (type != weechat::channel::chat_type::MUC)
        return true; // non-MUC: not applicable

    if (members.empty())
        return false; // no occupants known yet → treat as unsafe for OMEMO

    return std::ranges::all_of(members, [](const auto& p) {
        const auto& [_, m] = p;
        return m.real_jid.has_value() && !m.real_jid->empty();
    });
}

void weechat::channel::inc_pending_omemo_bundles()
{
    if (type == weechat::channel::chat_type::MUC)
    {
        ++omemo.pending_bundles;
        weechat_bar_item_update("xmpp_encryption");
    }
}

void weechat::channel::dec_pending_omemo_bundles()
{
    if (type == weechat::channel::chat_type::MUC && omemo.pending_bundles > 0)
    {
        --omemo.pending_bundles;
        weechat_bar_item_update("xmpp_encryption");
    }
}

std::string weechat::channel::omemo_status() const
{
    if (!omemo.enabled)
        return {};

    if (type == weechat::channel::chat_type::MUC && omemo.pending_bundles > 0)
    {
        return "🔒OMEMO (pending)";
    }

    return "🔒OMEMO";
}

// Helper for SFS/SIMS construction (shared with upload fd_cb fallback when no local
// channel entry exists at completion time, e.g. after /close or race). Builds the
// <message> with body + file-sharing (incl. esfs if present) + sims + oob + hints.
// Omemo wrapping (if any) and local printf are caller responsibility.
stanza::message weechat::channel::make_file_share_stanza(xmpp_ctx_t *xmpp_ctx,
    std::string_view to, const char *msg_type /*"chat" or "groupchat"*/,
    std::string_view saved_id,
    std::string_view body, std::string_view oob_url,
    const file_metadata& meta)
{
    (void)xmpp_ctx;
    stanza::message msg;
    msg.type(msg_type ? msg_type : "chat")
       .to(std::string(to))
       .id(saved_id)
       .origin_id(saved_id);

    // XEP-0385: SIMS + XEP-0066 OOB (legacy) + XEP-0447/0448 SFS/ESFS.
    {
        stanza::xep0447::file sfs_f;
        sfs_f.media_type(meta.content_type).name(meta.filename).size(meta.size);
        if (meta.width > 0 && meta.height > 0) sfs_f.width(meta.width).height(meta.height);
        if (!meta.file_date.empty()) sfs_f.date(meta.file_date);
        for (const auto& [algo, b64] : meta.hashes)
            sfs_f.add_hash(stanza::xep0447::hash(algo, b64));

        stanza::xep0447::file_sharing fs;
        fs.file(std::move(sfs_f));

        stanza::xep0447::sources os;
        if (meta.esfs) {
            stanza::xep0447::encrypted e;
            e.key(meta.esfs->key_b64).iv(meta.esfs->iv_b64);
            if (!meta.esfs->cipher_hash_b64.empty()) e.cipher_hash_sha256(meta.esfs->cipher_hash_b64);
            stanza::xep0447::sources isr;
            stanza::xep0447::url_data ud(oob_url);
            isr.add(std::move(ud));
            e.sources(isr);
            os.add(e);
        } else {
            stanza::xep0447::url_data ud(oob_url);
            os.add(std::move(ud));
        }
        fs.sources(os);

        msg.file_sharing(std::move(fs));

        // Fallback indication (XEP-0428)
        msg.fallback(stanza::xep0428::fallback("urn:xmpp:sfs:0", 0));

        // SIMS legacy (XEP-0385)
        stanza::xep0385::file sims_file;
        sims_file.media_type(meta.content_type)
                  .name(meta.filename)
                  .size(meta.size);
        if (meta.width > 0 && meta.height > 0)
            sims_file.width(meta.width).height(meta.height);
        if (!meta.file_date.empty()) sims_file.date(meta.file_date);
        for (const auto& [algo, b64] : meta.hashes)
            sims_file.add_hash(stanza::xep0385::hash(algo, b64));

        stanza::xep0385::sources sims_sources;
        sims_sources.add_source(oob_url);

        stanza::xep0385::media_sharing ms;
        ms.file(std::move(sims_file))
          .sources(std::move(sims_sources));

        stanza::xep0385::reference ref("0", std::to_string(body.size()));
        ref.media_sharing(std::move(ms));
        msg.sims_reference(std::move(ref));

        // OOB legacy (XEP-0066)
        msg.oob(stanza::xep0066::oob(oob_url));
    }

    msg.chatstate("active");

    // XEP-0184 §5.4: MUST NOT include <request/> in groupchat messages.
    // XEP-0333 §4.1: MUST NOT include <markable/> in groupchat messages.
    if (std::string(msg_type ? msg_type : "") != "groupchat")
    {
        msg.receipt_request().chat_marker_markable();
    }

    msg.store();

    // (mention references omitted in helper; added by caller if members available)

    return msg;
}

int weechat::channel::send_message(std::string to, std::string body,
                                   std::optional<std::string> oob,
                                   std::optional<file_metadata> file_meta)
{
    // Reuse the main send path for regular text messages so PM OMEMO logic
    // (auto-enable, capability gating, encode/decode behavior) stays consistent.
    if (!oob && !file_meta)
        return send_message(std::string_view(to), std::string_view(body), /*skip_probe=*/false);

    std::string saved_id = stanza::uuid(account.context);
    const char *mtype = (type == weechat::channel::chat_type::MUC ? "groupchat" : "chat");
    auto msg = make_file_share_stanza(account.context, to, mtype, saved_id, body,
                                      oob ? *oob : std::string{},
                                      file_meta ? *file_meta : file_metadata{});

    // (mention references are omitted from the helper to keep it ch-independent;
    // they are non-critical for file-share bodies which are URLs)

    // XEP-0384: OMEMO-encrypt the file-share stanza when the channel has OMEMO active.
    // This protects the ESFS key/IV XML children from eavesdroppers on the server.
    // For MUC, use encode_muc with real-JID recipients (like regular sends) so the
    // OMEMO header is correct for the room; otherwise the MUC may reject with
    // service-unavailable and no link/preview appears in the room.
    if (account.omemo && omemo.enabled)
    {
        constexpr const char *eme_namespace = "eu.siacs.conversations.axolotl";

        auto make_encrypted = [](xmpp_stanza_t *raw) -> std::shared_ptr<xmpp_stanza_t> {
            if (!raw) return {};
            return { raw, xmpp_stanza_release };
        };

        std::shared_ptr<xmpp_stanza_t> encrypted;
        if (type == weechat::channel::chat_type::MUC)
        {
            // Pre-checks (adapted from regular send; for upload we fallback instead of error
            // since the file is already uploaded).
            if (!all_occupants_have_real_jid())
            {
                // fall through to plaintext send with SFS (aesgcm body)
            }
            else if (omemo.pending_bundles > 0 || !account.omemo.pending_bundle_fetch.empty())
            {
                // fall through
            }
            else
            {
                std::vector<std::string> recipients;
                auto has_real = [](const auto& p) {
                    const auto& [_, m] = p;
                    return m.real_jid && !m.real_jid->empty();
                };
                auto extract = [](const auto& p) {
                    const auto& [_, m] = p;
                    return *m.real_jid;
                };
                std::ranges::copy(
                    members | std::views::filter(has_real) | std::views::transform(extract),
                    std::back_inserter(recipients)
                );
                const std::string own_bare = ::jid(nullptr, account.jid()).bare;
                if (!own_bare.empty())
                    recipients.push_back(own_bare);

                if (!recipients.empty())
                {
                    encrypted = make_encrypted(
                        account.omemo.encode_muc(&account, buffer, to,
                                                 recipients, body.c_str()));
                }
            }
        }
        if (!encrypted)
        {
            // PM or MUC fallback
            encrypted = make_encrypted(
                account.omemo.encode(&account, buffer, to, body));
        }

        if (encrypted)
        {
            msg.child(encrypted);
            msg.eme(stanza::xep0380::eme(eme_namespace));
            msg.body(OMEMO_ADVICE);
            set_transport(weechat::channel::transport::OMEMO, 0);

            // Cache the plaintext (URL) so MAM replay can display it later.
            std::string peer_bare(to);
            if (auto s = peer_bare.find('/'); s != std::string::npos)
                peer_bare.resize(s);
            account.mam_cache_store_omemo_plaintext(peer_bare, saved_id, body);
        }
        else
        {
            // If encode fails (keys not yet fetched), send plaintext as fallback.
            // The upload has already happened; we can't retry it. The user will
            // need to re-send after OMEMO device/bundle exchange completes.
            msg.body(body);
        }
    }
    else
    {
        msg.body(body);
    }

    auto msg_sp = msg.build(account.context);
    account.connection.send(msg_sp.get());
    if (type != weechat::channel::chat_type::MUC)
    {
        auto *self_user = user::search(&account, account.jid().data());
        auto prefix = self_user ? std::string(self_user->as_prefix_raw()) : std::string(account.jid());
        std::string tag = "xmpp_message,message,private,notify_none,self_msg,log1,id_" + saved_id;
        std::string display_body = weechat::config::instance
            && weechat::config::instance->look.emoticons.boolean()
            ? replace_emoticons(body)
            : std::string(body);
        weechat_printf_date_tags(buffer, 0,
                                 tag.c_str(),
                                 "%s\t%s ⌛",
                                 prefix.data(),
                                 display_body.c_str());
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

    std::string saved_id = stanza::uuid(account.context);

    stanza::message msg;
    msg.type(type == weechat::channel::chat_type::MUC
             ? "groupchat" : "chat")
       .to(to_str)
       .id(saved_id)
       .origin_id(saved_id);

    // XEP-0045 §7.5: for MUC private messages (chat to an occupant JID
    // room@service/nick), add <x xmlns='…muc#user'/> so that XEP-0280
    // Message Carbons can correctly synchronise the message to other clients.
    if (type == weechat::channel::chat_type::PM
        && to_str.contains('/')
        && account.channels.contains(peer_bare)
        && account.channels.at(peer_bare).type == weechat::channel::chat_type::MUC)
    {
        msg.muc_user(stanza::xep0045::muc_user_x());
    }

    // OMEMO for MUCs requires visible real JIDs for all occupants (non-anonymous room)
    // and no pending bundle fetches.
    if (account.omemo && omemo.enabled && type == weechat::channel::chat_type::MUC &&
        !all_occupants_have_real_jid())
    {
        weechat_printf_date_tags(buffer, 0, "notify_none",
            "%s", fmt::format("{}: OMEMO requires a non-anonymous room (real JIDs visible for all occupants)",
                              weechat_prefix("error")).c_str());
        return WEECHAT_RC_ERROR;
    }

    // docs/planning-muc-omemo.md §2.4: Block MUC OMEMO sends while bundles are
    // still being fetched for occupants (Option B). Prefer per-channel counter.
    if (account.omemo && omemo.enabled && type == weechat::channel::chat_type::MUC &&
        (omemo.pending_bundles > 0 || !account.omemo.pending_bundle_fetch.empty()))
    {
        weechat_printf_date_tags(buffer, 0, "notify_none",
            "%s", fmt::format("{}: OMEMO not ready yet (fetching occupant bundles… {} pending)",
                              weechat_prefix("error"), omemo.pending_bundles).c_str());
        return WEECHAT_RC_ERROR;
    }

    if (account.omemo && omemo.enabled)
    {
        std::shared_ptr<xmpp_stanza_t> encrypted;
        constexpr const char *eme_namespace = "eu.siacs.conversations.axolotl";

        auto make_encrypted = [](xmpp_stanza_t *raw) -> std::shared_ptr<xmpp_stanza_t> {
            if (!raw) return {};
            return { raw, xmpp_stanza_release };
        };

        if (type == weechat::channel::chat_type::MUC)
        {
            std::vector<std::string> recipients;
            auto has_real = [](const auto& p) {
                const auto& [_, m] = p;
                return m.real_jid && !m.real_jid->empty();
            };
            auto extract = [](const auto& p) { 
                const auto& [_, m] = p; 
                return *m.real_jid; 
            };

            std::ranges::copy(
                members | std::views::filter(has_real) | std::views::transform(extract),
                std::back_inserter(recipients)
            );
            const std::string own_bare = ::jid(nullptr, account.jid()).bare;
            if (!own_bare.empty())
                recipients.push_back(own_bare);

            encrypted = make_encrypted(
                account.omemo.encode_muc(&account, buffer, id,
                                         recipients, body_str.c_str()));
        }
        else
        {
            encrypted = make_encrypted(
                account.omemo.encode(&account, buffer, to_str, body_str));
        }

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
        msg.child(encrypted);
        msg.eme(stanza::xep0380::eme(eme_namespace));
        msg.body(OMEMO_ADVICE);
        set_transport(weechat::channel::transport::OMEMO, 0);

        XDEBUG("omemo cache store: peer_bare={} saved_id={} body_len={}",
               peer_bare, saved_id, body_str.size());
        account.mam_cache_store_omemo_plaintext(peer_bare, saved_id, body_str);
    }
    else if (pgp.enabled && !pgp.ids.empty())
    {
        auto ciphertext = account.pgp.encrypt(buffer, account.pgp_keyid().data(),
            std::vector(pgp.ids.begin(), pgp.ids.end()), body_str.c_str());
        if (ciphertext)
        {
            msg.pgp_encrypted(stanza::xep0027::encrypted(ciphertext->c_str()));
        }
        else
        {
            weechat_printf_date_tags(buffer, 0, "notify_none", "%s%s",
                                     weechat_prefix("error"), "PGP Error");
            set_transport(weechat::channel::transport::PLAIN, 1);
            return WEECHAT_RC_ERROR;
        }

        msg.eme(stanza::xep0380::eme("jabber:x:encrypted"));
        msg.body(weechat::xmpp::PGP_ADVICE);
        set_transport(weechat::channel::transport::PGP, 0);
    }
    else
    {
        msg.body(body_str);
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
            const std::string command = fmt::format("url:{}", url);
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

                auto task = std::unique_ptr<message_task>(task_raw);

                if (out && *out)
                    task->output += out;

                if (ret == 0)
                {
                    const std::string_view prefix = "content-type: ";
                    std::istringstream ss(task->output);
                    std::string line, mime;
                    while (std::getline(ss, line)) {
                        std::ranges::transform(line, line.begin(),
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
                                weechat::xmpp_color("gray").c_str(), mime.data());
                        task->channel.send_message(task->to, task->body, { task->url });
                    }
                    else
                    {
                        task->channel.send_message(
                                std::string_view(task->to), std::string_view(task->body),
                                /*skip_probe=*/true);
                    }
                }
                else
                {
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

    msg.chatstate("active");

    if (type != weechat::channel::chat_type::MUC)
    {
        msg.receipt_request().chat_marker_markable();
    }

    msg.store();

    auto msg_sp = msg.build(account.context);
    account.connection.send(msg_sp.get());

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
        std::string display_body = weechat::config::instance
            && weechat::config::instance->look.emoticons.boolean()
            ? replace_emoticons(body_str)
            : body_str;
        if (is_action)
        {
            weechat_printf_date_tags(buffer, 0,
                                     tag.c_str(),
                                     "%s%s %s%s ⌛",
                                     weechat_prefix("action"),
                                     prefix.data(),
                                     encrypted ? "🔒 " : "",
                                     display_body.c_str() + 4);
        }
        else
        {
            weechat_printf_date_tags(buffer, 0,
                                     tag.c_str(),
                                     "%s\t%s%s ⌛",
                                     prefix.data(),
                                     encrypted ? "🔒 " : "",
                                     display_body.c_str());
        }
    }

    return WEECHAT_RC_OK;
}

void weechat::channel::queue_pending_omemo_message(std::string_view body)
{
    if (body.empty())
        return;
    pending_omemo_messages.push_back(std::string(body));
}

void weechat::channel::flush_pending_omemo_messages()
{
    // docs/planning-muc-omemo.md §5.2: For MUCs we intentionally use Option B
    // (block) in v1 rather than a full pending queue, because queuing across
    // many occupants with partial bundle failures is complex. The queue
    // mechanism remains PM-only for now.
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

void weechat::channel::send_link_preview(std::string_view to, std::string_view url)
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
    auto task_owned = std::make_unique<link_preview_task>(
        link_preview_task { *this, std::string(to), std::string(url) });

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
                std::ranges::transform(head_html, head_html.begin(),
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
            std::string preview_id = stanza::uuid(ctx);

            stanza::message msg;
            msg.type(task->channel.type == weechat::channel::chat_type::MUC
                     ? "groupchat" : "chat")
               .to(task->to.data())
               .id(preview_id)
               .body("")
               .no_store()
               .no_copy();

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

            msg.child(rdf);

            auto msg_sp = msg.build(ctx);
            task->channel.account.connection.send(msg_sp.get());
        }

        return WEECHAT_RC_OK;
    };

    const std::string command = fmt::format("url:{}", url);
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

        // Global concurrency limiter for initial sync / bulk joins.
        if (!account.try_acquire_mam_slot())
        {
            if (account.mam_inflight == 0)
            {
                // Very first MAM request for this account in the current session
                // (global or first room) — always let it through so MAM visibly
                // starts on connect.
                account.mam_inflight = 1;
            }
            else
            {
                // Defer using the existing mechanism.
                account.mam_deferred_pages.push_back({
                    this->id,
                    std::string(id ? id : ""),
                    start ? std::optional(*start) : std::optional<time_t>(),
                    end   ? std::optional(*end)   : std::optional<time_t>(),
                    ""   // initial request (no RSM after token yet)
                });
                account.schedule_next_mam_page();
                return;
            }
        }
    }

    // Print a "Fetching history from X to Y" banner on the initial fetch only
    // (not on RSM continuation pages).  This mirrors what Profanity and Gajim do
    // to let the user see which time range is being loaded from the server archive.
    if (!after && buffer)
    {
        char start_str[32] = "the beginning";
        char end_str[32]   = "now";
        if (start)
        {
            struct tm *lt = localtime(start);
            strftime(start_str, sizeof(start_str), "%Y-%m-%d %H:%M", lt);
        }
        if (end)
        {
            struct tm *lt = localtime(end);
            strftime(end_str, sizeof(end_str), "%Y-%m-%d %H:%M", lt);
        }
        weechat_printf_date_tags(buffer, 0, "xmpp_mam_fetch,notify_none,no_log",
                                 "%sFetching history: %s → %s",
                                 weechat_prefix("network"),
                                 start_str, end_str);
    }

    // XEP-0313: MUC MAM is addressed to the room JID.
    // PM MAM (personal archive): omit the 'to' attribute entirely.
    // Sending to the user's bare JID (user@domain) would cause the server to
    // fan-out the IQ to ALL connected resources (Gajim, Conversations, etc.),
    // triggering a MAM catchup on every other active client on the account.
    // With no 'to', the server routes the IQ only to the requesting resource's
    // full JID and processes it against the user's personal archive — identical
    // behaviour to the global MAM query in connect_lifecycle.inl.
    std::string mam_to;
    if (type == weechat::channel::chat_type::MUC)
    {
        mam_to = this->id;
    }
    // else: PM — leave mam_to empty; iq_s.to() is only called when non-empty.

    stanza::xep0313::x_filter xf;
    // XEP-0313 §4: In a MUC archive, <with> filters by occupant publisher JID —
    // not the room JID itself.  Setting <with> to the room JID would ask the server
    // to return only messages sent *by the room JID as an occupant*, which is
    // nonsensical and returns zero results.  For MUC, the IQ is already addressed
    // to the room via the 'to' attribute; omit <with> entirely.
    // For PM (personal archive), <with> correctly filters messages exchanged with
    // a specific peer JID.
    if (type != weechat::channel::chat_type::MUC)
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
    stanza::xep0059::set rsm;
    rsm.max(50);
    if (after)
        rsm.after(after);
    q.rsm(rsm);

    stanza::iq iq_s;
    iq_s.id(mam_id).type("set");
    if (!mam_to.empty()) iq_s.to(mam_to);
    iq_s.xep0313().query(q);
    account.connection.send(iq_s.build(account.context).get());
}
