// This->Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <stdexcept>
#include <optional>
#include <time.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <curl/curl.h>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>
#include <fmt/core.h>
#include <fmt/chrono.h>
#include <libxml/uri.h>
#include <utility>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "xmpp/node.hh"
#include "xmpp/stanza.hh"
#include "config.hh"
#include "account.hh"
#include "user.hh"
#include "channel.hh"
#include "connection.hh"
#include "omemo.hh"
#include "pgp.hh"
#include "util.hh"

extern "C" {
#include "diff/diff.h"
}

void weechat::connection::init()
{
    srand(time(NULL));
    libstrophe::initialize();
}

void weechat::connection::send(xmpp_stanza_t *stanza)
{
    // Increment outbound counter for actual stanzas (not SM elements)
    const char *name = xmpp_stanza_get_name(stanza);
    if (name && account.sm_enabled)
    {
        std::string stanza_name(name);
        if (stanza_name == "message" || stanza_name == "presence" || stanza_name == "iq")
        {
            account.sm_h_outbound++;
        }
    }
    m_conn.send(stanza);
}

bool weechat::connection::version_handler(xmpp_stanza_t *stanza)
{
    const char *weechat_name = "weechat";
    std::unique_ptr<char> weechat_version(weechat_info_get("version", NULL));

    weechat_printf(NULL, "Received version request from %s", xmpp_stanza_get_from(stanza));

    auto reply = libstrophe::stanza::reply(stanza)
        .set_type("result");

    auto query = libstrophe::stanza(account.context)
        .set_name("query");
    if (const char *ns = xmpp_stanza_get_ns(xmpp_stanza_get_children(stanza)); ns) {
        query.set_ns(ns);
    }

    query.add_child(libstrophe::stanza(account.context)
                    .set_name("name")
                    .add_child(libstrophe::stanza(account.context)
                               .set_text(weechat_name)));
    query.add_child(libstrophe::stanza(account.context)
                    .set_name("version")
                    .add_child(libstrophe::stanza(account.context)
                               .set_text(weechat_version.get())));

    reply.add_child(query);

    account.connection.send(reply);

    return true;
}

bool weechat::connection::time_handler(xmpp_stanza_t *stanza)
{
    weechat_printf(NULL, "Received time request from %s", xmpp_stanza_get_from(stanza));

    auto reply = libstrophe::stanza::reply(stanza)
        .set_type("result");

    auto query = libstrophe::stanza(account.context)
        .set_name("time");
    if (const char *ns = xmpp_stanza_get_ns(xmpp_stanza_get_children(stanza)); ns) {
        query.set_ns(ns);
    }

    // Get current time
    time_t now = time(NULL);
    struct tm *tm_utc = gmtime(&now);
    struct tm *tm_local = localtime(&now);
    
    // Format UTC time as ISO 8601: YYYY-MM-DDTHH:MM:SSZ
    char utc_str[32];
    strftime(utc_str, sizeof(utc_str), "%Y-%m-%dT%H:%M:%SZ", tm_utc);
    
    // Calculate timezone offset
    long tz_offset = tm_local->tm_gmtoff;  // Offset in seconds
    int tz_hours = tz_offset / 3600;
    int tz_mins = abs((tz_offset % 3600) / 60);
    char tzo_str[16];
    snprintf(tzo_str, sizeof(tzo_str), "%+03d:%02d", tz_hours, tz_mins);

    query.add_child(libstrophe::stanza(account.context)
                    .set_name("utc")
                    .add_child(libstrophe::stanza(account.context)
                               .set_text(utc_str)));
    query.add_child(libstrophe::stanza(account.context)
                    .set_name("tzo")
                    .add_child(libstrophe::stanza(account.context)
                               .set_text(tzo_str)));

    reply.add_child(query);

    account.connection.send(reply);

    return true;
}

bool weechat::connection::presence_handler(xmpp_stanza_t *stanza, bool /* top_level */)
{
    // SM counter incremented in libstrophe wrapper, not here
    // top_level parameter kept for nested/recursive calls

    weechat::user *user;
    weechat::channel *channel;

    auto binding = xml::presence(account.context, stanza);
    if (!binding.from)
        return 1;

    std::string clientid;
    if (auto caps = binding.capabilities())
    {
        auto node = caps->node;
        auto ver = caps->verification;

        clientid = fmt::format("{}#{}", node, ver);
        
        // Check if we have this capability hash cached (XEP-0115)
        std::vector<std::string> cached_features;
        if (account.caps_cache_get(ver, cached_features))
        {
            // We have cached features, no need to query
            // Could use cached_features here if needed
        }
        else
        {
            // Not cached, send disco query and mark for caching
            char *disco_id = xmpp_uuid_gen(account.context);
            account.caps_disco_queries[disco_id] = ver;  // Track this query for caching
            
            std::string to_jid = binding.from ? binding.from->full : std::string();

            account.connection.send(stanza::iq()
                        .from(binding.to ? binding.to->full : "")
                        .to(to_jid)
                        .type("get")
                        .id(disco_id)
                        .xep0030()
                        .query()
                        .build(account.context)
                        .get());
            
            xmpp_free(account.context, disco_id);
        }
    }

    channel = account.channels.contains(binding.from->bare.data())
        ? &account.channels.find(binding.from->bare.data())->second : nullptr;
    
    // Note: We don't auto-create PM channels from presence anymore.
    // PM channels are only created when:
    // - User explicitly opens with /query
    // - We receive a message from them
    // This allows roster contacts to appear in account nicklist instead of creating buffers

    if (binding.type && *binding.type == "error" && channel)
    {
        if (auto error = binding.error())
        {
            const char *error_reason = error->reason();
            const char *from_str = binding.from ? binding.from->full.data() : "unknown";
            
            // Only log if the error has meaningful information
            // Skip generic "Unspecified" errors that don't have useful context
            if (strcmp(error_reason, "Unspecified") != 0 || error->description)
            {
                weechat_printf(channel->buffer, "[!]\t%s%sError: %s%s%s%s",
                               weechat_color("gray"),
                               binding.muc() ? "MUC " : "",
                               error_reason,
                               error->description ? " (" : "",
                               error->description ? error->description->data() : "",
                               error->description ? ")" : "");
            }
            else
            {
                // Debug: log unspecified errors with JID
                weechat_printf(account.buffer, "%s[DEBUG] Received unspecified error from %s (presence)",
                              weechat_prefix("network"), from_str);
            }
        }
        return 1;
    }

    if (auto x = binding.muc_user())
    {
        for (int& status : x->statuses)
        {
            switch (status)
            {
                case 100: // Non-Anonymous: [message | Entering a room]: Inform user that any occupant is allowed to see the user's full JID
                    if (channel)
                        weechat_buffer_set(channel->buffer, "notify", "2");
                    break;
                case 101: // : [message (out of band) | Affiliation change]: Inform user that his or her affiliation changed while not in the room
                    break;
                case 102: // : [message | Configuration change]: Inform occupants that room now shows unavailable members
                    break;
                case 103: // : [message | Configuration change]: Inform occupants that room now does not show unavailable members
                    break;
                case 104: // : [message | Configuration change]: Inform occupants that a non-privacy-related room configuration change has occurred
                    break;
                case 110: // Self-Presence: [presence | Any room presence]: Inform user that presence refers to one of its own room occupants
                    break;
                case 170: // Logging Active: [message or initial presence | Configuration change]: Inform occupants that room logging is now enabled
                    break;
                case 171: // : [message | Configuration change]: Inform occupants that room logging is now disabled
                    break;
                case 172: // : [message | Configuration change]: Inform occupants that the room is now non-anonymous
                    break;
                case 173: // : [message | Configuration change]: Inform occupants that the room is now semi-anonymous
                    break;
                case 174: // : [message | Configuration change]: Inform occupants that the room is now fully-anonymous
                    break;
                case 201: // : [presence | Entering a room]: Inform user that a new room has been created
                    break;
                case 210: // Nick Modified: [presence | Entering a room]: Inform user that the service has assigned or modified the occupant's roomnick
                    break;
                case 301: // : [presence | Removal from room]: Inform user that he or she has been banned from the room
                    weechat_printf(channel->buffer, "[!]\t%sBanned from Room", weechat_color("gray"));
                    break;
                case 303: // : [presence | Exiting a room]: Inform all occupants of new room nickname
                    break;
                case 307: // : [presence | Removal from room]: Inform user that he or she has been kicked from the room
                    weechat_printf(channel->buffer, "[!]\t%sKicked from room", weechat_color("gray"));
                    break;
                case 321: // : [presence | Removal from room]: Inform user that he or she is being removed from the room because of an affiliation change
                    weechat_printf(channel->buffer, "[!]\t%sRoom Affiliation changed, kicked", weechat_color("gray"));
                    break;
                case 322: // : [presence | Removal from room]: Inform user that he or she is being removed from the room because the room has been changed to members-only and the user is not a member
                    weechat_printf(channel->buffer, "[!]\t%sRoom now members-only, kicked", weechat_color("gray"));
                    break;
                case 332: // : [presence | Removal from room]: Inform user that he or she is being removed from the room because of a system shutdown
                    weechat_printf(channel->buffer, "[!]\t%sRoom Shutdown", weechat_color("gray"));
                    break;
                default:
                    break;
            }
        }

        for (auto& item : x->items)
        {
            using xml::xep0045;

            std::string role(item.role ? xep0045::format_role(*item.role) : "");
            std::string affiliation(item.affiliation ? xep0045::format_affiliation(*item.affiliation) : "");
            std::string jid = item.target ? item.target->full : clientid;

            user = weechat::user::search(&account, binding.from->full.data());
            if (!user)
            {
                auto name = binding.from->full.data();
                user = &account.users.emplace(std::piecewise_construct,
                                              std::forward_as_tuple(name),
                                              std::forward_as_tuple(&account, channel, name,
                                                                    channel && binding.from->bare.data() == channel->id
                                                                    ? (binding.from->resource.size() ? binding.from->resource.data() : "")
                                                                    : binding.from->full.data())).first->second;
            }
            auto status = binding.status();
            auto show = binding.show();
            auto idle = binding.idle_since();
            user->profile.status_text = status ? strdup(status->data()) : NULL;
            user->profile.status = show ? strdup(show->data()) : NULL;
            user->profile.idle = idle ? fmt::format("{}", *idle) : std::string();
            user->is_away = show ? *show == "away" : false;
            user->profile.role = role.size() ? strdup(role.data()) : NULL;
            user->profile.affiliation = affiliation.size() && affiliation == "none"
                ? strdup(affiliation.data()) : NULL;
            if (channel)
            {
                if (auto signature = binding.signature())
                {
                    user->profile.pgp_id = account.pgp.verify(channel->buffer, signature->data());
                    if (channel->type != weechat::channel::chat_type::MUC)
                        channel->pgp.ids.emplace(user->profile.pgp_id);
                }

                if (weechat_strcasecmp(role.data(), "none") == 0)
                    channel->remove_member(binding.from->full.data(), status ? status->data() : nullptr);
                else
                    channel->add_member(binding.from->full.data(), jid.data());
            }
        }
    }
    else
    {
        user = user::search(&account, binding.from->full.data());
        if (!user)
        {
            auto name = binding.from->full.data();
            user = &account.users.emplace(std::piecewise_construct,
                                          std::forward_as_tuple(name),
                                          std::forward_as_tuple(&account, channel, name,
                                                                channel && binding.from->bare.data() == channel->id
                                                                ? (binding.from->resource.size() ? binding.from->resource.data() : "")
                                                                : binding.from->full.data())).first->second;
        }
        auto status = binding.status();
        auto show = binding.show();
        auto idle = binding.idle_since();
        user->profile.status_text = status ? strdup(status->data()) : NULL;
        user->profile.status = show ? strdup(show->data()) : NULL;
        user->profile.idle = idle ? fmt::format("{}", *idle) : std::string();
        user->is_away = show ? *show == "away" : false;
        user->profile.role = NULL;
        user->profile.affiliation = NULL;
        
        // For roster contacts (not in a MUC), manage account buffer nicklist
        if (!channel)
        {
            if (binding.type && *binding.type == "unavailable")
            {
                // User went offline, remove from account nicklist
                user->nicklist_remove(&account, nullptr);
            }
            else
            {
                // User is online (or status update), add/update in account nicklist
                user->nicklist_remove(&account, nullptr);  // Remove first to avoid duplicates
                user->nicklist_add(&account, nullptr);
            }
        }
        
        if (channel)
        {
            if (auto signature = binding.signature(); signature)
            {
                user->profile.pgp_id = account.pgp.verify(channel->buffer, signature->data());
                if (channel->type != weechat::channel::chat_type::MUC)
                    channel->pgp.ids.emplace(user->profile.pgp_id);
            }

            if (user->profile.role)
                channel->remove_member(binding.from->full.data(), status ? status->data() : nullptr);
            else
                channel->add_member(binding.from->full.data(), clientid.data());
        }
    }

    return true;
}

bool weechat::connection::message_handler(xmpp_stanza_t *stanza, bool /* top_level */)
{
    // SM counter incremented in libstrophe wrapper, not here
    // top_level parameter kept for nested/recursive calls

    weechat::channel *channel, *parent_channel;
    xmpp_stanza_t *x, *body, *delay, *topic, *replace, *request, *markable, *composing, *sent, *received, *result, *forwarded, *event, *items, *item, *list, *device, *encrypted;
    const char *type, *from, *nick, *from_bare, *to, *to_bare, *id, *thread, *replace_id, *timestamp;
    char *text, *intext, *difftext = NULL, *cleartext = NULL;
    struct tm time = {0};
    time_t date = 0;

    auto binding = xml::message(account.context, stanza);
    body = xmpp_stanza_get_child_by_name(stanza, "body");
    if (body == NULL)
    {
        topic = xmpp_stanza_get_child_by_name(stanza, "subject");
        if (topic != NULL)
        {
            intext = xmpp_stanza_get_text(topic);
            type = xmpp_stanza_get_type(stanza);
            if (type != NULL && strcmp(type, "error") == 0)
                return 1;
            from = xmpp_stanza_get_from(stanza);
            if (from == NULL)
                return 1;
            from_bare = xmpp_jid_bare(account.context, from);
            from = xmpp_jid_resource(account.context, from);
            channel = account.channels.contains(from_bare)
                ? &account.channels.find(from_bare)->second : nullptr;
            if (!channel)
            {
                if (weechat_strcasecmp(type, "groupchat") == 0)
                {
                    channel = &account.channels.emplace(
                        std::make_pair(from_bare, weechat::channel {
                                account, weechat::channel::chat_type::MUC, from_bare, from_bare
                            })).first->second;
                }
                else
                {
                    channel = &account.channels.emplace(
                        std::make_pair(from_bare, weechat::channel {
                                account, weechat::channel::chat_type::PM, from_bare, from_bare
                            })).first->second;
                }
            }
            channel->update_topic(intext ? intext : "", from, 0);
            if (intext != NULL)
                xmpp_free(account.context, intext);
        }

        // XEP-0085: Chat State Notifications - handle all states
        composing = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "composing", "http://jabber.org/protocol/chatstates");
        xmpp_stanza_t *paused = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "paused", "http://jabber.org/protocol/chatstates");
        xmpp_stanza_t *active = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "active", "http://jabber.org/protocol/chatstates");
        xmpp_stanza_t *inactive = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "inactive", "http://jabber.org/protocol/chatstates");
        xmpp_stanza_t *gone = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "gone", "http://jabber.org/protocol/chatstates");
        
        if (composing || paused || active || inactive || gone)
        {
            from = xmpp_stanza_get_from(stanza);
            if (from == NULL)
                return 1;
            from_bare = xmpp_jid_bare(account.context, from);
            nick = xmpp_jid_resource(account.context, from);
            channel = account.channels.contains(from_bare)
                ? &account.channels.find(from_bare)->second : nullptr;
            if (!channel)
                return 1;
            auto user = user::search(&account, from);
            if (!user)
            {
                auto name = from;
                user = &account.users.emplace(std::piecewise_construct,
                                              std::forward_as_tuple(name),
                                              std::forward_as_tuple(&account, channel, name,
                                                                    weechat_strcasecmp(from_bare, channel->id.data()) == 0
                                                                    ? nick : from)).first->second;
            }
            
            // For PMs, always use bare JID; for MUCs use nick
            const char *display_name = channel->type == weechat::channel::chat_type::MUC 
                ? nick 
                : from_bare;
            
            if (composing)
            {
                channel->add_typing(user);
                weechat_printf(channel->buffer, "...\t%s%s is typing...",
                               weechat_color("gray"),
                               display_name);
            }
            else if (paused)
            {
                channel->add_typing(user);  // Keep in typing list but update state
                weechat_printf(channel->buffer, "...\t%s%s paused typing",
                               weechat_color("darkgray"),
                               display_name);
            }
            else if (active || inactive || gone)
            {
                // Clear typing state for active/inactive/gone
                channel->remove_typing(user);
            }
        }

        sent = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "sent", "urn:xmpp:carbons:2");
        if (sent)
            forwarded = xmpp_stanza_get_child_by_name_and_ns(
                sent, "forwarded", "urn:xmpp:forward:0");
        received = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "received", "urn:xmpp:carbons:2");
        if (received)
            forwarded = xmpp_stanza_get_child_by_name_and_ns(
                received, "forwarded", "urn:xmpp:forward:0");
        if ((sent || received) && forwarded != NULL)
        {
            xmpp_stanza_t *message = xmpp_stanza_get_children(forwarded);
            return message_handler(message, false);  // Don't double-count nested stanza
        }

        result = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "result", "urn:xmpp:mam:2");
        if (result)
        {
            forwarded = xmpp_stanza_get_child_by_name_and_ns(
                result, "forwarded", "urn:xmpp:forward:0");
            if (forwarded != NULL)
            {
                xmpp_stanza_t *message = xmpp_stanza_get_child_by_name(forwarded, "message");
                if (message)
                {
                    const char *debug_from = xmpp_stanza_get_from(message);
                    const char *debug_to = xmpp_stanza_get_to(message);
                    const char *debug_type = xmpp_stanza_get_type(message);
                    
                    // For global MAM queries, create PM channels based on conversation partners
                    // Since we can't reliably get the query ID from individual result messages,
                    // we'll create channels for all 1-on-1 conversations (non-MUC)
                    if (!debug_type || weechat_strcasecmp(debug_type, "groupchat") != 0)
                    {
                        const char *from_bare = debug_from ? xmpp_jid_bare(account.context, debug_from) : NULL;
                        const char *to_bare = debug_to ? xmpp_jid_bare(account.context, debug_to) : NULL;
                        
                        // Determine the conversation partner JID
                        const char *partner_jid = NULL;
                        if (from_bare && weechat_strcasecmp(from_bare, account.jid().data()) != 0)
                            partner_jid = from_bare;  // Message FROM someone else
                        else if (to_bare && weechat_strcasecmp(to_bare, account.jid().data()) != 0)
                            partner_jid = to_bare;  // Message TO someone else (sent by us)
                        
                        // Create PM channel if it doesn't exist
                        if (partner_jid && !account.channels.contains(partner_jid))
                        {
                            weechat_printf(account.buffer, "%sDiscovered conversation with %s",
                                          weechat_prefix("network"), partner_jid);
                            
                            account.channels.emplace(
                                std::make_pair(partner_jid, weechat::channel {
                                        account, weechat::channel::chat_type::PM,
                                        partner_jid, partner_jid
                                    }));
                        }
                        
                        if (from_bare)
                            xmpp_free(account.context, (void*)from_bare);
                        if (to_bare)
                            xmpp_free(account.context, (void*)to_bare);
                    }
                    
                    // Extract message details for caching
                    const char *msg_id = xmpp_stanza_get_id(message);
                    const char *msg_from = xmpp_stanza_get_from(message);
                    const char *msg_to = xmpp_stanza_get_to(message);
                    xmpp_stanza_t *msg_body = xmpp_stanza_get_child_by_name(message, "body");
                    char *msg_text = msg_body ? xmpp_stanza_get_text(msg_body) : NULL;
                    
                    delay = xmpp_stanza_get_child_by_name_and_ns(
                        forwarded, "delay", "urn:xmpp:delay");
                    const char *timestamp_str = delay ? xmpp_stanza_get_attribute(delay, "stamp") : NULL;
                    
                    // Parse timestamp
                    time_t msg_timestamp = 0;
                    if (timestamp_str)
                    {
                        struct tm time = {0};
                        strptime(timestamp_str, "%FT%T", &time);
                        msg_timestamp = mktime(&time);
                    }
                    
                    // Cache the message if we have all required fields
                    if (msg_id && msg_from && msg_text && msg_timestamp > 0)
                    {
                        const char *from_bare = xmpp_jid_bare(account.context, msg_from);
                        const char *to_bare = msg_to ? xmpp_jid_bare(account.context, msg_to) : NULL;
                        
                        // Determine channel JID (from_bare for received, to_bare for sent)
                        const char *channel_jid = from_bare;
                        if (to_bare && weechat_strcasecmp(to_bare, account.jid().data()) != 0)
                            channel_jid = to_bare;
                        
                        account.mam_cache_message(channel_jid, msg_id, from_bare, 
                                                  msg_timestamp, msg_text);
                        
                        xmpp_free(account.context, (void*)from_bare);
                        if (to_bare)
                            xmpp_free(account.context, (void*)to_bare);
                    }
                    
                    if (msg_text)
                        xmpp_free(account.context, msg_text);
                    
                    message = xmpp_stanza_copy(message);
                    if (delay != NULL)
                        xmpp_stanza_add_child_ex(message, xmpp_stanza_copy(delay), 0);
                    int ret = message_handler(message, false);  // Don't double-count MAM forwarded message
                    xmpp_stanza_release(message);
                    return ret;
                }
            }
        }

        event = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "event", "http://jabber.org/protocol/pubsub#event");
        if (event)
        {
            items = xmpp_stanza_get_child_by_name(event, "items");
            if (items)
            {
                const char *items_node = xmpp_stanza_get_attribute(items, "node");
                from = xmpp_stanza_get_from(stanza);
                to = xmpp_stanza_get_to(stanza);
                
                // Log all PEP events for debugging/future features (XEP-0163)
                if (items_node)
                {
                    weechat_printf_date_tags(account.buffer, 0, "xmpp_pep",
                                            "%sPEP event from %s: %s",
                                            weechat_prefix("network"),
                                            from ? from : account.jid().data(),
                                            items_node);
                }
                
                if (items_node
                    && weechat_strcasecmp(items_node,
                                          "eu.siacs.conversations.axolotl.devicelist") == 0)
                {
                    item = xmpp_stanza_get_child_by_name(items, "item");
                    if (item)
                    {
                        list = xmpp_stanza_get_child_by_name_and_ns(
                            item, "list", "eu.siacs.conversations.axolotl");
                        if (list)
                        {
                            if (account.omemo)
                            {
                                account.omemo.handle_devicelist(
                                    from ? from : account.jid().data(), items);
                            }

                            auto children = std::unique_ptr<xmpp_stanza_t*[]>(new xmpp_stanza_t*[3 + 1]);

                            for (device = xmpp_stanza_get_children(list);
                                 device; device = xmpp_stanza_get_next(device))
                            {
                                const char *name = xmpp_stanza_get_name(device);
                                if (weechat_strcasecmp(name, "device") != 0)
                                    continue;

                                const char *device_id = xmpp_stanza_get_id(device);

                                char bundle_node[128] = {0};
                                snprintf(bundle_node, sizeof(bundle_node),
                                         "eu.siacs.conversations.axolotl.bundles:%s",
                                         device_id);

                                children[1] = NULL;
                                children[0] =
                                stanza__iq_pubsub_items(account.context, NULL,
                                                        strdup(bundle_node));
                                children[0] =
                                stanza__iq_pubsub(account.context, NULL, children.get(),
                                                  with_noop("http://jabber.org/protocol/pubsub"));
                                char *uuid = xmpp_uuid_gen(account.context);
                                children[0] =
                                stanza__iq(account.context, NULL, children.get(), NULL, uuid,
                                            strdup(to), strdup(from), strdup("get"));
                                xmpp_free(account.context, uuid);

                                account.connection.send(children[0]);
                                xmpp_stanza_release(children[0]);
                            }
                        }
                    }
                }
                
                // XEP-0402: PEP Native Bookmarks  
                if (items_node
                    && weechat_strcasecmp(items_node, "urn:xmpp:bookmarks:1") == 0)
                {
                    // Clear existing bookmarks before loading from PEP
                    account.bookmarks.clear();
                    
                    for (item = xmpp_stanza_get_children(items);
                         item; item = xmpp_stanza_get_next(item))
                    {
                        const char *item_name = xmpp_stanza_get_name(item);
                        if (weechat_strcasecmp(item_name, "item") != 0)
                            continue;
                        
                        // <item id='roomjid@conference.example.org'>
                        //   <conference xmlns='urn:xmpp:bookmarks:1'
                        //               name='The Play Room'
                        //               autojoin='true'>
                        //     <nick>FeatherBrain</nick>
                        //   </conference>
                        // </item>
                        
                        const char *item_id = xmpp_stanza_get_id(item);
                        if (!item_id)
                            continue;  // Item ID is the room JID
                        
                        xmpp_stanza_t *conference = xmpp_stanza_get_child_by_name_and_ns(
                            item, "conference", "urn:xmpp:bookmarks:1");
                        if (!conference)
                            continue;
                        
                        const char *name = xmpp_stanza_get_attribute(conference, "name");
                        const char *autojoin = xmpp_stanza_get_attribute(conference, "autojoin");
                        
                        xmpp_stanza_t *nick_elem = xmpp_stanza_get_child_by_name(conference, "nick");
                        char *nick_text = NULL;
                        if (nick_elem)
                            nick_text = xmpp_stanza_get_text(nick_elem);
                        
                        // Store bookmark
                        account.bookmarks[item_id].jid = item_id;
                        account.bookmarks[item_id].name = name ? name : "";
                        account.bookmarks[item_id].nick = nick_text ? nick_text : "";
                        account.bookmarks[item_id].autojoin = autojoin && 
                            (weechat_strcasecmp(autojoin, "true") == 0 || 
                             weechat_strcasecmp(autojoin, "1") == 0);
                        
                        if (nick_text)
                            xmpp_free(account.context, nick_text);
                        
                        // Autodisco the room
                        account.connection.send(stanza::iq()
                                    .from(to)
                                    .to(item_id)
                                    .type("get")
                                    .id(stanza::uuid(account.context))
                                    .xep0030()
                                    .query()
                                    .build(account.context)
                                    .get());
                        
                        // Autojoin if enabled
                        if (account.bookmarks[item_id].autojoin)
                        {
                            // Skip biboumi (IRC gateway) rooms
                            bool is_biboumi = (strchr(item_id, '%') != NULL) ||
                                            (strstr(item_id, "biboumi") != NULL) ||
                                            (strstr(item_id, "@irc.") != NULL);
                            
                            if (!is_biboumi)
                            {
                                char **command = weechat_string_dyn_alloc(256);
                                weechat_string_dyn_concat(command, "/enter ", -1);
                                weechat_string_dyn_concat(command, item_id, -1);
                                if (nick_text && strlen(nick_text) > 0)
                                {
                                    weechat_string_dyn_concat(command, "/", -1);
                                    weechat_string_dyn_concat(command, nick_text, -1);
                                }
                                weechat_command(account.buffer, *command);
                                weechat_string_dyn_free(command, 1);
                            }
                            else
                            {
                                weechat_printf(account.buffer, 
                                              "%sSkipping autojoin for IRC gateway room: %s",
                                              weechat_prefix("network"), item_id);
                            }
                        }
                    }
                    
                    weechat_printf(account.buffer, "%sLoaded %zu bookmarks from PEP",
                                  weechat_prefix("network"),
                                  account.bookmarks.size());
                }
            }
        }

        return 1;
    }
    type = xmpp_stanza_get_type(stanza);
    if (type != NULL && strcmp(type, "error") == 0)
        return 1;
    from = xmpp_stanza_get_from(stanza);
    if (from == NULL)
        return 1;
    from_bare = xmpp_jid_bare(account.context, from);
    to = xmpp_stanza_get_to(stanza);
    if (to == NULL)
        to = account.jid().data();
    to_bare = to ? xmpp_jid_bare(account.context, to) : NULL;
    id = xmpp_stanza_get_id(stanza);
    thread = xmpp_stanza_get_attribute(stanza, "thread");
    
    // XEP-0359: Unique and Stable Stanza IDs
    xmpp_stanza_t *stanza_id_elem = xmpp_stanza_get_child_by_name_and_ns(stanza, "stanza-id", "urn:xmpp:sid:0");
    const char *stanza_id = stanza_id_elem ? xmpp_stanza_get_attribute(stanza_id_elem, "id") : NULL;
    const char *stanza_id_by = stanza_id_elem ? xmpp_stanza_get_attribute(stanza_id_elem, "by") : NULL;
    
    xmpp_stanza_t *origin_id_elem = xmpp_stanza_get_child_by_name_and_ns(stanza, "origin-id", "urn:xmpp:sid:0");
    const char *origin_id = origin_id_elem ? xmpp_stanza_get_attribute(origin_id_elem, "id") : NULL;
    
    // Prefer stanza-id over origin-id over regular id for stable message identification
    const char *stable_id = stanza_id ? stanza_id : (origin_id ? origin_id : id);
    
    replace = xmpp_stanza_get_child_by_name_and_ns(stanza, "replace",
                                                   "urn:xmpp:message-correct:0");
    replace_id = replace ? xmpp_stanza_get_id(replace) : NULL;
    request = xmpp_stanza_get_child_by_name_and_ns(stanza, "request",
                                                   "urn:xmpp:receipts");
    markable = xmpp_stanza_get_child_by_name_and_ns(stanza, "markable",
                                                    "urn:xmpp:chat-markers:0");

    const char *channel_id = account.jid() == from_bare ? to_bare : from_bare;
    parent_channel = account.channels.contains(channel_id)
        ? &account.channels.find(channel_id)->second : nullptr;
    const char *pm_id = account.jid() == from_bare ? to : from;
    channel = parent_channel;
    if (!channel)
    {
        channel = &account.channels.emplace(
            std::make_pair(channel_id, weechat::channel {
                    account,
                    weechat_strcasecmp(type, "groupchat") == 0
                        ? weechat::channel::chat_type::MUC : weechat::channel::chat_type::PM,
                    channel_id, channel_id
                })).first->second;
    }
    if (channel && channel->type == weechat::channel::chat_type::MUC
        && weechat_strcasecmp(type, "chat") == 0)
    {
        channel = &account.channels.emplace(
            std::make_pair(pm_id, weechat::channel {
                    account, weechat::channel::chat_type::PM,
                    pm_id, pm_id
                })).first->second;
    }

    if (id && (markable || request))
    {
        auto unread = new weechat::channel::unread();
        unread->id = strdup(id);
        unread->thread = thread ? strdup(thread) : NULL;

        xmpp_stanza_t *message = xmpp_message_new(account.context, NULL,
                                                  channel->id.data(), NULL);

        if (request)
        {
            xmpp_stanza_t *message__received = xmpp_stanza_new(account.context);
            xmpp_stanza_set_name(message__received, "received");
            xmpp_stanza_set_ns(message__received, "urn:xmpp:receipts");
            xmpp_stanza_set_id(message__received, unread->id);

            xmpp_stanza_add_child(message, message__received);
            xmpp_stanza_release(message__received);
        }

        if (markable)
        {
            xmpp_stanza_t *message__received = xmpp_stanza_new(account.context);
            xmpp_stanza_set_name(message__received, "received");
            xmpp_stanza_set_ns(message__received, "urn:xmpp:chat-markers:0");
            xmpp_stanza_set_id(message__received, unread->id);

            xmpp_stanza_add_child(message, message__received);
            xmpp_stanza_release(message__received);
        }

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

        account.connection.send( message);
        xmpp_stanza_release(message);

        channel->unreads.push_back(*unread);
    }

    // XEP-0249: Direct MUC Invitations
    x = xmpp_stanza_get_child_by_name_and_ns(stanza, "x", "jabber:x:conference");
    if (x)
    {
        const char *room_jid = xmpp_stanza_get_attribute(x, "jid");
        const char *password = xmpp_stanza_get_attribute(x, "password");
        const char *reason = xmpp_stanza_get_attribute(x, "reason");
        
        if (room_jid)
        {
            from = xmpp_stanza_get_from(stanza);
            from_bare = from ? xmpp_jid_bare(account.context, from) : "unknown";
            
            weechat_printf(account.buffer,
                          _("%s%s invited you to %s%s%s"),
                          weechat_prefix("network"),
                          from_bare,
                          room_jid,
                          reason ? " (" : "",
                          reason ? reason : "");
            if (reason)
                weechat_printf(account.buffer, "%s)", "");
            weechat_printf(account.buffer,
                          _("%sTo join: /join %s%s%s"),
                          weechat_prefix("network"),
                          room_jid,
                          password ? " " : "",
                          password ? password : "");
        }
        return 1;
    }

    encrypted = xmpp_stanza_get_child_by_name_and_ns(stanza, "encrypted",
                                                     "eu.siacs.conversations.axolotl");
    x = xmpp_stanza_get_child_by_name_and_ns(stanza, "x", "jabber:x:encrypted");
    
    // XEP-0380: Explicit Message Encryption
    xmpp_stanza_t *eme = xmpp_stanza_get_child_by_name_and_ns(stanza, "encryption",
                                                                "urn:xmpp:eme:0");
    const char *eme_namespace = eme ? xmpp_stanza_get_attribute(eme, "namespace") : NULL;
    const char *eme_name = eme ? xmpp_stanza_get_attribute(eme, "name") : NULL;
    
    intext = xmpp_stanza_get_text(body);
    
    // Auto-enable OMEMO for PM channels when receiving encrypted messages
    if (encrypted && channel && channel->type == weechat::channel::chat_type::PM && !channel->omemo.enabled)
    {
        weechat_printf(channel->buffer, "%sAuto-enabling OMEMO (received encrypted message)",
                       weechat_prefix("network"));
        channel->omemo.enabled = 1;
        channel->set_transport(weechat::channel::transport::OMEMO, 0);
    }
    
    // Auto-enable PGP for PM channels when receiving PGP encrypted messages
    if (x && channel && channel->type == weechat::channel::chat_type::PM && !channel->pgp.enabled)
    {
        weechat_printf(channel->buffer, "%sAuto-enabling PGP (received encrypted message)",
                       weechat_prefix("network"));
        channel->pgp.enabled = 1;
        channel->set_transport(weechat::channel::transport::PGP, 0);
    }
    
    if (encrypted && account.omemo)
    {
        cleartext = account.omemo.decode(&account, from_bare, encrypted);
        if (!cleartext)
        {
            weechat_printf_date_tags(channel->buffer, 0, "notify_none", "%s%s (%s)",
                                     weechat_prefix("error"), "OMEMO Decryption Error", from);
            return 1;
        }
    }
    if (x)
    {
        char *ciphertext = xmpp_stanza_get_text(x);
        cleartext = account.pgp.decrypt(channel->buffer, ciphertext);
        xmpp_free(account.context, ciphertext);
    }
    text = cleartext ? cleartext : intext;

    if (replace)
    {
        const char *orig = NULL;
        void *lines = weechat_hdata_pointer(weechat_hdata_get("buffer"),
                                            channel->buffer, "lines");
        if (lines)
        {
            void *last_line = weechat_hdata_pointer(weechat_hdata_get("lines"),
                                                    lines, "last_line");
            while (last_line && !orig)
            {
                void *line_data = weechat_hdata_pointer(weechat_hdata_get("line"),
                                                        last_line, "data");
                if (line_data)
                {
                    int tags_count = weechat_hdata_integer(weechat_hdata_get("line_data"),
                                                           line_data, "tags_count");
                    char str_tag[24] = {0};
                    for (int n_tag = 0; n_tag < tags_count; n_tag++)
                    {
                        snprintf(str_tag, sizeof(str_tag), "%d|tags_array", n_tag);
                        const char *tag = weechat_hdata_string(weechat_hdata_get("line_data"),
                                                               line_data, str_tag);
                        if (strlen(tag) > strlen("id_") &&
                            weechat_strcasecmp(tag+strlen("id_"), replace_id) == 0)
                        {
                            struct t_arraylist *orig_lines = weechat_arraylist_new(
                                0, 0, 0, NULL, NULL, NULL, NULL);
                            char *msg = (char*)weechat_hdata_string(weechat_hdata_get("line_data"),
                                                                    line_data, "message");
                            weechat_arraylist_insert(orig_lines, 0, msg);

                            while (msg)
                            {
                                last_line = weechat_hdata_pointer(weechat_hdata_get("line"),
                                                                  last_line, "prev_line");
                                if (last_line)
                                    line_data = weechat_hdata_pointer(weechat_hdata_get("line"),
                                                                      last_line, "data");
                                else
                                    line_data = NULL;

                                msg = NULL;
                                if (line_data)
                                {
                                    tags_count = weechat_hdata_integer(weechat_hdata_get("line_data"),
                                                                       line_data, "tags_count");
                                    for (n_tag = 0; n_tag < tags_count; n_tag++)
                                    {
                                        snprintf(str_tag, sizeof(str_tag), "%d|tags_array", n_tag);
                                        tag = weechat_hdata_string(weechat_hdata_get("line_data"),
                                                                   line_data, str_tag);
                                        if (strlen(tag) > strlen("id_") &&
                                            weechat_strcasecmp(tag+strlen("id_"), replace_id) == 0)
                                        {
                                            msg = (char*)weechat_hdata_string(weechat_hdata_get("line_data"),
                                                                              line_data, "message");
                                            break;
                                        }
                                    }
                                }

                                if (msg)
                                    weechat_arraylist_insert(orig_lines, 0, msg);
                            }

                            char **orig_message = weechat_string_dyn_alloc(256);
                            for (int i = 0; i < weechat_arraylist_size(orig_lines); i++)
                                weechat_string_dyn_concat(orig_message,
                                                          (const char*)weechat_arraylist_get(orig_lines, i),
                                                          -1);
                            orig = *orig_message;
                            weechat_string_dyn_free(orig_message, 0);
                            break;
                        }
                    }
                }

                last_line = weechat_hdata_pointer(weechat_hdata_get("line"),
                                                  last_line, "prev_line");
            }
        }

        if (orig)
        {
            struct diff result;
            if (diff(&result, char_cmp, 1, orig, strlen(orig), text, strlen(text)) > 0)
            {
                char **visual = weechat_string_dyn_alloc(256);
                char ch[2] = {0};
                int retention = 0;
                int modification = 0;

                for (size_t i = 0; i < result.sessz; i++)
                    switch (result.ses[i].type)
                    {
                        case DIFF_ADD:
                            weechat_string_dyn_concat(visual, weechat_color("green"), -1);
                            *ch = *(const char *)result.ses[i].e;
                            weechat_string_dyn_concat(visual, ch, -1);
                            modification++;
                            break;
                        case DIFF_DELETE:
                            weechat_string_dyn_concat(visual, weechat_color("red"), -1);
                            *ch = *(const char *)result.ses[i].e;
                            weechat_string_dyn_concat(visual, ch, -1);
                            modification++;
                            break;
                        case DIFF_COMMON:
                        default:
                            weechat_string_dyn_concat(visual, weechat_color("resetcolor"), -1);
                            *ch = *(const char *)result.ses[i].e;

                            weechat_string_dyn_concat(visual, ch, -1);
                            retention++;
                            break;
                    }
                free(result.ses);
                free(result.lcs);

                if ((modification > 20) && (modification > retention)) {
                    weechat_string_dyn_free(visual, 1);
                    visual = weechat_string_dyn_alloc(256);
                    weechat_string_dyn_concat(visual, weechat_color("red"), -1);
                    if (strlen(orig) >= 16) {
                        weechat_string_dyn_concat(visual, orig, 16);
                        weechat_string_dyn_concat(visual, "...", -1);
                    } else
                        weechat_string_dyn_concat(visual, orig, -1);
                    weechat_string_dyn_concat(visual, weechat_color("green"), -1);
                    weechat_string_dyn_concat(visual, text, -1);
                }
                difftext = strdup(*visual);
                weechat_string_dyn_free(visual, 1);
            }
        }
    }

    // XEP-0425: Message Moderation (extends XEP-0424)
    // Look for <apply-to xmlns='urn:xmpp:fasten:0'><moderate xmlns='urn:xmpp:message-moderate:1'>
    xmpp_stanza_t *apply_to = xmpp_stanza_get_child_by_name_and_ns(stanza, "apply-to",
                                                                    "urn:xmpp:fasten:0");
    const char *moderate_id = NULL;
    const char *moderate_reason = NULL;
    
    if (apply_to)
    {
        moderate_id = xmpp_stanza_get_attribute(apply_to, "id");
        xmpp_stanza_t *moderate = xmpp_stanza_get_child_by_name_and_ns(apply_to, "moderate",
                                                                        "urn:xmpp:message-moderate:1");
        if (moderate && moderate_id)
        {
            // Extract optional reason
            xmpp_stanza_t *reason_elem = xmpp_stanza_get_child_by_name(moderate, "reason");
            if (reason_elem)
            {
                char *reason_text = xmpp_stanza_get_text(reason_elem);
                moderate_reason = reason_text;  // Will be freed later
            }
            
            // Save moderation to MAM cache
            const char *channel_id = account.jid() == from_bare ? to_bare : from_bare;
            account.mam_cache_retract_message(channel_id, moderate_id);
            
            // Find and tombstone the moderated message in buffer
            void *lines = weechat_hdata_pointer(weechat_hdata_get("buffer"),
                                                channel->buffer, "lines");
            if (lines)
            {
                void *last_line = weechat_hdata_pointer(weechat_hdata_get("lines"),
                                                        lines, "last_line");
                while (last_line)
                {
                    void *line_data = weechat_hdata_pointer(weechat_hdata_get("line"),
                                                            last_line, "data");
                    if (line_data)
                    {
                        int tags_count = weechat_hdata_integer(weechat_hdata_get("line_data"),
                                                               line_data, "tags_count");
                        char str_tag[24] = {0};
                        for (int n_tag = 0; n_tag < tags_count; n_tag++)
                        {
                            snprintf(str_tag, sizeof(str_tag), "%d|tags_array", n_tag);
                            const char *tag = weechat_hdata_string(weechat_hdata_get("line_data"),
                                                                   line_data, str_tag);
                            if (strlen(tag) > strlen("id_") &&
                                weechat_strcasecmp(tag+strlen("id_"), moderate_id) == 0)
                            {
                                // Found the message to moderate - update it with tombstone
                                char tombstone[512];
                                if (moderate_reason)
                                    snprintf(tombstone, sizeof(tombstone), 
                                            "%s[Message moderated: %s]%s", 
                                            weechat_color("darkgray"),
                                            moderate_reason,
                                            weechat_color("resetcolor"));
                                else
                                    snprintf(tombstone, sizeof(tombstone), 
                                            "%s[Message moderated by room moderator]%s", 
                                            weechat_color("darkgray"),
                                            weechat_color("resetcolor"));
                                
                                // Update the line with tombstone
                                struct t_hashtable *hashtable = weechat_hashtable_new(8,
                                    WEECHAT_HASHTABLE_STRING,
                                    WEECHAT_HASHTABLE_STRING,
                                    NULL, NULL);
                                weechat_hashtable_set(hashtable, "message", tombstone);
                                weechat_hashtable_set(hashtable, "tags", "xmpp_retracted,xmpp_moderated,notify_none");
                                weechat_hdata_update(weechat_hdata_get("line_data"), line_data, hashtable);
                                weechat_hashtable_free(hashtable);
                                
                                // Print notification
                                if (moderate_reason)
                                    weechat_printf_date_tags(channel->buffer, 0, "notify_none",
                                        "%s%s moderated a message: %s",
                                        weechat_prefix("network"),
                                        from_bare, moderate_reason);
                                else
                                    weechat_printf_date_tags(channel->buffer, 0, "notify_none",
                                        "%s%s moderated a message",
                                        weechat_prefix("network"),
                                        from_bare);
                                
                                if (moderate_reason)
                                    xmpp_free(account.context, (void*)moderate_reason);
                                xmpp_free(account.context, (void*)from_bare);
                                if (to_bare) xmpp_free(account.context, (void*)to_bare);
                                if (cleartext) free(cleartext);
                                if (intext) xmpp_free(account.context, intext);
                                return 1;
                            }
                        }
                    }

                    last_line = weechat_hdata_pointer(weechat_hdata_get("line"),
                                                      last_line, "prev_line");
                }
            }
            
            // If we didn't find the message, still print notification
            if (moderate_reason)
                weechat_printf_date_tags(channel->buffer, 0, "notify_none",
                    "%s%s moderated a message (not found in buffer): %s",
                    weechat_prefix("network"),
                    from_bare, moderate_reason);
            else
                weechat_printf_date_tags(channel->buffer, 0, "notify_none",
                    "%s%s moderated a message (not found in buffer)",
                    weechat_prefix("network"),
                    from_bare);
            
            if (moderate_reason)
                xmpp_free(account.context, (void*)moderate_reason);
            xmpp_free(account.context, (void*)from_bare);
            if (to_bare) xmpp_free(account.context, (void*)to_bare);
            if (cleartext) free(cleartext);
            if (intext) xmpp_free(account.context, intext);
            return 1;
        }
    }

    // XEP-0424: Message Retraction
    xmpp_stanza_t *retract = xmpp_stanza_get_child_by_name_and_ns(stanza, "retract",
                                                                   "urn:xmpp:message-retract:1");
    const char *retract_id = retract ? xmpp_stanza_get_attribute(retract, "id") : NULL;
    
    if (retract_id)
    {
        // Save retraction to MAM cache
        const char *channel_id = account.jid() == from_bare ? to_bare : from_bare;
        account.mam_cache_retract_message(channel_id, retract_id);
        
        // Find and tombstone the retracted message in buffer
        void *lines = weechat_hdata_pointer(weechat_hdata_get("buffer"),
                                            channel->buffer, "lines");
        if (lines)
        {
            void *last_line = weechat_hdata_pointer(weechat_hdata_get("lines"),
                                                    lines, "last_line");
            while (last_line)
            {
                void *line_data = weechat_hdata_pointer(weechat_hdata_get("line"),
                                                        last_line, "data");
                if (line_data)
                {
                    int tags_count = weechat_hdata_integer(weechat_hdata_get("line_data"),
                                                           line_data, "tags_count");
                    char str_tag[24] = {0};
                    for (int n_tag = 0; n_tag < tags_count; n_tag++)
                    {
                        snprintf(str_tag, sizeof(str_tag), "%d|tags_array", n_tag);
                        const char *tag = weechat_hdata_string(weechat_hdata_get("line_data"),
                                                               line_data, str_tag);
                        if (strlen(tag) > strlen("id_") &&
                            weechat_strcasecmp(tag+strlen("id_"), retract_id) == 0)
                        {
                            // Found the message to retract - update it with tombstone
                            // Create tombstone text
                            char tombstone[256];
                            snprintf(tombstone, sizeof(tombstone), 
                                    "%s[Message deleted]%s", 
                                    weechat_color("darkgray"),
                                    weechat_color("resetcolor"));
                            
                            // Update the line with tombstone
                            struct t_hashtable *hashtable = weechat_hashtable_new(8,
                                WEECHAT_HASHTABLE_STRING,
                                WEECHAT_HASHTABLE_STRING,
                                NULL, NULL);
                            weechat_hashtable_set(hashtable, "message", tombstone);
                            weechat_hashtable_set(hashtable, "tags", "xmpp_retracted,notify_none");
                            weechat_hdata_update(weechat_hdata_get("line_data"), line_data, hashtable);
                            weechat_hashtable_free(hashtable);
                            
                            // Print notification
                            weechat_printf_date_tags(channel->buffer, 0, "notify_none",
                                "%s%s retracted a message",
                                weechat_prefix("network"),
                                from_bare);
                            
                            xmpp_free(account.context, (void*)from_bare);
                            if (to_bare) xmpp_free(account.context, (void*)to_bare);
                            if (cleartext) free(cleartext);
                            if (intext) xmpp_free(account.context, intext);
                            return 1;
                        }
                    }
                }

                last_line = weechat_hdata_pointer(weechat_hdata_get("line"),
                                                  last_line, "prev_line");
            }
        }
        
        // If we didn't find the message, still print notification
        weechat_printf_date_tags(channel->buffer, 0, "notify_none",
            "%s%s retracted a message (not found in buffer)",
            weechat_prefix("network"),
            from_bare);
        
        xmpp_free(account.context, (void*)from_bare);
        if (to_bare) xmpp_free(account.context, (void*)to_bare);
        if (cleartext) free(cleartext);
        if (intext) xmpp_free(account.context, intext);
        return 1;
    }

    // XEP-0444: Message Reactions
    xmpp_stanza_t *reactions = xmpp_stanza_get_child_by_name_and_ns(stanza, "reactions",
                                                                     "urn:xmpp:reactions:0");
    const char *reactions_id = reactions ? xmpp_stanza_get_attribute(reactions, "id") : NULL;
    
    if (reactions_id)
    {
        // Extract emoji from <reaction> elements
        char **dyn_emojis = weechat_string_dyn_alloc(64);
        xmpp_stanza_t *reaction_elem = xmpp_stanza_get_children(reactions);
        bool first = true;
        while (reaction_elem)
        {
            const char *name = xmpp_stanza_get_name(reaction_elem);
            if (name && weechat_strcasecmp(name, "reaction") == 0)
            {
                char *emoji = xmpp_stanza_get_text(reaction_elem);
                if (emoji)
                {
                    if (!first) weechat_string_dyn_concat(dyn_emojis, " ", -1);
                    weechat_string_dyn_concat(dyn_emojis, emoji, -1);
                    xmpp_free(account.context, emoji);
                    first = false;
                }
            }
            reaction_elem = xmpp_stanza_get_next(reaction_elem);
        }
        
        const char *emojis = *dyn_emojis;
        if (strlen(emojis) > 0)
        {
            // Find the message being reacted to and append reaction
            void *lines = weechat_hdata_pointer(weechat_hdata_get("buffer"),
                                                channel->buffer, "lines");
            if (lines)
            {
                void *last_line = weechat_hdata_pointer(weechat_hdata_get("lines"),
                                                        lines, "last_line");
                while (last_line)
                {
                    void *line_data = weechat_hdata_pointer(weechat_hdata_get("line"),
                                                            last_line, "data");
                    if (line_data)
                    {
                        int tags_count = weechat_hdata_integer(weechat_hdata_get("line_data"),
                                                               line_data, "tags_count");
                        char str_tag[24] = {0};
                        for (int n_tag = 0; n_tag < tags_count; n_tag++)
                        {
                            snprintf(str_tag, sizeof(str_tag), "%d|tags_array", n_tag);
                            const char *tag = weechat_hdata_string(weechat_hdata_get("line_data"),
                                                                   line_data, str_tag);
                            if (strlen(tag) > strlen("id_") &&
                                weechat_strcasecmp(tag+strlen("id_"), reactions_id) == 0)
                            {
                                // Found the message - get original text and append reaction
                                const char *orig_message = weechat_hdata_string(
                                    weechat_hdata_get("line_data"), line_data, "message");
                                
                                char new_message[4096];
                                snprintf(new_message, sizeof(new_message), 
                                        "%s %s[%s]%s", 
                                        orig_message,
                                        weechat_color("blue"),
                                        emojis,
                                        weechat_color("resetcolor"));
                                
                                // Update the line with reaction appended
                                struct t_hashtable *hashtable = weechat_hashtable_new(8,
                                    WEECHAT_HASHTABLE_STRING,
                                    WEECHAT_HASHTABLE_STRING,
                                    NULL, NULL);
                                weechat_hashtable_set(hashtable, "message", new_message);
                                weechat_hdata_update(weechat_hdata_get("line_data"), line_data, hashtable);
                                weechat_hashtable_free(hashtable);
                                
                                weechat_string_dyn_free(dyn_emojis, 1);
                                xmpp_free(account.context, (void*)from_bare);
                                if (to_bare) xmpp_free(account.context, (void*)to_bare);
                                if (cleartext) free(cleartext);
                                if (intext) xmpp_free(account.context, intext);
                                return 1;
                            }
                        }
                    }

                    last_line = weechat_hdata_pointer(weechat_hdata_get("line"),
                                                      last_line, "prev_line");
                }
            }
        }
        
        weechat_string_dyn_free(dyn_emojis, 1);
        xmpp_free(account.context, (void*)from_bare);
        if (to_bare) xmpp_free(account.context, (void*)to_bare);
        if (cleartext) free(cleartext);
        if (intext) xmpp_free(account.context, intext);
        return 1;
    }

    nick = from;
    const char *display_from = from_bare;
    if (weechat_strcasecmp(type, "groupchat") == 0)
    {
        nick = channel->name == xmpp_jid_bare(account.context, from)
            ? xmpp_jid_resource(account.context, from)
            : from;
        display_from = from;
    }
    else if (parent_channel && parent_channel->type == weechat::channel::chat_type::MUC)
    {
        nick = channel->name == from
            ? xmpp_jid_resource(account.context, from)
            : from;
        display_from = from;
    }
    delay = xmpp_stanza_get_child_by_name_and_ns(stanza, "delay", "urn:xmpp:delay");
    timestamp = delay ? xmpp_stanza_get_attribute(delay, "stamp") : NULL;
    const char *delay_from = delay ? xmpp_stanza_get_attribute(delay, "from") : NULL;
    if (timestamp)
    {
        strptime(timestamp, "%FT%T", &time);
        date = mktime(&time);
    }

    char **dyn_tags = weechat_string_dyn_alloc(1);
    weechat_string_dyn_concat(dyn_tags, "xmpp_message,message", -1);
    {
        weechat_string_dyn_concat(dyn_tags, ",nick_", -1);
        weechat_string_dyn_concat(dyn_tags, nick, -1);
    }
    {
        weechat_string_dyn_concat(dyn_tags, ",host_", -1);
        weechat_string_dyn_concat(dyn_tags, from, -1);
    }
    // Store stable ID (XEP-0359) in tags for message tracking
    if (stable_id)
    {
        weechat_string_dyn_concat(dyn_tags, ",id_", -1);
        weechat_string_dyn_concat(dyn_tags, stable_id, -1);
    }
    // Also store origin-id if different from stable_id
    if (origin_id && origin_id != stable_id)
    {
        weechat_string_dyn_concat(dyn_tags, ",origin_id_", -1);
        weechat_string_dyn_concat(dyn_tags, origin_id, -1);
    }
    // Store stanza-id metadata if present
    if (stanza_id && stanza_id_by)
    {
        weechat_string_dyn_concat(dyn_tags, ",stanza_id_", -1);
        weechat_string_dyn_concat(dyn_tags, stanza_id, -1);
        weechat_string_dyn_concat(dyn_tags, ",stanza_id_by_", -1);
        weechat_string_dyn_concat(dyn_tags, stanza_id_by, -1);
    }
    // XEP-0380: Store encryption method if advertised
    if (eme_namespace)
    {
        weechat_string_dyn_concat(dyn_tags, ",eme_", -1);
        if (eme_name)
            weechat_string_dyn_concat(dyn_tags, eme_name, -1);
        else if (strstr(eme_namespace, "axolotl"))
            weechat_string_dyn_concat(dyn_tags, "OMEMO", -1);
        else if (strstr(eme_namespace, "openpgp"))
            weechat_string_dyn_concat(dyn_tags, "OpenPGP", -1);
        else if (strstr(eme_namespace, "encryption"))
            weechat_string_dyn_concat(dyn_tags, "PGP", -1);
        else
            weechat_string_dyn_concat(dyn_tags, "encrypted", -1);
    }
    // XEP-0203: Tag delayed messages
    if (delay)
    {
        weechat_string_dyn_concat(dyn_tags, ",delayed", -1);
        if (delay_from)
        {
            weechat_string_dyn_concat(dyn_tags, ",delay_from_", -1);
            weechat_string_dyn_concat(dyn_tags, delay_from, -1);
        }
    }

    if (channel->type == weechat::channel::chat_type::PM)
        weechat_string_dyn_concat(dyn_tags, ",private", -1);
    if (weechat_string_match(text, "/me *", 0))
        weechat_string_dyn_concat(dyn_tags, ",xmpp_action", -1);
    if (replace)
    {
        weechat_string_dyn_concat(dyn_tags, ",edit", -1);
        weechat_string_dyn_concat(dyn_tags, ",replace_", -1);
        weechat_string_dyn_concat(dyn_tags, replace_id, -1);
    }

    if (date != 0 || encrypted)
        weechat_string_dyn_concat(dyn_tags, ",notify_none", -1);
    else if (channel->type == weechat::channel::chat_type::PM
             && from_bare != account.jid())
        weechat_string_dyn_concat(dyn_tags, ",notify_private", -1);
    else
        weechat_string_dyn_concat(dyn_tags, ",notify_message,log1", -1);

    const char *edit = replace ? "* " : ""; // Losing which message was edited, sadly
    if (x && text == cleartext && channel->transport != weechat::channel::transport::PGP)
    {
        channel->transport = weechat::channel::transport::PGP;
        weechat_printf_date_tags(channel->buffer, date, NULL, "%s%sTransport: %s",
                                 weechat_prefix("network"), weechat_color("gray"),
                                 channel::transport_name(channel->transport));
    }
    else if (!x && text == intext && channel->transport != weechat::channel::transport::PLAIN)
    {
        channel->transport = weechat::channel::transport::PLAIN;
        weechat_printf_date_tags(channel->buffer, date, NULL, "%s%sTransport: %s",
                                 weechat_prefix("network"), weechat_color("gray"),
                                 channel::transport_name(channel->transport));
    }
    auto display_prefix = user::as_prefix_raw(&account, display_from);
    if (display_prefix.empty())
        display_prefix = user::as_prefix_raw(from_bare);
    
    // XEP-0461: Message Replies - extract reply context
    xmpp_stanza_t *reply_elem = xmpp_stanza_get_child_by_name_and_ns(stanza, "reply", "urn:xmpp:reply:0");
    const char *reply_to_id = reply_elem ? xmpp_stanza_get_attribute(reply_elem, "id") : NULL;
    std::string reply_prefix;
    
    if (reply_to_id)
    {
        // Find the original message being replied to
        void *lines = weechat_hdata_pointer(weechat_hdata_get("buffer"),
                                            channel->buffer, "lines");
        if (lines)
        {
            void *last_line = weechat_hdata_pointer(weechat_hdata_get("lines"),
                                                    lines, "last_line");
            while (last_line)
            {
                void *line_data = weechat_hdata_pointer(weechat_hdata_get("line"),
                                                        last_line, "data");
                if (line_data)
                {
                    int tags_count = weechat_hdata_integer(weechat_hdata_get("line_data"),
                                                           line_data, "tags_count");
                    char str_tag[24] = {0};
                    for (int n_tag = 0; n_tag < tags_count; n_tag++)
                    {
                        snprintf(str_tag, sizeof(str_tag), "%d|tags_array", n_tag);
                        const char *tag = weechat_hdata_string(weechat_hdata_get("line_data"),
                                                               line_data, str_tag);
                        if (strlen(tag) > strlen("id_") &&
                            weechat_strcasecmp(tag+strlen("id_"), reply_to_id) == 0)
                        {
                            // Found the original message - get excerpt
                            const char *orig_message = weechat_hdata_string(
                                weechat_hdata_get("line_data"), line_data, "message");
                            
                            if (orig_message)
                            {
                                // Extract just the text part (after tab if present)
                                const char *msg_text = strchr(orig_message, '\t');
                                if (msg_text)
                                    msg_text++; // Skip the tab
                                else
                                    msg_text = orig_message;
                                
                                // Create prefix showing what message this is replying to (max 40 chars)
                                char excerpt[45];
                                if (strlen(msg_text) > 40)
                                {
                                    strncpy(excerpt, msg_text, 40);
                                    excerpt[40] = '\0';
                                    strcat(excerpt, "...");
                                }
                                else
                                {
                                    strcpy(excerpt, msg_text);
                                }
                                
                                reply_prefix = std::string(weechat_color("cyan")) + 
                                             "↪ " + excerpt + " " +
                                             weechat_color("resetcolor");
                            }
                            break;
                        }
                    }
                }

                last_line = weechat_hdata_pointer(weechat_hdata_get("line"),
                                                  last_line, "prev_line");
            }
        }
        
        // If we didn't find the original, just show reply indicator
        if (reply_prefix.empty())
        {
            reply_prefix = std::string(weechat_color("cyan")) + 
                         "↪ [reply] " +
                         weechat_color("resetcolor");
        }
    }
    
    // XEP-0066: Out of Band Data - extract URL from <x xmlns='jabber:x:oob'>
    xmpp_stanza_t *oob_x = xmpp_stanza_get_child_by_name_and_ns(stanza, "x", "jabber:x:oob");
    std::string oob_suffix;
    if (oob_x)
    {
        xmpp_stanza_t *url_elem = xmpp_stanza_get_child_by_name(oob_x, "url");
        if (url_elem)
        {
            char *url_text = xmpp_stanza_get_text(url_elem);
            if (url_text)
            {
                // Optionally get description
                xmpp_stanza_t *desc_elem = xmpp_stanza_get_child_by_name(oob_x, "desc");
                char *desc_text = desc_elem ? xmpp_stanza_get_text(desc_elem) : NULL;
                
                // Format: [URL: url] or [URL: description (url)]
                oob_suffix = std::string(" ") + 
                            weechat_color("blue") + "[URL: ";
                if (desc_text && strlen(desc_text) > 0)
                {
                    oob_suffix += desc_text;
                    oob_suffix += " (";
                    oob_suffix += url_text;
                    oob_suffix += ")";
                }
                else
                {
                    oob_suffix += url_text;
                }
                oob_suffix += "]" + std::string(weechat_color("resetcolor"));
                
                xmpp_free(account.context, url_text);
                if (desc_text) xmpp_free(account.context, desc_text);
            }
        }
    }
    
    // Apply XEP-0393 Message Styling
    const char *display_text = text;
    std::string styled_text;
    if (text && !difftext)  // Don't style diffs (already styled)
    {
        styled_text = apply_xep393_styling(text);
        display_text = styled_text.c_str();
    }
    else if (difftext)
    {
        display_text = difftext;
    }
    
    // Prepend reply context if this is a reply
    std::string final_text;
    if (!reply_prefix.empty())
    {
        final_text = reply_prefix + (display_text ? display_text : "");
        display_text = final_text.c_str();
    }
    
    // Append OOB URL if present
    if (!oob_suffix.empty())
    {
        if (final_text.empty())
            final_text = display_text ? display_text : "";
        final_text += oob_suffix;
        display_text = final_text.c_str();
    }
    
    if (channel_id == from_bare && to == channel->id)
        weechat_printf_date_tags(channel->buffer, date, *dyn_tags, "%s%s\t[to %s]: %s",
                                 edit, display_prefix.data(),
                                 to, display_text ? display_text : "");
    else if (weechat_string_match(text, "/me *", 0))
        weechat_printf_date_tags(channel->buffer, date, *dyn_tags, "%s%s\t%s %s",
                                 edit, weechat_prefix("action"), display_prefix.data(),
                                 difftext ? difftext+4 : display_text ? display_text+4 : "");
    else
        weechat_printf_date_tags(channel->buffer, date, *dyn_tags, "%s%s\t%s",
                                 edit, display_prefix.data(),
                                 display_text ? display_text : "");

    weechat_string_dyn_free(dyn_tags, 1);

    if (intext)
        xmpp_free(account.context, intext);
    if (difftext)
        free(difftext);
    if (cleartext)
        free(cleartext);

    return true;
}

xmpp_stanza_t *weechat::connection::get_caps(xmpp_stanza_t *reply, char **hash)
{
    xmpp_stanza_t *query = xmpp_stanza_new(account.context);
    xmpp_stanza_set_name(query, "query");
    xmpp_stanza_set_ns(query, "http://jabber.org/protocol/disco#info");

    char *client_name = weechat_string_eval_expression(
            "weechat ${info:version}", NULL, NULL, NULL);
    char **serial = weechat_string_dyn_alloc(256);
    weechat_string_dyn_concat(serial, "client/pc//", -1);
    weechat_string_dyn_concat(serial, client_name, -1);
    weechat_string_dyn_concat(serial, "<", -1);

    xmpp_stanza_t *identity = xmpp_stanza_new(account.context);
    xmpp_stanza_set_name(identity, "identity");
    xmpp_stanza_set_attribute(identity, "category", "client");
    xmpp_stanza_set_attribute(identity, "name", client_name);
    free(client_name);
    xmpp_stanza_set_attribute(identity, "type", "pc");
    xmpp_stanza_add_child(query, identity);
    xmpp_stanza_release(identity);

    xmpp_stanza_t *feature = NULL;

#define FEATURE(NS)                                 \
    feature = xmpp_stanza_new(account.context);     \
    xmpp_stanza_set_name(feature, "feature");       \
    xmpp_stanza_set_attribute(feature, "var", NS);  \
    xmpp_stanza_add_child(query, feature);          \
    xmpp_stanza_release(feature);                   \
    weechat_string_dyn_concat(serial, NS, -1);      \
    weechat_string_dyn_concat(serial, "<", -1);

    FEATURE("eu.siacs.conversations.axolotl.devicelist+notify");
    FEATURE("http://jabber.org/protocol/caps");
    FEATURE("http://jabber.org/protocol/chatstates");
    FEATURE("http://jabber.org/protocol/disco#info");
    FEATURE("http://jabber.org/protocol/disco#items");
    FEATURE("http://jabber.org/protocol/muc");
    FEATURE("http://jabber.org/protocol/nick+notify");
    FEATURE("http://jabber.org/protocol/pep");
    FEATURE("jabber:iq:version");
    FEATURE("jabber:x:conference");
    FEATURE("jabber:x:oob");
    FEATURE("storage:bookmarks+notify");
    FEATURE("urn:xmpp:avatar:metadata+notify");
    FEATURE("urn:xmpp:chat-markers:0");
    FEATURE("urn:xmpp:delay");  // XEP-0203: Delayed Delivery
    FEATURE("urn:xmpp:hints");  // XEP-0334: Message Processing Hints
    FEATURE("urn:xmpp:idle:1");
  //FEATURE("urn:xmpp:jingle-message:0");
  //FEATURE("urn:xmpp:jingle:1");
  //FEATURE("urn:xmpp:jingle:apps:dtls:0");
  //FEATURE("urn:xmpp:jingle:apps:file-transfer:3");
  //FEATURE("urn:xmpp:jingle:apps:file-transfer:4");
  //FEATURE("urn:xmpp:jingle:apps:file-transfer:5");
  //FEATURE("urn:xmpp:jingle:apps:rtp:1");
  //FEATURE("urn:xmpp:jingle:apps:rtp:audio");
  //FEATURE("urn:xmpp:jingle:apps:rtp:video");
  //FEATURE("urn:xmpp:jingle:jet-omemo:0");
  //FEATURE("urn:xmpp:jingle:jet:0");
  //FEATURE("urn:xmpp:jingle:transports:ibb:1");
  //FEATURE("urn:xmpp:jingle:transports:ice-udp:1");
  //FEATURE("urn:xmpp:jingle:transports:s5b:1");
    FEATURE("urn:xmpp:message-correct:0");
    FEATURE("urn:xmpp:message-retract:1");
    FEATURE("urn:xmpp:message-moderate:1");  // XEP-0425: Message Moderation
    FEATURE("urn:xmpp:fasten:0");  // XEP-0422: Message Fastening (used by moderation)
    FEATURE("urn:xmpp:reactions:0");  // XEP-0444: Message Reactions
    FEATURE("urn:xmpp:reply:0");  // XEP-0461: Message Replies
    FEATURE("urn:xmpp:sid:0");  // XEP-0359: Stanza IDs
    FEATURE("urn:xmpp:styling:0");
    FEATURE("urn:xmpp:eme:0");  // XEP-0380: Explicit Message Encryption
    FEATURE("http://jabber.org/protocol/mood");  // XEP-0107: User Mood
    FEATURE("http://jabber.org/protocol/mood+notify");  // Subscribe to mood updates
    FEATURE("http://jabber.org/protocol/activity");  // XEP-0108: User Activity
    FEATURE("http://jabber.org/protocol/activity+notify");  // Subscribe to activity updates
    FEATURE("urn:xmpp:bookmarks:1");  // XEP-0402: PEP Native Bookmarks
    FEATURE("urn:xmpp:bookmarks:1+notify");  // Subscribe to bookmark updates
    FEATURE("urn:xmpp:ping");
    FEATURE("urn:xmpp:receipts");
    FEATURE("urn:xmpp:time");
#undef FEATURE

    xmpp_stanza_t *x = xmpp_stanza_new(account.context);
    xmpp_stanza_set_name(x, "x");
    xmpp_stanza_set_ns(x, "jabber:x:data");
    xmpp_stanza_set_attribute(x, "type", "result");

    static struct utsname osinfo;
    if (uname(&osinfo) < 0)
    {
        *osinfo.sysname = 0;
        *osinfo.release = 0;
    }

    xmpp_stanza_t *field, *value, *text;
    // This is utter bullshit, TODO: anything but this
#define FEATURE1(VAR, TYPE, VALUE)                            \
    field = xmpp_stanza_new(account.context);                 \
    xmpp_stanza_set_name(field, "field");                     \
    xmpp_stanza_set_attribute(field, "var", VAR);             \
    if(TYPE) xmpp_stanza_set_attribute(field, "type", TYPE);  \
    value = xmpp_stanza_new(account.context);                 \
    xmpp_stanza_set_name(value, "value");                     \
    text = xmpp_stanza_new(account.context);                  \
    xmpp_stanza_set_text(text, VALUE);                        \
    xmpp_stanza_add_child(value, text);                       \
    xmpp_stanza_release(text);                                \
    xmpp_stanza_add_child(field, value);                      \
    xmpp_stanza_release(value);                               \
    xmpp_stanza_add_child(x, field);                          \
    xmpp_stanza_release(field);                               \
    if (strcmp(VAR, "FORM_TYPE") == 0) {                      \
        weechat_string_dyn_concat(serial, VAR, -1);           \
        weechat_string_dyn_concat(serial, "<", -1);           \
    }                                                         \
    weechat_string_dyn_concat(serial, VALUE, -1);             \
    weechat_string_dyn_concat(serial, "<", -1);
#define FEATURE2(VAR, TYPE, VALUE1, VALUE2)                   \
    field = xmpp_stanza_new(account.context);                 \
    xmpp_stanza_set_name(field, "field");                     \
    xmpp_stanza_set_attribute(field, "var", VAR);             \
    xmpp_stanza_set_attribute(field, "type", TYPE);           \
    value = xmpp_stanza_new(account.context);                 \
    xmpp_stanza_set_name(value, "value");                     \
    text = xmpp_stanza_new(account.context);                  \
    xmpp_stanza_set_text(text, VALUE1);                       \
    xmpp_stanza_add_child(value, text);                       \
    xmpp_stanza_release(text);                                \
    xmpp_stanza_add_child(field, value);                      \
    xmpp_stanza_release(value);                               \
    value = xmpp_stanza_new(account.context);                 \
    xmpp_stanza_set_name(value, "value");                     \
    text = xmpp_stanza_new(account.context);                  \
    xmpp_stanza_set_text(text, VALUE2);                       \
    xmpp_stanza_add_child(value, text);                       \
    xmpp_stanza_release(text);                                \
    xmpp_stanza_add_child(field, value);                      \
    xmpp_stanza_release(value);                               \
    xmpp_stanza_add_child(x, field);                          \
    xmpp_stanza_release(field);                               \
    weechat_string_dyn_concat(serial, VAR, -1);               \
    weechat_string_dyn_concat(serial, "<", -1);               \
    weechat_string_dyn_concat(serial, VALUE1, -1);            \
    weechat_string_dyn_concat(serial, "<", -1);               \
    weechat_string_dyn_concat(serial, VALUE2, -1);            \
    weechat_string_dyn_concat(serial, "<", -1);

    FEATURE1("FORM_TYPE", "hidden", "urn:xmpp:dataforms:softwareinfo");
    FEATURE2("ip_version", "text-multi", "ipv4", "ipv6");
    FEATURE1("os", NULL, osinfo.sysname);
    FEATURE1("os_version", NULL, osinfo.release);
    FEATURE1("software", NULL, "weechat");
    FEATURE1("software_version", NULL, weechat_info_get("version", NULL));
#undef FEATURE1
#undef FEATURE2

    xmpp_stanza_add_child(query, x);
    xmpp_stanza_release(x);

    xmpp_stanza_set_type(reply, "result");
    xmpp_stanza_add_child(reply, query);

    unsigned char digest[20];
    xmpp_sha1_t *sha1 = xmpp_sha1_new(account.context);
    xmpp_sha1_update(sha1, (unsigned char*)*serial, strlen(*serial));
    xmpp_sha1_final(sha1);
    weechat_string_dyn_free(serial, 1);
    xmpp_sha1_to_digest(sha1, digest);
    xmpp_sha1_free(sha1);

    if (hash)
    {
        char *cap_hash = xmpp_base64_encode(account.context, digest, 20);
        *hash = strdup(cap_hash);
        xmpp_free(account.context, cap_hash);
    }

    return reply;
}

bool weechat::connection::iq_handler(xmpp_stanza_t *stanza, bool /* top_level */)
{
    // SM counter incremented in libstrophe wrapper, not here
    // top_level parameter kept for nested/recursive calls

    xmpp_stanza_t *reply, *query, *text, *fin;
    xmpp_stanza_t         *pubsub, *items, *item, *list, *bundle, *device;
    xmpp_stanza_t         *storage, *conference, *nick;

    auto binding = xml::iq(account.context, stanza);
    const char *id = xmpp_stanza_get_id(stanza);
    const char *from = xmpp_stanza_get_from(stanza);
    const char *to = xmpp_stanza_get_to(stanza);
    const char *type = xmpp_stanza_get_attribute(stanza, "type");
    
    // Handle XMPP Ping (XEP-0199)
    xmpp_stanza_t *ping = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "ping", "urn:xmpp:ping");
    if (ping && type && weechat_strcasecmp(type, "get") == 0)
    {
        // Respond with iq result
        reply = xmpp_iq_new(account.context, "result", id);
        xmpp_stanza_set_to(reply, from);
        if (to)
            xmpp_stanza_set_from(reply, to);
        
        account.connection.send(reply);
        xmpp_stanza_release(reply);
        return true;
    }
    
    // Handle ping responses (XEP-0199 and XEP-0410)
    if (type && (weechat_strcasecmp(type, "result") == 0 || weechat_strcasecmp(type, "error") == 0))
    {
        const char *stanza_id = xmpp_stanza_get_id(stanza);
        if (stanza_id && account.user_ping_queries.count(stanza_id))
        {
            time_t start_time = account.user_ping_queries[stanza_id];
            time_t now = time(NULL);
            long rtt_ms = (now - start_time) * 1000;  // Convert to milliseconds
            
            account.user_ping_queries.erase(stanza_id);
            
            const char *from_jid = from ? from : account.jid().data();
            
            // Check if this is a MUC self-ping (XEP-0410)
            bool is_muc_selfping = false;
            std::string room_jid;
            if (from)
            {
                std::string from_str(from);
                size_t slash_pos = from_str.find('/');
                if (slash_pos != std::string::npos)
                {
                    room_jid = from_str.substr(0, slash_pos);
                    std::string resource = from_str.substr(slash_pos + 1);
                    
                    // Check if this is our own nickname in a MUC
                    if (resource == account.nickname())
                    {
                        // Check if we have a channel for this room
                        if (account.channels.find(room_jid) != account.channels.end())
                        {
                            is_muc_selfping = true;
                        }
                    }
                }
            }
            
            if (weechat_strcasecmp(type, "result") == 0)
            {
                if (is_muc_selfping)
                {
                    weechat_printf(account.buffer, "%sMUC self-ping OK: still in %s",
                                  weechat_prefix("network"), room_jid.c_str());
                }
                else
                {
                    weechat_printf(account.buffer, "%sPong from %s (RTT: %ld ms)",
                                  weechat_prefix("network"), from_jid, rtt_ms);
                }
            }
            else
            {
                // Error response
                if (is_muc_selfping)
                {
                    weechat_printf(account.buffer, "%sMUC self-ping FAILED: no longer in %s",
                                  weechat_prefix("error"), room_jid.c_str());
                }
                else
                {
                    xmpp_stanza_t *error = xmpp_stanza_get_child_by_name(stanza, "error");
                    const char *error_type = error ? xmpp_stanza_get_attribute(error, "type") : "unknown";
                    weechat_printf(account.buffer, "%sPing failed to %s: %s",
                                  weechat_prefix("error"), from_jid, error_type);
                }
            }
            return true;
        }
    }
    
    // Handle vCard responses (XEP-0054)
    xmpp_stanza_t *vcard = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "vCard", "vcard-temp");
    if (vcard && type && weechat_strcasecmp(type, "result") == 0)
    {
        const char *from_jid = from ? from : account.jid().data();
        
        weechat_printf(account.buffer, "%svCard for %s:",
                      weechat_prefix("network"), from_jid);
        
        // Extract and display vCard fields
        xmpp_stanza_t *fn = xmpp_stanza_get_child_by_name(vcard, "FN");
        if (fn)
        {
            char *fn_text = xmpp_stanza_get_text(fn);
            if (fn_text)
            {
                weechat_printf(account.buffer, "  Name: %s", fn_text);
                xmpp_free(account.context, fn_text);
            }
        }
        
        xmpp_stanza_t *nickname = xmpp_stanza_get_child_by_name(vcard, "NICKNAME");
        if (nickname)
        {
            char *nick_text = xmpp_stanza_get_text(nickname);
            if (nick_text)
            {
                weechat_printf(account.buffer, "  Nickname: %s", nick_text);
                xmpp_free(account.context, nick_text);
            }
        }
        
        xmpp_stanza_t *email = xmpp_stanza_get_child_by_name(vcard, "EMAIL");
        if (email)
        {
            xmpp_stanza_t *userid = xmpp_stanza_get_child_by_name(email, "USERID");
            if (userid)
            {
                char *email_text = xmpp_stanza_get_text(userid);
                if (email_text)
                {
                    weechat_printf(account.buffer, "  Email: %s", email_text);
                    xmpp_free(account.context, email_text);
                }
            }
        }
        
        xmpp_stanza_t *url = xmpp_stanza_get_child_by_name(vcard, "URL");
        if (url)
        {
            char *url_text = xmpp_stanza_get_text(url);
            if (url_text)
            {
                weechat_printf(account.buffer, "  URL: %s", url_text);
                xmpp_free(account.context, url_text);
            }
        }
        
        xmpp_stanza_t *desc = xmpp_stanza_get_child_by_name(vcard, "DESC");
        if (desc)
        {
            char *desc_text = xmpp_stanza_get_text(desc);
            if (desc_text)
            {
                weechat_printf(account.buffer, "  Description: %s", desc_text);
                xmpp_free(account.context, desc_text);
            }
        }
        
        return true;
    }
    
    // XEP-0363: HTTP File Upload - handle upload slot response
    xmpp_stanza_t *slot = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "slot", "urn:xmpp:http:upload:0");
    
    if (slot && id && type && weechat_strcasecmp(type, "result") == 0)
    {
        weechat_printf(account.buffer, "%s[DEBUG] Upload slot response received (id: %s)",
                      weechat_prefix("network"), id);
        
        auto req_it = account.upload_requests.find(id);
        if (req_it != account.upload_requests.end())
        {
            weechat_printf(account.buffer, "%s[DEBUG] Found matching upload request",
                          weechat_prefix("network"));
            // Extract PUT and GET URLs
            xmpp_stanza_t *put_elem = xmpp_stanza_get_child_by_name(slot, "put");
            xmpp_stanza_t *get_elem = xmpp_stanza_get_child_by_name(slot, "get");
            
            const char *put_url = put_elem ? xmpp_stanza_get_attribute(put_elem, "url") : NULL;
            const char *get_url = get_elem ? xmpp_stanza_get_attribute(get_elem, "url") : NULL;
            
            weechat_printf(account.buffer, "%s[DEBUG] PUT URL: %s",
                          weechat_prefix("network"), put_url ? put_url : "(null)");
            weechat_printf(account.buffer, "%s[DEBUG] GET URL: %s",
                          weechat_prefix("network"), get_url ? get_url : "(null)");
            
            // Extract PUT headers (XEP-0363 allows Authorization and other headers)
            std::vector<std::string> put_headers;
            if (put_elem)
            {
                xmpp_stanza_t *header = xmpp_stanza_get_child_by_name(put_elem, "header");
                while (header)
                {
                    const char *name = xmpp_stanza_get_attribute(header, "name");
                    char *value = xmpp_stanza_get_text(header);
                    if (name && value)
                    {
                        std::string header_str = fmt::format("{}: {}", name, value);
                        put_headers.push_back(header_str);
                        weechat_printf(account.buffer, "%s[DEBUG] PUT header: %s",
                                      weechat_prefix("network"), header_str.c_str());
                    }
                    if (value) xmpp_free(account.context, value);
                    header = xmpp_stanza_get_next(header);
                }
            }
            
            if (put_url && get_url)
            {
                weechat_printf(account.buffer, "%sUpload slot received, uploading file...",
                              weechat_prefix("network"));
                
                // Verify file exists and get file size
                FILE *file = fopen(req_it->second.filepath.c_str(), "rb");
                if (!file)
                {
                    weechat_printf(account.buffer, "%s%s: failed to open file for upload: %s",
                                  weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                                  req_it->second.filepath.c_str());
                    account.upload_requests.erase(req_it);
                    return 1;
                }
                fclose(file);
                
                // Get Content-Type from filename extension
                std::string filename = req_it->second.filename;
                std::string content_type = "application/octet-stream";
                size_t dot_pos = filename.find_last_of('.');
                if (dot_pos != std::string::npos)
                {
                    std::string ext = filename.substr(dot_pos + 1);
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    
                    if (ext == "jpg" || ext == "jpeg") content_type = "image/jpeg";
                    else if (ext == "png") content_type = "image/png";
                    else if (ext == "gif") content_type = "image/gif";
                    else if (ext == "webp") content_type = "image/webp";
                    else if (ext == "mp4") content_type = "video/mp4";
                    else if (ext == "webm") content_type = "video/webm";
                    else if (ext == "pdf") content_type = "application/pdf";
                    else if (ext == "txt") content_type = "text/plain";
                }
                
                // Use libcurl for HTTP PUT upload
                // Open file for reading
                FILE *upload_file = fopen(req_it->second.filepath.c_str(), "rb");
                if (!upload_file)
                {
                    weechat_printf(account.buffer, "%s%s: failed to open file for upload",
                                  weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                    account.upload_requests.erase(req_it);
                    return 1;
                }
                
                // Get file size
                fseek(upload_file, 0, SEEK_END);
                long file_size = ftell(upload_file);
                fseek(upload_file, 0, SEEK_SET);
                
                // Calculate SHA-256 hash for SIMS using modern EVP API
                unsigned char hash[EVP_MAX_MD_SIZE];
                unsigned int hash_len = 0;
                EVP_MD_CTX *sha256_ctx = EVP_MD_CTX_new();
                EVP_DigestInit_ex(sha256_ctx, EVP_sha256(), nullptr);
                
                unsigned char buffer[8192];
                size_t bytes_read;
                while ((bytes_read = fread(buffer, 1, sizeof(buffer), upload_file)) > 0)
                {
                    EVP_DigestUpdate(sha256_ctx, buffer, bytes_read);
                }
                EVP_DigestFinal_ex(sha256_ctx, hash, &hash_len);
                EVP_MD_CTX_free(sha256_ctx);
                
                // Base64 encode the hash using OpenSSL
                BIO *bio = BIO_new(BIO_s_mem());
                BIO *b64 = BIO_new(BIO_f_base64());
                BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
                bio = BIO_push(b64, bio);
                BIO_write(bio, hash, hash_len);
                BIO_flush(bio);
                
                BUF_MEM *buffer_ptr;
                BIO_get_mem_ptr(bio, &buffer_ptr);
                std::string sha256_b64(buffer_ptr->data, buffer_ptr->length);
                BIO_free_all(bio);
                
                // Store hash in upload request for later use
                req_it->second.sha256_hash = sha256_b64;
                
                // Reset file position for upload
                fseek(upload_file, 0, SEEK_SET);
                
                // Initialize curl
                CURL *curl = curl_easy_init();
                if (!curl)
                {
                    fclose(upload_file);
                    weechat_printf(account.buffer, "%s%s: failed to initialize curl",
                                  weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                    account.upload_requests.erase(req_it);
                    return 1;
                }
                
                // Set up curl options
                curl_easy_setopt(curl, CURLOPT_URL, put_url);
                curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
                curl_easy_setopt(curl, CURLOPT_READDATA, upload_file);
                curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)file_size);
                
                // Build header list
                struct curl_slist *headers = NULL;
                headers = curl_slist_append(headers, fmt::format("Content-Type: {}", content_type).c_str());
                for (const auto& header : put_headers)
                {
                    headers = curl_slist_append(headers, header.c_str());
                }
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                
                weechat_printf(account.buffer, "%s[DEBUG] Uploading %ld bytes with libcurl",
                              weechat_prefix("network"), file_size);
                
                // Perform the upload
                CURLcode res = curl_easy_perform(curl);
                
                long http_code = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
                
                // Cleanup
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
                fclose(upload_file);
                
                if (res != CURLE_OK || http_code != 201)
                {
                    weechat_printf(account.buffer, "%s%s: file upload failed (HTTP %ld): %s",
                                  weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                                  http_code, curl_easy_strerror(res));
                }
                else
                {
                    weechat_printf(account.buffer, "%sFile uploaded successfully!",
                                  weechat_prefix("network"));
                    
                    // Send message with SIMS metadata
                    auto channel_it = account.channels.find(req_it->second.channel_id);
                    if (channel_it != account.channels.end())
                    {
                        // Build file metadata for SIMS
                        weechat::channel::file_metadata file_meta;
                        file_meta.filename = req_it->second.filename;
                        file_meta.content_type = req_it->second.content_type;
                        file_meta.size = req_it->second.file_size;
                        file_meta.sha256_hash = req_it->second.sha256_hash;
                        
                        channel_it->second.send_message(
                            channel_it->second.id,
                            get_url,
                            std::optional<std::string>(get_url),
                            std::optional<weechat::channel::file_metadata>(file_meta)
                        );
                    }
                }
                
                account.upload_requests.erase(req_it);
            }
            else
            {
                weechat_printf(account.buffer, "%s%s: upload failed - missing PUT or GET URL",
                              weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                account.upload_requests.erase(req_it);
            }
        }
        else
        {
            weechat_printf(account.buffer, "%s[DEBUG] Upload request ID not found: %s",
                          weechat_prefix("error"), id);
        }
    }
    else if (id && account.upload_requests.count(id))
    {
        weechat_printf(account.buffer, "%s[DEBUG] Slot response but no slot element or wrong type (type: %s)",
                      weechat_prefix("error"), type ? type : "(null)");
    }
    
    // XEP-0363: HTTP File Upload - handle upload slot errors
    if (id && type && weechat_strcasecmp(type, "error") == 0)
    {
        auto req_it = account.upload_requests.find(id);
        if (req_it != account.upload_requests.end())
        {
            xmpp_stanza_t *error_elem = xmpp_stanza_get_child_by_name(stanza, "error");
            const char *error_type = error_elem ? xmpp_stanza_get_attribute(error_elem, "type") : NULL;
            
            // Try to get error text
            std::string error_msg = "Upload slot request failed";
            if (error_elem)
            {
                xmpp_stanza_t *text_elem = xmpp_stanza_get_child_by_name(error_elem, "text");
                if (text_elem)
                {
                    char *text = xmpp_stanza_get_text(text_elem);
                    if (text)
                    {
                        error_msg = fmt::format("Upload slot request failed: {}", text);
                        xmpp_free(account.context, text);
                    }
                }
                else
                {
                    // Try to get error condition
                    xmpp_stanza_t *child = xmpp_stanza_get_children(error_elem);
                    while (child)
                    {
                        const char *name = xmpp_stanza_get_name(child);
                        if (name && strcmp(name, "text") != 0)
                        {
                            error_msg = fmt::format("Upload slot request failed: {}", name);
                            break;
                        }
                        child = xmpp_stanza_get_next(child);
                    }
                }
            }
            
            weechat_printf(account.buffer, "%s%s: %s (type: %s)",
                          weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                          error_msg.c_str(), error_type ? error_type : "unknown");
            
            account.upload_requests.erase(req_it);
        }
    }
    
    // XEP-0191: Blocking Command
    xmpp_stanza_t *blocklist = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "blocklist", "urn:xmpp:blocking");
    xmpp_stanza_t *block = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "block", "urn:xmpp:blocking");
    xmpp_stanza_t *unblock = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "unblock", "urn:xmpp:blocking");
    
    if (blocklist && type && weechat_strcasecmp(type, "result") == 0)
    {
        // Handle blocklist response
        xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(blocklist, "item");
        
        if (item)
        {
            weechat_printf(account.buffer, "%sBlocked JIDs:",
                          weechat_prefix("network"));
            
            while (item)
            {
                const char *jid = xmpp_stanza_get_attribute(item, "jid");
                if (jid)
                {
                    weechat_printf(account.buffer, "  %s", jid);
                }
                item = xmpp_stanza_get_next(item);
            }
        }
        else
        {
            weechat_printf(account.buffer, "%sNo JIDs blocked",
                          weechat_prefix("network"));
        }
        
        return true;
    }
    
    if (block && type && weechat_strcasecmp(type, "result") == 0)
    {
        weechat_printf(account.buffer, "%sBlock request successful",
                      weechat_prefix("network"));
        return true;
    }
    
    if (unblock && type && weechat_strcasecmp(type, "result") == 0)
    {
        weechat_printf(account.buffer, "%sUnblock request successful",
                      weechat_prefix("network"));
        return true;
    }
    
    // XEP-0030: Service Discovery - disco#items response
    xmpp_stanza_t *items_query = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "query", "http://jabber.org/protocol/disco#items");
    
    if (items_query && type && weechat_strcasecmp(type, "result") == 0)
    {
        // Look for HTTP upload service in items
        xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(items_query, "item");
        while (item)
        {
            const char *item_jid = xmpp_stanza_get_attribute(item, "jid");
            if (item_jid)
            {
                // Query this item for its features
                char *disco_info_id = xmpp_uuid_gen(account.context);
                account.upload_disco_queries[disco_info_id] = item_jid;
                
                account.connection.send(stanza::iq()
                            .from(account.jid())
                            .to(item_jid)
                            .type("get")
                            .id(disco_info_id)
                            .xep0030()
                            .query()
                            .build(account.context)
                            .get());
                
                xmpp_free(account.context, disco_info_id);
            }
            item = xmpp_stanza_get_next(item);
        }
    }
    
    query = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "query", "http://jabber.org/protocol/disco#info");
    if (query && type)
    {
        if (weechat_strcasecmp(type, "get") == 0)
        {
            reply = get_caps(xmpp_stanza_reply(stanza), NULL);

            account.connection.send(reply);
            xmpp_stanza_release(reply);
        }

        if (weechat_strcasecmp(type, "result") == 0)
        {
            const char *stanza_id = xmpp_stanza_get_id(stanza);
            bool user_initiated = stanza_id && account.user_disco_queries.count(stanza_id);
            bool caps_query = stanza_id && account.caps_disco_queries.count(stanza_id);
            
            // Extract features for capability caching
            std::vector<std::string> features;
            if (caps_query)
            {
                xmpp_stanza_t *feature = xmpp_stanza_get_child_by_name(query, "feature");
                while (feature)
                {
                    const char *var = xmpp_stanza_get_attribute(feature, "var");
                    if (var)
                        features.push_back(var);
                    feature = xmpp_stanza_get_next(feature);
                }
                
                // Save to capability cache
                std::string ver_hash = account.caps_disco_queries[stanza_id];
                account.caps_cache_save(ver_hash, features);
                account.caps_disco_queries.erase(stanza_id);
            }
            
            // Check if this is a response to upload service discovery
            bool upload_disco = stanza_id && account.upload_disco_queries.count(stanza_id);
            if (upload_disco)
            {
                std::string service_jid = account.upload_disco_queries[stanza_id];
                account.upload_disco_queries.erase(stanza_id);
                
                // Check if this service supports HTTP File Upload
                bool supports_upload = false;
                size_t max_size = 0;
                
                xmpp_stanza_t *feature = xmpp_stanza_get_child_by_name(query, "feature");
                while (feature)
                {
                    const char *var = xmpp_stanza_get_attribute(feature, "var");
                    if (var && strcmp(var, "urn:xmpp:http:upload:0") == 0)
                    {
                        supports_upload = true;
                    }
                    feature = xmpp_stanza_get_next(feature);
                }
                
                // Check for max file size in x data form
                if (supports_upload)
                {
                    xmpp_stanza_t *x = xmpp_stanza_get_child_by_name_and_ns(query, "x", "jabber:x:data");
                    if (x)
                    {
                        xmpp_stanza_t *field = xmpp_stanza_get_child_by_name(x, "field");
                        while (field)
                        {
                            const char *var = xmpp_stanza_get_attribute(field, "var");
                            if (var && strcmp(var, "max-file-size") == 0)
                            {
                                xmpp_stanza_t *value = xmpp_stanza_get_child_by_name(field, "value");
                                if (value)
                                {
                                    char *value_text = xmpp_stanza_get_text(value);
                                    if (value_text)
                                    {
                                        max_size = strtoull(value_text, NULL, 10);
                                        xmpp_free(account.context, value_text);
                                    }
                                }
                            }
                            field = xmpp_stanza_get_next(field);
                        }
                    }
                    
                    account.upload_service = service_jid;
                    account.upload_max_size = max_size;
                    
                    if (max_size > 0)
                    {
                        weechat_printf(account.buffer, "%sDiscovered upload service: %s (max: %zu MB)",
                                      weechat_prefix("network"), service_jid.c_str(),
                                      max_size / (1024 * 1024));
                    }
                    else
                    {
                        weechat_printf(account.buffer, "%sDiscovered upload service: %s",
                                      weechat_prefix("network"), service_jid.c_str());
                    }
                }
            }
            
            if (user_initiated)
            {
                account.user_disco_queries.erase(stanza_id);
                
                const char *from_jid = xmpp_stanza_get_from(stanza);
                struct t_gui_buffer *output_buffer = account.buffer;
                
                weechat_printf(output_buffer, "");
                weechat_printf(output_buffer, "%sService Discovery for %s%s:", 
                              weechat_color("chat_prefix_network"),
                              weechat_color("chat_server"), 
                              from_jid ? from_jid : "server");
            }
            
            xmpp_stanza_t *identity = xmpp_stanza_get_child_by_name(query, "identity");
            while (identity)
            {
                std::string category;
                std::string name;
                std::string type;

                if (const char *attr = xmpp_stanza_get_attribute(identity, "category"))
                    category = attr;
                if (const char *attr = xmpp_stanza_get_attribute(identity, "name"))
                    name = unescape(attr);
                if (const char *attr = xmpp_stanza_get_attribute(identity, "type"))
                    type = attr;

                if (user_initiated)
                {
                    weechat_printf(account.buffer, "  %sIdentity:%s %s/%s %s%s%s",
                                  weechat_color("chat_prefix_network"),
                                  weechat_color("reset"),
                                  category.c_str(), type.c_str(),
                                  weechat_color("chat_delimiters"),
                                  name.empty() ? "" : name.c_str(),
                                  weechat_color("reset"));
                }

                if (category == "conference")
                {
                    auto ptr_channel = account.channels.find(from);
                    if (ptr_channel != account.channels.end())
                        ptr_channel->second.update_name(name.data());
                }
                else if (category == "conference")
                {
                    xmpp_stanza_t *children[2] = {NULL};
                    children[0] = stanza__iq_pubsub_items(account.context, NULL,
                            const_cast<char*>("eu.siacs.conversations.axolotl.devicelist"));
                    children[0] = stanza__iq_pubsub(account.context, NULL,
                            children, with_noop("http://jabber.org/protocol/pubsub"));
                    children[0] = stanza__iq(account.context, NULL, children, NULL,
                            strdup("fetch2"), to ? strdup(to) : NULL,
                            binding.from ? strdup(binding.from->bare.data()) : NULL, strdup("get"));
                    account.connection.send(children[0]);
                    xmpp_stanza_release(children[0]);
                }
                
                identity = xmpp_stanza_get_next(identity);
            }
            
            if (user_initiated)
            {
                xmpp_stanza_t *feature = xmpp_stanza_get_child_by_name(query, "feature");
                if (feature)
                {
                    weechat_printf(account.buffer, "  %sFeatures:",
                                  weechat_color("chat_prefix_network"));
                    while (feature)
                    {
                        const char *var = xmpp_stanza_get_attribute(feature, "var");
                        if (var)
                            weechat_printf(account.buffer, "    %s", var);
                        feature = xmpp_stanza_get_next(feature);
                    }
                }
            }
        }
    }
    
    // Handle roster (RFC 6121)
    query = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "query", "jabber:iq:roster");
    if (query && type && weechat_strcasecmp(type, "result") == 0)
    {
        xmpp_stanza_t *item;
        for (item = xmpp_stanza_get_children(query);
             item; item = xmpp_stanza_get_next(item))
        {
            const char *name = xmpp_stanza_get_name(item);
            if (weechat_strcasecmp(name, "item") != 0)
                continue;

            const char *jid = xmpp_stanza_get_attribute(item, "jid");
            const char *roster_name = xmpp_stanza_get_attribute(item, "name");
            const char *subscription = xmpp_stanza_get_attribute(item, "subscription");
            
            if (!jid)
                continue;

            account.roster[jid].jid = jid;
            account.roster[jid].name = roster_name ? roster_name : "";
            account.roster[jid].subscription = subscription ? subscription : "none";
            account.roster[jid].groups.clear();

            xmpp_stanza_t *group;
            for (group = xmpp_stanza_get_children(item);
                 group; group = xmpp_stanza_get_next(group))
            {
                const char *group_name = xmpp_stanza_get_name(group);
                if (weechat_strcasecmp(group_name, "group") != 0)
                    continue;

                xmpp_stanza_t *group_text = xmpp_stanza_get_children(group);
                if (group_text)
                {
                    char *text = xmpp_stanza_get_text(group_text);
                    if (text)
                    {
                        account.roster[jid].groups.push_back(text);
                        xmpp_free(account.context, text);
                    }
                }
            }
        }
    }
    
    query = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "query", "jabber:iq:private");
    if (query && type)
    {
        storage = xmpp_stanza_get_child_by_name_and_ns(
                query, "storage", "storage:bookmarks");
        if (storage)
        {
            // Clear existing bookmarks
            account.bookmarks.clear();
            
            for (conference = xmpp_stanza_get_children(storage);
                 conference; conference = xmpp_stanza_get_next(conference))
            {
                const char *name = xmpp_stanza_get_name(conference);
                if (weechat_strcasecmp(name, "conference") != 0)
                    continue;

                const char *jid = xmpp_stanza_get_attribute(conference, "jid");
                const char *autojoin = xmpp_stanza_get_attribute(conference, "autojoin");
                name = xmpp_stanza_get_attribute(conference, "name");
                nick = xmpp_stanza_get_child_by_name(conference, "nick");
                char *intext = NULL;
                if (nick)
                {
                    text = xmpp_stanza_get_children(nick);
                    intext = xmpp_stanza_get_text(text);
                }
                
                if (!jid)
                    continue;

                // Store bookmark
                account.bookmarks[jid].jid = jid;
                account.bookmarks[jid].name = name ? name : "";
                account.bookmarks[jid].nick = intext ? intext : "";
                account.bookmarks[jid].autojoin = autojoin && weechat_strcasecmp(autojoin, "true") == 0;

                account.connection.send(stanza::iq()
                            .from(to)
                            .to(jid)
                            .type("get")
                            .id(stanza::uuid(account.context))
                            .xep0030()
                            .query()
                            .build(account.context)
                            .get());
                if (weechat_strcasecmp(autojoin, "true") == 0)
                {
                    // Skip autojoin for biboumi (IRC gateway) rooms
                    // Biboumi JIDs typically contain % (e.g., #channel%irc.server.org@gateway)
                    // or have 'biboumi' in the server component
                    bool is_biboumi = false;
                    if (jid)
                    {
                        is_biboumi = (strchr(jid, '%') != NULL) ||
                                    (strstr(jid, "biboumi") != NULL) ||
                                    (strstr(jid, "@irc.") != NULL);
                    }
                    
                    if (is_biboumi)
                    {
                        weechat_printf(account.buffer, 
                                      "%sSkipping autojoin for IRC gateway room: %s",
                                      weechat_prefix("network"), jid);
                    }
                    else
                    {
                        char **command = weechat_string_dyn_alloc(256);
                        weechat_string_dyn_concat(command, "/enter ", -1);
                        weechat_string_dyn_concat(command, jid, -1);
                        if (nick)
                        {
                            weechat_string_dyn_concat(command, "/", -1);
                            weechat_string_dyn_concat(command, intext, -1);
                        }
                        weechat_command(account.buffer, *command);
                        auto ptr_channel = account.channels.find(jid);
                        struct t_gui_buffer *ptr_buffer =
                            ptr_channel != account.channels.end()
                            ? ptr_channel->second.buffer : NULL;
                        if (ptr_buffer)
                            weechat_buffer_set(ptr_buffer, "short_name", name);
                        weechat_string_dyn_free(command, 1);
                    }
                }

                if (nick)
                    free(intext);
            }
        }
    }

    pubsub = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "pubsub", "http://jabber.org/protocol/pubsub");
    if (pubsub)
    {
        const char *items_node, *device_id;

        items = xmpp_stanza_get_child_by_name(pubsub, "items");
        if (items)
        {
            items_node = xmpp_stanza_get_attribute(items, "node");
            if (items_node
                && weechat_strcasecmp(items_node,
                                      "eu.siacs.conversations.axolotl.devicelist") == 0)
            {
                item = xmpp_stanza_get_child_by_name(items, "item");
                if (item)
                {
                    list = xmpp_stanza_get_child_by_name_and_ns(
                        item, "list", "eu.siacs.conversations.axolotl");
                    if (list && account.omemo)
                    {
                        account.omemo.handle_devicelist(
                            from ? from : account.jid().data(), items);

                        xmpp_stanza_t *children[2] = {NULL};
                        for (device = xmpp_stanza_get_children(list);
                             device; device = xmpp_stanza_get_next(device))
                        {
                            const char *name = xmpp_stanza_get_name(device);
                            if (weechat_strcasecmp(name, "device") != 0)
                                continue;

                            const char *device_id = xmpp_stanza_get_id(device);

                            char bundle_node[128] = {0};
                            snprintf(bundle_node, sizeof(bundle_node),
                                        "eu.siacs.conversations.axolotl.bundles:%s",
                                        device_id);

                            children[1] = NULL;
                            children[0] =
                            stanza__iq_pubsub_items(account.context, NULL,
                                                    strdup(bundle_node));
                            children[0] =
                            stanza__iq_pubsub(account.context, NULL, children,
                                                with_noop("http://jabber.org/protocol/pubsub"));
                            char *uuid = xmpp_uuid_gen(account.context);
                            children[0] =
                            stanza__iq(account.context, NULL, children, NULL, uuid,
                                to ? strdup(to) : NULL, from ? strdup(from) : NULL,
                                strdup("get"));
                            xmpp_free(account.context, uuid);

                            account.connection.send(children[0]);
                            xmpp_stanza_release(children[0]);
                        }

                        if (from && account.jid() == from)
                        {
                            weechat::account::device dev;
                            char id[64] = {0};

                            account.devices.clear();

                            dev.id = account.omemo.device_id;
                            snprintf(id, sizeof(id), "%d", dev.id);
                            dev.name = id;
                            dev.label = "weechat";
                            account.devices.emplace(dev.id, dev);

                            for (device = xmpp_stanza_get_children(list);
                                 device; device = xmpp_stanza_get_next(device))
                            {
                                const char *name = xmpp_stanza_get_name(device);
                                if (weechat_strcasecmp(name, "device") != 0)
                                    continue;

                                device_id = xmpp_stanza_get_id(device);

                                dev.id = atoi(device_id);
                                dev.name = device_id;
                                dev.label = "";
                                account.devices.emplace(dev.id, dev);
                            }

                            reply = account.get_devicelist();
                            char *uuid = xmpp_uuid_gen(account.context);
                            xmpp_stanza_set_id(reply, uuid);
                            xmpp_free(account.context, uuid);
                            xmpp_stanza_set_attribute(reply, "to", from);
                            xmpp_stanza_set_attribute(reply, "from", to);
                            account.connection.send(reply);
                            xmpp_stanza_release(reply);
                        }
                    }
                }
            }
            if (items_node
                && strncmp(items_node,
                           "eu.siacs.conversations.axolotl.bundles",
                           strnlen(items_node,
                                   strlen("eu.siacs.conversations.axolotl.bundles"))) == 0)
            {
                item = xmpp_stanza_get_child_by_name(items, "item");
                if (item)
                {
                    bundle = xmpp_stanza_get_child_by_name_and_ns(item, "bundle", "eu.siacs.conversations.axolotl");
                    if (bundle)
                    {
                        size_t node_prefix =
                            strlen("eu.siacs.conversations.axolotl.bundles:");
                        if (account.omemo && strlen(items_node) > node_prefix)
                        {
                            account.omemo.handle_bundle(
                                from ? from : account.jid().data(),
                                                 strtol(items_node+node_prefix,
                                                        NULL, 10),
                                                 items);
                        }
                    }
                }
            }
        }
    }

    fin = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "fin", "urn:xmpp:mam:2");
    if (fin)
    {
        xmpp_stanza_t *set, *set__last;
        char *set__last__text;
        weechat::account::mam_query mam_query;

        set = xmpp_stanza_get_child_by_name_and_ns(
            fin, "set", "http://jabber.org/protocol/rsm");
        if (set && account.mam_query_search(&mam_query, id))
        {
            // Check if this is a global MAM query (empty 'with')
            bool is_global_query = mam_query.with.empty();
            
            auto channel = account.channels.find(mam_query.with.data());

            set__last = xmpp_stanza_get_child_by_name(set, "last");
            set__last__text = set__last
                ? xmpp_stanza_get_text(set__last) : NULL;

            if (channel != account.channels.end())
            {
                if (set__last__text)
                {
                    time_t *start_ptr = mam_query.start ? &*mam_query.start : nullptr;
                    time_t *end_ptr   = mam_query.end   ? &*mam_query.end   : nullptr;

                    channel->second.fetch_mam(id,
                                       start_ptr,
                                       end_ptr,
                                       set__last__text);
                }
                else
                {
                    // MAM fetch complete, update last fetch timestamp
                    channel->second.last_mam_fetch = time(NULL);
                    account.mam_cache_set_last_timestamp(channel->second.id, channel->second.last_mam_fetch);
                    weechat_printf(channel->second.buffer, 
                                  "%sMAM history loaded",
                                  weechat_prefix("network"));
                    account.mam_query_remove(mam_query.id);
                }
            }
            else if (is_global_query)
            {
                // Global MAM query complete - conversations were auto-created during message processing
                if (!set__last__text)
                {
                    weechat_printf(account.buffer, "%sMAM conversation discovery complete",
                                  weechat_prefix("network"));
                    account.mam_query_remove(mam_query.id);
                }
            }
            else
            {
                if (!set__last)
                    account.mam_query_remove(mam_query.id);
            }
        }
        else
        {
            weechat_printf(account.buffer, "MAM query not found or no set: id=%s, set=%s", 
                id ? id : "null", set ? "yes" : "no");
        }
    }

    return true;
}

bool weechat::connection::sm_handler(xmpp_stanza_t *stanza)
{
    // CRITICAL: Verify this is actually an SM stanza by checking xmlns
    const char *xmlns = xmpp_stanza_get_ns(stanza);
    if (!xmlns || strcmp(xmlns, "urn:xmpp:sm:3") != 0)
    {
        // Not an SM stanza, ignore
        return true;
    }

    const char *name = xmpp_stanza_get_name(stanza);
    if (!name)
        return true;

    std::string element_name(name);

    if (element_name == "enabled")
    {
        // Stream management successfully enabled
        account.sm_enabled = true;
        account.sm_h_inbound = 0;
        account.sm_h_outbound = 0;
        account.sm_last_ack = 0;

        const char *id = xmpp_stanza_get_attribute(stanza, "id");
        if (id)
        {
            account.sm_id = id;
            weechat_printf(account.buffer, "%sStream Management enabled (resumable, id=%s)",
                          weechat_prefix("network"), id);
        }
        else
        {
            weechat_printf(account.buffer, "%sStream Management enabled (not resumable)",
                          weechat_prefix("network"));
        }

        // Set up periodic ack timer (every 30 seconds)
        account.sm_ack_timer_hook = (struct t_hook *)weechat_hook_timer(30 * 1000, 0, 0,
                                   &account::sm_ack_timer_cb, &account, nullptr);
    }
    else if (element_name == "resumed")
    {
        // Stream resumed successfully
        const char *h = xmpp_stanza_get_attribute(stanza, "h");
        if (h)
        {
            account.sm_last_ack = std::stoul(h);
            weechat_printf(account.buffer, "%sStream resumed (h=%u)",
                          weechat_prefix("network"), account.sm_last_ack);
        }
        else
        {
            weechat_printf(account.buffer, "%sStream resumed",
                          weechat_prefix("network"));
        }
    }
    else if (element_name == "failed")
    {
        // Stream management failed or resume failed
        const char *xmlns_err = "urn:ietf:params:xml:ns:xmpp-stanzas";
        xmpp_stanza_t *error = xmpp_stanza_get_child_by_name_and_ns(stanza, "unexpected-request", xmlns_err);
        
        if (!error)
            error = xmpp_stanza_get_child_by_name_and_ns(stanza, "item-not-found", xmlns_err);
        
        if (error)
        {
            const char *error_name = xmpp_stanza_get_name(error);
            weechat_printf(account.buffer, "%sSM error: %s",
                          weechat_prefix("error"), error_name ? error_name : "unknown");
        }
        else
        {
            weechat_printf(account.buffer, "%sStream Management failed",
                          weechat_prefix("error"));
        }

        // Reset SM state (but don't try to enable again this session)
        account.sm_enabled = false;
        account.sm_id = "";
        account.sm_h_inbound = 0;
        account.sm_h_outbound = 0;
        account.sm_last_ack = 0;
        
        // Mark SM as unavailable to prevent retry loops
        // (Will be reset when user manually reconnects)
        account.sm_available = false;
        
        weechat_printf(account.buffer, "%sStream Management disabled for this session",
                      weechat_prefix("network"));
    }
    else if (element_name == "a")
    {
        // Acknowledgement from server
        const char *h = xmpp_stanza_get_attribute(stanza, "h");
        if (h)
        {
            uint32_t ack_count = std::stoul(h);
            account.sm_last_ack = ack_count;
            
            // Guard against underflow
            int32_t unacked = (int32_t)account.sm_h_outbound - (int32_t)ack_count;
            if (unacked < 0) unacked = 0;
            
            // Only log if there were unacked stanzas or if it's been a while
            static time_t last_ack_log = 0;
            time_t now = time(NULL);
            if (unacked > 0 || (now - last_ack_log) > 300)  // Log every 5 minutes if quiet
            {
                weechat_printf(account.buffer, "%sReceived ack: h=%u (sent=%u, unacked=%d)",
                              weechat_prefix("network"),
                              ack_count,
                              account.sm_h_outbound,
                              unacked);
                last_ack_log = now;
            }
        }
        else
        {
            weechat_printf(account.buffer, "%sSM error: 'a' stanza missing 'h' attribute",
                          weechat_prefix("error"));
        }
    }
    else if (element_name == "r")
    {
        // Server requests acknowledgement
        // Send answer with our current h value
        this->send(stanza::xep0198::answer(account.sm_h_inbound)
                  .build(account.context)
                  .get());
    }

    return true;
}

bool weechat::connection::conn_handler(event status, int error, xmpp_stream_error_t *stream_error)
{
    (void)error;
    (void)stream_error;

    if (status == event::connect)
    {
        account.disconnected = 0;

        xmpp_stanza_t *pres__c, *pres__status, *pres__status__text,
            *pres__x, *pres__x__text;

        // Only add handlers once (they persist across reconnects via libstrophe)
        if (!account.sm_handlers_registered)
        {
            this->handler_add<jabber::iq::version>(
                "iq", nullptr, [](xmpp_conn_t *conn, xmpp_stanza_t *stanza, void *userdata) {
                    auto& connection = *reinterpret_cast<weechat::connection*>(userdata);
                    if (connection != conn) throw std::invalid_argument("connection != conn");
                    return connection.version_handler(stanza) ? 1 : 0;
                });
            this->handler_add<urn::xmpp::time>(
                "iq", nullptr, [](xmpp_conn_t *conn, xmpp_stanza_t *stanza, void *userdata) {
                    auto& connection = *reinterpret_cast<weechat::connection*>(userdata);
                    if (connection != conn) throw std::invalid_argument("connection != conn");
                    return connection.time_handler(stanza) ? 1 : 0;
                });
            this->handler_add(
                "presence", nullptr, [](xmpp_conn_t *conn, xmpp_stanza_t *stanza, void *userdata) {
                    auto& connection = *reinterpret_cast<weechat::connection*>(userdata);
                    if (connection != conn) throw std::invalid_argument("connection != conn");
                    
                    // Increment SM counter for top-level stanzas only (called by libstrophe)
                    if (connection.account.sm_enabled)
                        connection.account.sm_h_inbound++;
                    
                    return connection.presence_handler(stanza, false) ? 1 : 0;  // Pass false since we already counted
                });
            this->handler_add(
                "message", /*type*/ nullptr, [](xmpp_conn_t *conn, xmpp_stanza_t *stanza, void *userdata) {
                    auto& connection = *reinterpret_cast<weechat::connection*>(userdata);
                    if (connection != conn) throw std::invalid_argument("connection != conn");
                    
                    // Increment SM counter for top-level stanzas only (called by libstrophe)
                    if (connection.account.sm_enabled)
                        connection.account.sm_h_inbound++;
                    
                    return connection.message_handler(stanza, false) ? 1 : 0;  // Pass false since we already counted
                });
            this->handler_add(
                "iq", nullptr, [](xmpp_conn_t *conn, xmpp_stanza_t *stanza, void *userdata) {
                    auto& connection = *reinterpret_cast<weechat::connection*>(userdata);
                    if (connection != conn) throw std::invalid_argument("connection != conn");
                    
                    // Increment SM counter for top-level stanzas only (called by libstrophe)
                    if (connection.account.sm_enabled)
                        connection.account.sm_h_inbound++;
                    
                    return connection.iq_handler(stanza, false) ? 1 : 0;  // Pass false since we already counted
                });

            // Stream Management handlers (XEP-0198)
            this->handler_add(
                "enabled", nullptr, [](xmpp_conn_t *conn, xmpp_stanza_t *stanza, void *userdata) {
                    auto& connection = *reinterpret_cast<weechat::connection*>(userdata);
                    if (connection != conn) throw std::invalid_argument("connection != conn");
                    return connection.sm_handler(stanza) ? 1 : 0;
                });
            this->handler_add(
                "resumed", nullptr, [](xmpp_conn_t *conn, xmpp_stanza_t *stanza, void *userdata) {
                    auto& connection = *reinterpret_cast<weechat::connection*>(userdata);
                    if (connection != conn) throw std::invalid_argument("connection != conn");
                    return connection.sm_handler(stanza) ? 1 : 0;
                });
            this->handler_add(
                "failed", nullptr, [](xmpp_conn_t *conn, xmpp_stanza_t *stanza, void *userdata) {
                    auto& connection = *reinterpret_cast<weechat::connection*>(userdata);
                    if (connection != conn) throw std::invalid_argument("connection != conn");
                    return connection.sm_handler(stanza) ? 1 : 0;
                });
            this->handler_add(
                "a", nullptr, [](xmpp_conn_t *conn, xmpp_stanza_t *stanza, void *userdata) {
                    auto& connection = *reinterpret_cast<weechat::connection*>(userdata);
                    if (connection != conn) throw std::invalid_argument("connection != conn");
                    return connection.sm_handler(stanza) ? 1 : 0;
                });
            this->handler_add(
                "r", nullptr, [](xmpp_conn_t *conn, xmpp_stanza_t *stanza, void *userdata) {
                    auto& connection = *reinterpret_cast<weechat::connection*>(userdata);
                    if (connection != conn) throw std::invalid_argument("connection != conn");
                    return connection.sm_handler(stanza) ? 1 : 0;
                });
            
            account.sm_handlers_registered = true;
        }

        /* Send initial <presence/> so that we appear online to contacts */
        auto children = std::unique_ptr<xmpp_stanza_t*[]>(new xmpp_stanza_t*[3 + 1]);

        pres__c = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(pres__c, "c");
        xmpp_stanza_set_ns(pres__c, "http://jabber.org/protocol/caps");
        xmpp_stanza_set_attribute(pres__c, "hash", "sha-1");
        xmpp_stanza_set_attribute(pres__c, "node", "http://weechat.org");

        xmpp_stanza_t *caps = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(caps, "caps");
        char *cap_hash;
        caps = this->get_caps(caps, &cap_hash);
        xmpp_stanza_release(caps);
        xmpp_stanza_set_attribute(pres__c, "ver", cap_hash);
        free(cap_hash);

        children[0] = pres__c;

        pres__status = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(pres__status, "status");

        pres__status__text = xmpp_stanza_new(account.context);
        xmpp_stanza_set_text(pres__status__text, account.status().data());
        xmpp_stanza_add_child(pres__status, pres__status__text);
        xmpp_stanza_release(pres__status__text);

        children[1] = pres__status;
        children[2] = NULL;

        if (true)//account.pgp)
        {
            pres__x = xmpp_stanza_new(account.context);
            xmpp_stanza_set_name(pres__x, "x");
            xmpp_stanza_set_ns(pres__x, "jabber:x:signed");

            pres__x__text = xmpp_stanza_new(account.context);
            char *signature = account.pgp.sign(account.buffer, account.pgp_keyid().data(), account.status().data());
            xmpp_stanza_set_text(pres__x__text, signature ? signature : "");
            free(signature);
            xmpp_stanza_add_child(pres__x, pres__x__text);
            xmpp_stanza_release(pres__x__text);

            children[2] = pres__x;
            children[3] = NULL;
        }

        this->send(stanza::presence()
                    .from(account.jid())
                    .build(account.context)
                    .get());

        this->send(stanza::iq()
                    .from(account.jid())
                    .type("set")
                    .id(stanza::uuid(account.context))
                    .xep0280()
                    .enable()
                    .build(account.context)
                    .get());

        this->send(stanza::iq()
                    .from(account.jid())
                    .to(account.jid())
                    .type("get")
                    .id(stanza::uuid(account.context))
                    .rfc6121()
                    .query(stanza::rfc6121::query())
                    .build(account.context)
                    .get());

        this->send(stanza::iq()
                    .from(account.jid())
                    .to(account.jid())
                    .type("get")
                    .id(stanza::uuid(account.context))
                    .xep0049()
                    .query(stanza::xep0049::query().bookmarks())
                    .build(account.context)
                    .get());

        children[1] = NULL;
        children[0] =
        stanza__iq_pubsub_items(account.context, NULL,
                                strdup("eu.siacs.conversations.axolotl.devicelist"));
        children[0] =
        stanza__iq_pubsub(account.context, NULL, children.get(),
                          with_noop("http://jabber.org/protocol/pubsub"));
        char *uuid = xmpp_uuid_gen(account.context);
        children[0] =
        stanza__iq(account.context, NULL, children.get(), NULL, uuid,
                   strdup(account.jid().data()), strdup(account.jid().data()),
                   strdup("get"));
        xmpp_free(account.context, uuid);

        this->send(children[0]);
        xmpp_stanza_release(children[0]);

        account.omemo.init(account.buffer, account.name.data());

        if (account.omemo)
        {
            children[0] =
            account.omemo.get_bundle(account.context,
                                      strdup(account.jid().data()), NULL);
            this->send(children[0]);
            xmpp_stanza_release(children[0]);
        }

        // Discover HTTP File Upload service (XEP-0363)
        weechat_printf(account.buffer, "%sDiscovering upload service...",
                      weechat_prefix("network"));
        
        // Build disco#items query manually
        char *disco_items_id = xmpp_uuid_gen(account.context);
        xmpp_stanza_t *items_iq = xmpp_iq_new(account.context, "get", disco_items_id);
        char *server_domain = xmpp_jid_domain(account.context, account.jid().data());
        xmpp_stanza_set_to(items_iq, server_domain);
        xmpp_stanza_set_from(items_iq, account.jid().data());
        
        xmpp_stanza_t *items_query = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(items_query, "query");
        xmpp_stanza_set_ns(items_query, "http://jabber.org/protocol/disco#items");
        xmpp_stanza_add_child(items_iq, items_query);
        xmpp_stanza_release(items_query);
        
        this->send(items_iq);
        xmpp_stanza_release(items_iq);
        xmpp_free(account.context, server_domain);
        xmpp_free(account.context, disco_items_id);

        // Query MAM globally to discover recent conversations
        weechat_printf(account.buffer, "%sQuerying MAM for recent conversations...",
                      weechat_prefix("network"));
        
        time_t now = time(NULL);
        time_t start = now - (7 * 86400);  // Last 7 days
        char *global_mam_id = xmpp_uuid_gen(account.context);
        account.add_mam_query(global_mam_id, "",  // Empty 'with' means global query
                             std::optional<time_t>(start), std::optional<time_t>(now));
        
        // Build MAM query manually (global query - no 'with' field)
        xmpp_stanza_t *iq = xmpp_iq_new(account.context, "set", global_mam_id);
        xmpp_stanza_set_id(iq, global_mam_id);
        
        xmpp_stanza_t *query = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(query, "query");
        xmpp_stanza_set_ns(query, "urn:xmpp:mam:2");
        
        xmpp_stanza_t *x = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(x, "x");
        xmpp_stanza_set_ns(x, "jabber:x:data");
        xmpp_stanza_set_attribute(x, "type", "submit");
        
        // FORM_TYPE field
        {
            xmpp_stanza_t *field = xmpp_stanza_new(account.context);
            xmpp_stanza_set_name(field, "field");
            xmpp_stanza_set_attribute(field, "var", "FORM_TYPE");
            xmpp_stanza_set_attribute(field, "type", "hidden");
            
            xmpp_stanza_t *value = xmpp_stanza_new(account.context);
            xmpp_stanza_set_name(value, "value");
            
            xmpp_stanza_t *text = xmpp_stanza_new(account.context);
            xmpp_stanza_set_text(text, "urn:xmpp:mam:2");
            xmpp_stanza_add_child(value, text);
            xmpp_stanza_release(text);
            
            xmpp_stanza_add_child(field, value);
            xmpp_stanza_release(value);
            
            xmpp_stanza_add_child(x, field);
            xmpp_stanza_release(field);
        }
        
        // Start time field
        {
            xmpp_stanza_t *field = xmpp_stanza_new(account.context);
            xmpp_stanza_set_name(field, "field");
            xmpp_stanza_set_attribute(field, "var", "start");
            
            xmpp_stanza_t *value = xmpp_stanza_new(account.context);
            xmpp_stanza_set_name(value, "value");
            
            xmpp_stanza_t *text = xmpp_stanza_new(account.context);
            char time_buf[256];
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%SZ", gmtime(&start));
            xmpp_stanza_set_text(text, time_buf);
            xmpp_stanza_add_child(value, text);
            xmpp_stanza_release(text);
            
            xmpp_stanza_add_child(field, value);
            xmpp_stanza_release(value);
            
            xmpp_stanza_add_child(x, field);
            xmpp_stanza_release(field);
        }
        
        xmpp_stanza_add_child(query, x);
        xmpp_stanza_release(x);
        
        xmpp_stanza_add_child(iq, query);
        xmpp_stanza_release(query);
        
        this->send(iq);
        xmpp_stanza_release(iq);
        xmpp_free(account.context, global_mam_id);

        // Restore existing PM buffers from previous session
        struct t_hdata *hdata_buffer = weechat_hdata_get("buffer");
        struct t_gui_buffer *ptr_buffer = (struct t_gui_buffer*)weechat_hdata_get_list(hdata_buffer, "gui_buffers");
        
        while (ptr_buffer)
        {
            if (weechat_buffer_get_pointer(ptr_buffer, "plugin") == weechat_plugin)
            {
                const char *ptr_type = weechat_buffer_get_string(ptr_buffer, "localvar_type");
                const char *ptr_account_name = weechat_buffer_get_string(ptr_buffer, "localvar_account");
                const char *ptr_remote_jid = weechat_buffer_get_string(ptr_buffer, "localvar_remote_jid");
                
                // Restore PM buffers only (MUCs will be restored via bookmarks)
                if (ptr_type && strcmp(ptr_type, "private") == 0 &&
                    ptr_account_name && account.name == ptr_account_name &&
                    ptr_remote_jid && ptr_remote_jid[0])
                {
                    // Check if channel already exists
                    if (!account.channels.contains(ptr_remote_jid))
                    {
                        weechat_printf(account.buffer, "%sRestoring PM buffer: %s",
                                      weechat_prefix("network"), ptr_remote_jid);
                        
                        // Create channel object for existing buffer
                        account.channels.emplace(
                            std::make_pair(ptr_remote_jid, weechat::channel {
                                    account, weechat::channel::chat_type::PM,
                                    ptr_remote_jid, ptr_remote_jid
                                }));
                    }
                }
            }
            ptr_buffer = (struct t_gui_buffer*)weechat_hdata_move(hdata_buffer, ptr_buffer, 1);
        }

        // Initialize Client State Indication (XEP-0352)
        account.last_activity = time(NULL);
        account.csi_active = true;
        
        // Send initial active state
        this->send(stanza::xep0352::active()
                   .build(account.context)
                   .get());
        
        // Hook user activity signals to detect when user becomes active
        account.csi_activity_hooks[0] = (struct t_hook *)weechat_hook_signal("input_text_changed", &account::activity_cb, &account, nullptr);
        account.csi_activity_hooks[1] = (struct t_hook *)weechat_hook_signal("buffer_switch", &account::activity_cb, &account, nullptr);
        account.csi_activity_hooks[2] = (struct t_hook *)weechat_hook_signal("key_pressed", &account::activity_cb, &account, nullptr);
        
        // Set up idle timer (check every 60 seconds)
        account.idle_timer_hook = (struct t_hook *)weechat_hook_timer(60 * 1000, 0, 0,
                                 &account::idle_timer_cb, &account, nullptr);

        // Enable/Resume Stream Management (XEP-0198) if available
        // Note: libstrophe's built-in SM is disabled via XMPP_CONN_FLAG_DISABLE_SM
        // Only try once per manual connect - don't retry on auto-reconnect if failed
        if (account.sm_available)
        {
            // Try to resume if we have a saved session
            if (!account.sm_id.empty() && account.sm_h_inbound > 0)
            {
                weechat_printf(account.buffer, "%sAttempting to resume SM session (id=%s, h=%u)...",
                              weechat_prefix("network"),
                              account.sm_id.data(),
                              account.sm_h_inbound);
                this->send(stanza::xep0198::resume(account.sm_h_inbound, account.sm_id)
                           .build(account.context)
                           .get());
            }
            else
            {
                // No saved session, request new one
                this->send(stanza::xep0198::enable(true, 300)
                           .build(account.context)
                           .get());
            }
        }

        (void) weechat_hook_signal_send("xmpp_account_connected",
                                        WEECHAT_HOOK_SIGNAL_STRING, account.name.data());
    }
    else
    {
        if (stream_error)
        {
            const char *err_text = stream_error->text;
            const char *err_type = stream_error->type == XMPP_SE_BAD_FORMAT ? "bad-format" :
                                   stream_error->type == XMPP_SE_BAD_NS_PREFIX ? "bad-namespace-prefix" :
                                   stream_error->type == XMPP_SE_CONFLICT ? "conflict" :
                                   stream_error->type == XMPP_SE_CONN_TIMEOUT ? "connection-timeout" :
                                   stream_error->type == XMPP_SE_HOST_GONE ? "host-gone" :
                                   stream_error->type == XMPP_SE_HOST_UNKNOWN ? "host-unknown" :
                                   stream_error->type == XMPP_SE_IMPROPER_ADDR ? "improper-addressing" :
                                   stream_error->type == XMPP_SE_INTERNAL_SERVER_ERROR ? "internal-server-error" :
                                   stream_error->type == XMPP_SE_INVALID_FROM ? "invalid-from" :
                                   stream_error->type == XMPP_SE_INVALID_ID ? "invalid-id" :
                                   stream_error->type == XMPP_SE_INVALID_NS ? "invalid-namespace" :
                                   stream_error->type == XMPP_SE_INVALID_XML ? "invalid-xml" :
                                   stream_error->type == XMPP_SE_NOT_AUTHORIZED ? "not-authorized" :
                                   stream_error->type == XMPP_SE_POLICY_VIOLATION ? "policy-violation" :
                                   stream_error->type == XMPP_SE_REMOTE_CONN_FAILED ? "remote-connection-failed" :
                                   stream_error->type == XMPP_SE_RESOURCE_CONSTRAINT ? "resource-constraint" :
                                   stream_error->type == XMPP_SE_RESTRICTED_XML ? "restricted-xml" :
                                   stream_error->type == XMPP_SE_SEE_OTHER_HOST ? "see-other-host" :
                                   stream_error->type == XMPP_SE_SYSTEM_SHUTDOWN ? "system-shutdown" :
                                   stream_error->type == XMPP_SE_UNDEFINED_CONDITION ? "undefined-condition" :
                                   stream_error->type == XMPP_SE_UNSUPPORTED_ENCODING ? "unsupported-encoding" :
                                   stream_error->type == XMPP_SE_UNSUPPORTED_STANZA_TYPE ? "unsupported-stanza-type" :
                                   stream_error->type == XMPP_SE_UNSUPPORTED_VERSION ? "unsupported-version" :
                                   stream_error->type == XMPP_SE_XML_NOT_WELL_FORMED ? "xml-not-well-formed" :
                                   "unknown";
            
            weechat_printf(account.buffer, "%sStream error: %s%s%s",
                          weechat_prefix("error"),
                          err_type,
                          err_text ? " - " : "",
                          err_text ? err_text : "");
        }
        
        // Clear SM session on clean disconnect (server-initiated or normal close)
        // This prevents trying to resume a session the server has already closed
        if (status == event::disconnect && error == 0)
        {
            if (!account.sm_id.empty())
            {
                weechat_printf(account.buffer, "%sSM session %s closed by server",
                              weechat_prefix("network"), account.sm_id.data());
            }
            account.sm_id = "";
            account.sm_h_inbound = 0;
            account.sm_h_outbound = 0;
            account.sm_last_ack = 0;
        }
        
        account.disconnect(1);
      //xmpp_stop(account.context); //keep context?
    }

    return true;
}

char* rand_string(int length)
{
    char *string = new char[length];
    for(int i = 0; i < length; ++i){
        string[i] = '0' + rand()%72; // starting on '0', ending on '}'
        if (!((string[i] >= '0' && string[i] <= '9') ||
              (string[i] >= 'A' && string[i] <= 'Z') ||
              (string[i] >= 'a' && string[i] <= 'z')))
            i--; // reroll
    }
    string[length] = 0;
    return string;
}

int weechat::connection::connect(std::string jid, std::string password, weechat::tls_policy tls)
{
    static const unsigned ka_timeout_sec = 60;
    static const unsigned ka_timeout_ivl = 1;

    m_conn.set_keepalive(ka_timeout_sec, ka_timeout_ivl);

    const char *resource = account.resource().data();
    if (!(resource && strlen(resource)))
    {
        char *const rand = rand_string(8);
        char ident[64] = {0};
        snprintf(ident, sizeof(ident), "weechat.%s", rand);
        delete[] rand;

        account.resource(ident);
        resource = account.resource().data();
    }
    m_conn.set_jid(xmpp_jid_new(account.context,
                                xmpp_jid_node(account.context, jid.data()),
                                xmpp_jid_domain(account.context, jid.data()),
                                resource));
    m_conn.set_pass(weechat_string_eval_expression(password.data(),
                    NULL, NULL, NULL));

    int flags = m_conn.get_flags();
    switch (tls)
    {
        case weechat::tls_policy::disable:
            flags |= XMPP_CONN_FLAG_DISABLE_TLS;
            break;
        case weechat::tls_policy::normal:
            flags &= ~XMPP_CONN_FLAG_DISABLE_TLS;
            flags &= ~XMPP_CONN_FLAG_TRUST_TLS;
            break;
        case weechat::tls_policy::trust:
            flags |= XMPP_CONN_FLAG_TRUST_TLS;
            break;
        default:
            break;
    }
    m_conn.set_flags(flags);

    if (!connect_client(
            nullptr, 0, [](xmpp_conn_t *conn, xmpp_conn_event_t status,
                           int error, xmpp_stream_error_t *stream_error,
                           void *userdata) {
                auto& connection = *reinterpret_cast<weechat::connection*>(userdata);
                if (connection != conn) throw std::invalid_argument("connection != conn");
                connection.conn_handler(static_cast<event>(status), error, stream_error);
            }))
    {
        weechat_printf(
            nullptr,
            _("%s%s: error connecting to %s"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
            jid.data());
        return false;
    }

    return true;
}

void weechat::connection::process(xmpp_ctx_t *context, const unsigned long timeout)
{
    xmpp_run_once(context ? context : this->context(), timeout);
}
