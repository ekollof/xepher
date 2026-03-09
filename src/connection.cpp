// This->Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <stdexcept>
#include <optional>
#include <thread>
#include <time.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
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
#include "avatar.hh"
#include "xmpp/xep-0054.inl"
#include "xmpp/xep-0084.inl"
#include "xmpp/xep-0172.inl"
#include "xmpp/xep-0292.inl"

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
            // Keep a copy in the retransmit queue (XEP-0198 §5)
            xmpp_stanza_t *copy = xmpp_stanza_copy(stanza);
            account.sm_outqueue.emplace_back(
                account.sm_h_outbound,
                std::shared_ptr<xmpp_stanza_t>(copy, xmpp_stanza_release));
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
        bool is_nick_change = false;
        bool is_new_room = false;
        bool is_server_nick = false;
        for (int& status : x->statuses)
        {
            switch (status)
            {
                case 100: // Non-Anonymous: [message | Entering a room]: Inform user that any occupant is allowed to see the user's full JID
                    if (channel)
                        weechat_buffer_set(channel->buffer, "notify", "2");
                    break;
                case 101: // Affiliation change: room visibility changed, JID now visible to all
                    if (channel)
                        weechat_printf_date_tags(channel->buffer, 0, "xmpp_presence,notify_none",
                                                 "%s%s[Room] Your affiliation changed; your full JID is now visible to all occupants",
                                                 weechat_prefix("network"), weechat_color("gray"));
                    break;
                case 102: // : [message | Configuration change]: Inform occupants that room now shows unavailable members
                    break;
                case 103: // : [message | Configuration change]: Inform occupants that room now does not show unavailable members
                    break;
                case 104: // : [message | Configuration change]: Inform occupants that a non-privacy-related room configuration change has occurred
                    break;
                case 110: // Self-Presence: [presence | Any room presence]: Inform user that presence refers to one of its own room occupants
                    // Status 110 is sent last in the initial presence flood — clear joining flag
                    if (channel)
                        channel->joining = false;
                    break;
                case 170: // Logging Active: room logging enabled — privacy notice
                    if (channel)
                        weechat_printf_date_tags(channel->buffer, 0, "xmpp_presence,notify_none",
                                                 "%s%s[Room] Warning: room logging is now %senabled%s — messages are being stored",
                                                 weechat_prefix("network"), weechat_color("yellow"),
                                                 weechat_color("yellow,bold"), weechat_color("reset"));
                    break;
                case 171: // Logging disabled
                    if (channel)
                        weechat_printf_date_tags(channel->buffer, 0, "xmpp_presence,notify_none",
                                                 "%s%s[Room] Room logging is now disabled",
                                                 weechat_prefix("network"), weechat_color("gray"));
                    break;
                case 172: // Room is now non-anonymous (full JIDs visible)
                    if (channel)
                        weechat_printf_date_tags(channel->buffer, 0, "xmpp_presence,notify_none",
                                                 "%s%s[Room] Room is now %snon-anonymous%s — full JIDs are visible to all occupants",
                                                 weechat_prefix("network"), weechat_color("yellow"),
                                                 weechat_color("yellow,bold"), weechat_color("reset"));
                    break;
                case 173: // Room is now semi-anonymous
                    if (channel)
                        weechat_printf_date_tags(channel->buffer, 0, "xmpp_presence,notify_none",
                                                 "%s%s[Room] Room is now semi-anonymous — full JIDs visible to moderators only",
                                                 weechat_prefix("network"), weechat_color("gray"));
                    break;
                case 174: // Room is now fully-anonymous
                    if (channel)
                        weechat_printf_date_tags(channel->buffer, 0, "xmpp_presence,notify_none",
                                                 "%s%s[Room] Room is now fully-anonymous — JIDs are hidden from all occupants",
                                                 weechat_prefix("network"), weechat_color("gray"));
                    break;
                case 201: // New room created — must accept default config to unlock it
                    is_new_room = true;
                    if (channel)
                        weechat_printf_date_tags(channel->buffer, 0, "xmpp_presence,notify_none",
                                                 "%s%s[Room] New room created; accepting default configuration",
                                                 weechat_prefix("network"), weechat_color("gray"));
                    break;
                case 210: // Server assigned/modified nick
                    is_server_nick = true;
                    break;
                case 301: // : [presence | Removal from room]: Inform user that he or she has been banned from the room
                    weechat_printf(channel->buffer, "[!]\t%sBanned from Room", weechat_color("gray"));
                    break;
                case 303: // Nick change — this unavailable presence means occupant is changing nick
                    is_nick_change = true;
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
            user->profile.status_text = status ? std::optional<std::string>(status->data()) : std::nullopt;
            user->profile.status = show ? std::optional<std::string>(show->data()) : std::nullopt;
            user->profile.idle = idle ? fmt::format("{}", *idle) : std::string();
            user->is_away = show ? *show == "away" : false;
            user->profile.role = role.size() ? std::optional<std::string>(role.data()) : std::nullopt;
            user->profile.affiliation = (affiliation.size() && affiliation != "none")
                ? std::optional<std::string>(affiliation.data()) : std::nullopt;
            if (channel)
            {
                if (auto signature = binding.signature())
                {
                    char *pgp_raw = account.pgp.verify(channel->buffer, signature->data());
                    user->profile.pgp_id = pgp_raw ? std::optional<std::string>(pgp_raw) : std::nullopt;
                    free(pgp_raw);
                    if (user->profile.pgp_id.has_value() && channel->type != weechat::channel::chat_type::MUC)
                        channel->pgp.ids.emplace(user->profile.pgp_id.value());
                }

                if (weechat_strcasecmp(role.data(), "none") == 0)
                {
                    if (is_nick_change && item.nick && channel)
                    {
                        // Status 303: nick change — print rename notice instead of "left"
                        const char *old_nick = binding.from->resource.size()
                            ? binding.from->resource.data() : binding.from->full.data();
                        // Smart filter: suppress nick-change for users who haven't spoken recently
                        const char *nick_tags = channel->smart_filter_nick(old_nick)
                            ? "xmpp_presence,nick,log4,xmpp_smart_filter"
                            : "xmpp_presence,nick,log4";
                        weechat_printf_date_tags(channel->buffer, 0, nick_tags,
                                                 "%s%s%s%s is now known as %s%s",
                                                 weechat_prefix("network"),
                                                 weechat_color("irc.color.nick_change"),
                                                 old_nick,
                                                 weechat_color("reset"),
                                                 weechat_color("irc.color.nick_change"),
                                                 item.nick->data());
                        // Still remove from nicklist so the new presence can re-add
                        weechat::user *leaving = weechat::user::search(&account, binding.from->full.data());
                        if (leaving)
                            leaving->nicklist_remove(&account, channel);
                    }
                    else
                        channel->remove_member(binding.from->full.data(), status ? status->data() : nullptr);
                }
                else
                {
                    channel->add_member(binding.from->full.data(), jid.data());
                    if (is_server_nick && channel)
                    {
                        // Status 210: server assigned a different nick than requested
                        const char *assigned = binding.from->resource.size()
                            ? binding.from->resource.data() : binding.from->full.data();
                        weechat_printf_date_tags(channel->buffer, 0, "xmpp_presence,notify_none",
                                                 "%s%s[Room] Server assigned you the nick: %s%s%s",
                                                 weechat_prefix("network"), weechat_color("gray"),
                                                 weechat_color("irc.color.nick_change"),
                                                 assigned,
                                                 weechat_color("reset"));
                    }
                }
            }
        }

        if (is_new_room && channel)
        {
            // Status 201: new room was created — send empty config submit to unlock it
            char *room_jid = xmpp_jid_bare(account.context, binding.from->full.data());
            char *owner_id = xmpp_uuid_gen(account.context);
            xmpp_stanza_t *iq = xmpp_iq_new(account.context, "set", owner_id);
            xmpp_stanza_set_to(iq, room_jid);
            xmpp_stanza_t *query = xmpp_stanza_new(account.context);
            xmpp_stanza_set_name(query, "query");
            xmpp_stanza_set_ns(query, "http://jabber.org/protocol/muc#owner");
            xmpp_stanza_t *x_form = xmpp_stanza_new(account.context);
            xmpp_stanza_set_name(x_form, "x");
            xmpp_stanza_set_ns(x_form, "jabber:x:data");
            xmpp_stanza_set_attribute(x_form, "type", "submit");
            xmpp_stanza_add_child(query, x_form);
            xmpp_stanza_add_child(iq, query);
            xmpp_stanza_release(x_form);
            xmpp_stanza_release(query);
            xmpp_send(account.connection, iq);
            xmpp_stanza_release(iq);
            xmpp_free(account.context, owner_id);
            xmpp_free(account.context, room_jid);
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
        user->profile.status_text = status ? std::optional<std::string>(status->data()) : std::nullopt;
        user->profile.status = show ? std::optional<std::string>(show->data()) : std::nullopt;
        user->profile.idle = idle ? fmt::format("{}", *idle) : std::string();
        user->is_away = show ? *show == "away" : false;
        user->profile.role = std::nullopt;
        user->profile.affiliation = std::nullopt;
        
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
                char *pgp_raw = account.pgp.verify(channel->buffer, signature->data());
                user->profile.pgp_id = pgp_raw ? std::optional<std::string>(pgp_raw) : std::nullopt;
                free(pgp_raw);
                if (user->profile.pgp_id.has_value() && channel->type != weechat::channel::chat_type::MUC)
                    channel->pgp.ids.emplace(user->profile.pgp_id.value());
            }

            if (user->profile.role.has_value())
                channel->remove_member(binding.from->full.data(), status ? status->data() : nullptr);
            else
                channel->add_member(binding.from->full.data(), clientid.data());
        }
    }

    // XEP-0153: vCard-Based Avatars — parse photo hash from presence <x>
    // Only fetch vCards for roster contacts (non-MUC presences).
    // MUC occupant presences come from room@conference.example/nick — the bare
    // JID is the room itself, not a real user, so vcard-temp requests there
    // are meaningless and would spam the server.
    {
        xmpp_stanza_t *vcard_x = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "x", "vcard-temp:x:update");
        bool is_muc_presence = channel && channel->type == weechat::channel::chat_type::MUC;
        if (vcard_x && user && !is_muc_presence)
        {
            xmpp_stanza_t *photo = xmpp_stanza_get_child_by_name(vcard_x, "photo");
            if (photo)
            {
                char *photo_hash = xmpp_stanza_get_text(photo);
                if (photo_hash && strlen(photo_hash) > 0)
                {
                    // Store the vCard avatar hash if we don't already have one
                    // from XEP-0084 (prefer PEP avatar over legacy vCard avatar)
                    if (user->profile.avatar_hash.empty())
                    {
                        user->profile.avatar_hash = photo_hash;
                    }
                    // Auto-fetch vCard to retrieve the photo and profile info if
                    // we haven't fetched it yet, or if the hash changed.
                    bool hash_changed = (user->profile.avatar_hash != photo_hash);
                    if (!user->profile.vcard_fetched || hash_changed)
                    {
                        // Use the bare JID for the vCard request
                        const char *from_full = xmpp_stanza_get_from(stanza);
                        if (from_full)
                        {
                            char *bare = xmpp_jid_bare(account.context, from_full);
                            if (bare)
                            {
                                xmpp_stanza_t *iq = ::xmpp::xep0054::vcard_request(account.context, bare);
                                const char *req_id = xmpp_stanza_get_id(iq);
                                if (req_id)
                                    account.whois_queries[req_id] = { account.buffer, std::string(bare) };
                                send(iq);
                                xmpp_stanza_release(iq);
                                xmpp_free(account.context, bare);
                            }
                        }
                    }
                    xmpp_free(account.context, photo_hash);
                }
            }
        }
    }

    return true;
}

// XEP-0071: XHTML-IM — recursively extract plain text from an XHTML stanza tree.
// Handles block elements (p, div, br) by inserting newlines/spaces.
// Map a CSS color value (name or #rrggbb/#rgb) to a WeeChat color string.
// Returns empty string if no reasonable mapping found.
static std::string css_color_to_weechat(const std::string &css)
{
    // Named colors → WeeChat terminal color names
    struct { const char *css; const char *wc; } table[] = {
        {"black",   "black"},
        {"white",   "white"},
        {"red",     "red"},
        {"green",   "green"},
        {"blue",    "blue"},
        {"yellow",  "yellow"},
        {"cyan",    "cyan"},
        {"magenta", "magenta"},
        {"orange",  "214"},    // WeeChat 256-color index
        {"gray",    "gray"},
        {"grey",    "gray"},
        {"darkgray","darkgray"},
        {"darkgrey","darkgray"},
        {"purple",  "magenta"},
        {"pink",    "213"},
        {"brown",   "130"},
        {"lime",    "46"},
        {"teal",    "30"},
        {"navy",    "18"},
        {"silver",  "250"},
        {nullptr, nullptr}
    };
    // Lowercase the input for comparison
    std::string lc = css;
    for (auto &c : lc) c = (char)tolower((unsigned char)c);
    // Strip leading/trailing spaces
    auto s = lc.find_first_not_of(" \t");
    auto e = lc.find_last_not_of(" \t");
    if (s == std::string::npos) return "";
    lc = lc.substr(s, e - s + 1);

    for (int i = 0; table[i].css; ++i)
        if (lc == table[i].css) return table[i].wc;

    // #rrggbb or #rgb → WeeChat uses "color(r,g,b)" syntax for 24-bit,
    // but for broad terminal compat map to nearest of the 16 named colors.
    if (lc.size() >= 4 && lc[0] == '#')
    {
        unsigned r = 0, g = 0, b = 0;
        if (lc.size() == 7)
        {
            r = std::stoul(lc.substr(1,2), nullptr, 16);
            g = std::stoul(lc.substr(3,2), nullptr, 16);
            b = std::stoul(lc.substr(5,2), nullptr, 16);
        }
        else if (lc.size() == 4)
        {
            r = std::stoul(lc.substr(1,1), nullptr, 16) * 17;
            g = std::stoul(lc.substr(2,1), nullptr, 16) * 17;
            b = std::stoul(lc.substr(3,1), nullptr, 16) * 17;
        }
        // Map to nearest basic color
        if (r > 200 && g < 100 && b < 100) return "red";
        if (r < 100 && g > 150 && b < 100) return "green";
        if (r < 100 && g < 100 && b > 150) return "blue";
        if (r > 150 && g > 150 && b < 100) return "yellow";
        if (r < 100 && g > 150 && b > 150) return "cyan";
        if (r > 150 && g < 100 && b > 150) return "magenta";
        if (r > 180 && g > 120 && b < 80)  return "214"; // orange
        if (r > 180 && g > 180 && b > 180) return "white";
        if (r < 80  && g < 80  && b < 80)  return "black";
        if (r > 100 && g > 100 && b > 100) return "gray";
    }
    return "";
}

// Parse an inline CSS style string and return opening/closing WeeChat color codes.
// Handles: color, background-color, font-weight, font-style, text-decoration.
static std::pair<std::string, std::string> css_style_to_weechat(const char *style)
{
    if (!style) return {"", ""};

    std::string open, close;
    std::string s(style);

    // Iterate over semicolon-separated declarations
    size_t pos = 0;
    while (pos < s.size())
    {
        size_t semi = s.find(';', pos);
        std::string decl = s.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
        pos = (semi == std::string::npos) ? s.size() : semi + 1;

        size_t colon = decl.find(':');
        if (colon == std::string::npos) continue;
        std::string prop = decl.substr(0, colon);
        std::string val  = decl.substr(colon + 1);

        // Trim
        auto trim = [](std::string &str) {
            auto a = str.find_first_not_of(" \t");
            auto b = str.find_last_not_of(" \t");
            str = (a == std::string::npos) ? "" : str.substr(a, b - a + 1);
            for (auto &c : str) c = (char)tolower((unsigned char)c);
        };
        trim(prop); trim(val);

        if (prop == "font-weight" && val == "bold")
        {
            open  += weechat_color("bold");
            close  = std::string(weechat_color("-bold")) + close;
        }
        else if (prop == "font-style" && val == "italic")
        {
            open  += weechat_color("italic");
            close  = std::string(weechat_color("-italic")) + close;
        }
        else if (prop == "text-decoration" && val == "underline")
        {
            open  += weechat_color("underline");
            close  = std::string(weechat_color("-underline")) + close;
        }
        else if (prop == "color")
        {
            std::string wc = css_color_to_weechat(val);
            if (!wc.empty())
            {
                open  += weechat_color(wc.c_str());
                close  = std::string(weechat_color("resetcolor")) + close;
            }
        }
        // background-color: intentionally ignored (terminal BG control is risky)
    }
    return {open, close};
}

// Recursively convert an XHTML-IM stanza tree to a WeeChat-formatted string.
// Inline elements emit WeeChat color/attribute codes; block elements add newlines.
// blockquote content is prefixed with "> " in green on each line.
static std::string xhtml_to_weechat(xmpp_stanza_t *stanza, bool in_blockquote = false)
{
    std::string result;
    for (xmpp_stanza_t *child = xmpp_stanza_get_children(stanza);
         child; child = xmpp_stanza_get_next(child))
    {
        if (xmpp_stanza_is_text(child))
        {
            const char *raw = xmpp_stanza_get_text_ptr(child);
            if (!raw) continue;
            if (in_blockquote)
            {
                // Prefix every line with "> "
                std::string txt(raw);
                std::string line;
                for (char c : txt)
                {
                    if (c == '\n')
                    {
                        result += weechat_color("green");
                        result += "> ";
                        result += weechat_color("resetcolor");
                        result += line;
                        result += '\n';
                        line.clear();
                    }
                    else line += c;
                }
                if (!line.empty())
                {
                    result += weechat_color("green");
                    result += "> ";
                    result += weechat_color("resetcolor");
                    result += line;
                }
            }
            else
            {
                result += raw;
            }
            continue;
        }

        const char *name = xmpp_stanza_get_name(child);
        if (!name) continue;

        // ── Block-level elements ──────────────────────────────────────
        bool is_block = (strcmp(name, "p") == 0
                      || strcmp(name, "div") == 0
                      || strcmp(name, "li") == 0);
        bool is_br         = (strcmp(name, "br") == 0);
        bool is_blockquote = (strcmp(name, "blockquote") == 0);
        bool is_pre        = (strcmp(name, "pre") == 0);

        if (is_br)
        {
            result += '\n';
            if (in_blockquote)
            {
                result += weechat_color("green");
                result += "> ";
                result += weechat_color("resetcolor");
            }
            continue;
        }

        if (is_blockquote)
        {
            if (!result.empty() && result.back() != '\n') result += '\n';
            // Render blockquote contents with in_blockquote=true
            std::string inner = xhtml_to_weechat(child, true);
            // Prefix the very first line too
            result += weechat_color("green");
            result += "> ";
            result += weechat_color("resetcolor");
            result += inner;
            if (!result.empty() && result.back() != '\n') result += '\n';
            continue;
        }

        if (is_pre)
        {
            if (!result.empty() && result.back() != '\n') result += '\n';
            result += weechat_color("gray");
            result += xhtml_to_weechat(child, in_blockquote);
            result += weechat_color("resetcolor");
            if (!result.empty() && result.back() != '\n') result += '\n';
            continue;
        }

        if (is_block)
        {
            if (!result.empty() && result.back() != '\n') result += '\n';
            result += xhtml_to_weechat(child, in_blockquote);
            if (!result.empty() && result.back() != '\n') result += '\n';
            continue;
        }

        // ── Inline elements ───────────────────────────────────────────
        if (strcmp(name, "b") == 0 || strcmp(name, "strong") == 0)
        {
            result += weechat_color("bold");
            result += xhtml_to_weechat(child, in_blockquote);
            result += weechat_color("-bold");
            continue;
        }

        if (strcmp(name, "i") == 0 || strcmp(name, "em") == 0)
        {
            result += weechat_color("italic");
            result += xhtml_to_weechat(child, in_blockquote);
            result += weechat_color("-italic");
            continue;
        }

        if (strcmp(name, "u") == 0)
        {
            result += weechat_color("underline");
            result += xhtml_to_weechat(child, in_blockquote);
            result += weechat_color("-underline");
            continue;
        }

        if (strcmp(name, "del") == 0 || strcmp(name, "s") == 0 || strcmp(name, "strike") == 0)
        {
            // No real strikethrough in most terminals; use dim/darkgray
            result += weechat_color("darkgray");
            result += xhtml_to_weechat(child, in_blockquote);
            result += weechat_color("resetcolor");
            continue;
        }

        if (strcmp(name, "code") == 0 || strcmp(name, "tt") == 0)
        {
            result += weechat_color("gray");
            result += xhtml_to_weechat(child, in_blockquote);
            result += weechat_color("resetcolor");
            continue;
        }

        if (strcmp(name, "span") == 0)
        {
            const char *style_attr = xmpp_stanza_get_attribute(child, "style");
            auto [open, close] = css_style_to_weechat(style_attr);
            result += open;
            result += xhtml_to_weechat(child, in_blockquote);
            result += close;
            continue;
        }

        if (strcmp(name, "a") == 0)
        {
            const char *href = xmpp_stanza_get_attribute(child, "href");
            std::string link_text = xhtml_to_weechat(child, in_blockquote);
            if (href && *href)
            {
                // If link text equals the URL, just show it in blue
                if (link_text == href)
                {
                    result += weechat_color("blue");
                    result += href;
                    result += weechat_color("resetcolor");
                }
                else
                {
                    result += link_text;
                    result += ' ';
                    result += weechat_color("blue");
                    result += '(';
                    result += href;
                    result += ')';
                    result += weechat_color("resetcolor");
                }
            }
            else
            {
                result += link_text;
            }
            continue;
        }

        if (strcmp(name, "img") == 0)
        {
            // Show alt text, or a placeholder
            const char *alt = xmpp_stanza_get_attribute(child, "alt");
            const char *src = xmpp_stanza_get_attribute(child, "src");
            result += weechat_color("darkgray");
            result += '[';
            result += (alt && *alt) ? alt : (src ? src : "image");
            result += ']';
            result += weechat_color("resetcolor");
            continue;
        }

        // Fallback: recurse without any special formatting (e.g. <body>, <html>, unknown tags)
        result += xhtml_to_weechat(child, in_blockquote);
    }
    return result;
}

bool weechat::connection::message_handler(xmpp_stanza_t *stanza, bool /* top_level */)
{
    // SM counter incremented in libstrophe wrapper, not here
    // top_level parameter kept for nested/recursive calls

    weechat::channel *channel, *parent_channel;
    xmpp_stanza_t *x, *body, *delay, *topic, *replace, *request, *markable, *composing, *sent, *received, *result, *forwarded, *event, *items, *item, *list, *device, *encrypted;
    const char *type, *from, *nick, *from_bare, *to, *to_bare, *id, *thread, *replace_id, *timestamp;
    const char *text = nullptr;
    struct xmpp_guard { xmpp_ctx_t *ctx; char *ptr; ~xmpp_guard() { if (ptr) xmpp_free(ctx, ptr); } };
    struct free_guard { char *ptr; ~free_guard() { if (ptr) ::free(ptr); } };
    xmpp_guard intext_g { account.context, nullptr };
    char *&intext = intext_g.ptr;
    free_guard cleartext_g { nullptr };
    char *&cleartext = cleartext_g.ptr;
    free_guard difftext_g { nullptr };
    char *&difftext = difftext_g.ptr;
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
            intext = nullptr;  // Released by RAII guard (was xmpp_free)
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

            // XEP-0085 §5.1: record that this JID supports chat states so we
            // may send states back to them.
            channel->mark_chat_state_supported(std::string(from));
            channel->mark_chat_state_supported(std::string(from_bare));

            if (composing)
            {
                channel->add_typing(user);
            }
            else if (paused)
            {
                // <paused> means stopped typing but hasn't sent yet — remove from
                // typing list so the indicator clears immediately
                channel->remove_typing(user);
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
                    
                    // Parse timestamp (stamp is always UTC, use timegm not mktime)
                    time_t msg_timestamp = 0;
                    if (timestamp_str)
                    {
                        struct tm time = {0};
                        strptime(timestamp_str, "%FT%T", &time);
                        msg_timestamp = timegm(&time);
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
                                                        bundle_node);
                                children[0] =
                                stanza__iq_pubsub(account.context, NULL, children.get(),
                                                  with_noop("http://jabber.org/protocol/pubsub"));
                                char *uuid = xmpp_uuid_gen(account.context);
                                children[0] =
                                stanza__iq(account.context, NULL, children.get(), NULL, uuid,
                                            to, from, "get");
                                xmpp_free(account.context, uuid);

                                account.connection.send(children[0]);
                                xmpp_stanza_release(children[0]);
                            }
                        }
                    }
                }
                
                // XEP-0084: User Avatar - Metadata  
                if (items_node
                    && weechat_strcasecmp(items_node, "urn:xmpp:avatar:metadata") == 0)
                {
                    item = xmpp_stanza_get_child_by_name(items, "item");
                    if (item)
                    {
                        xmpp_stanza_t *metadata = xmpp_stanza_get_child_by_name_and_ns(
                            item, "metadata", "urn:xmpp:avatar:metadata");
                        
                        if (metadata)
                        {
                            xmpp_stanza_t *info = xmpp_stanza_get_child_by_name(metadata, "info");
                            if (info)
                            {
                                const char *info_id = xmpp_stanza_get_id(info);
                                const char *info_type = xmpp_stanza_get_attribute(info, "type");
                                const char *info_bytes = xmpp_stanza_get_attribute(info, "bytes");
                                
                                if (info_id)
                                {
                                    const char *from_jid = from ? from : account.jid().data();
                                    
                                    // Update user's avatar hash
                                    weechat::user *user = weechat::user::search(&account, from_jid);
                                    if (user)
                                    {
                                        user->profile.avatar_hash = info_id;
                                    }
                                    
                                    weechat_printf_date_tags(account.buffer, 0, "xmpp_avatar",
                                                            "%sAvatar update from %s (hash: %.8s..., type: %s, bytes: %s)",
                                                            weechat_prefix("network"),
                                                            from_jid,
                                                            info_id,
                                                            info_type ? info_type : "unknown",
                                                            info_bytes ? info_bytes : "unknown");
                                    
                                    // Request avatar data
                                    weechat::avatar::request_data(account, from_jid, info_id);
                                }
                            }
                        }
                    }
                }
                
                // XEP-0084: User Avatar - Data
                if (items_node
                    && weechat_strcasecmp(items_node, "urn:xmpp:avatar:data") == 0)
                {
                    item = xmpp_stanza_get_child_by_name(items, "item");
                    if (item)
                    {
                        xmpp_stanza_t *data_elem = xmpp_stanza_get_child_by_name_and_ns(
                            item, "data", "urn:xmpp:avatar:data");
                        
                        if (data_elem)
                        {
                            char *b64_data = xmpp_stanza_get_text(data_elem);
                            if (b64_data)
                            {
                                // Decode base64 avatar data
                                BIO *bio, *b64;
                                size_t decode_len = strlen(b64_data);
                                std::vector<uint8_t> image_data(decode_len);
                                
                                bio = BIO_new_mem_buf(b64_data, -1);
                                b64 = BIO_new(BIO_f_base64());
                                bio = BIO_push(b64, bio);
                                BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
                                
                                int actual_len = BIO_read(bio, image_data.data(), decode_len);
                                BIO_free_all(bio);
                                
                                if (actual_len > 0)
                                {
                                    image_data.resize(actual_len);
                                    
                                    const char *from_jid = from ? from : account.jid().data();
                                    std::string hash = weechat::avatar::calculate_hash(image_data);
                                    
                                    // Save to cache
                                    weechat::avatar::data avatar_data;
                                    avatar_data.image_data = image_data;
                                    avatar_data.meta.id = hash;
                                    avatar_data.meta.bytes = actual_len;
                                    avatar_data.meta.type = "image/png";  // Default, should get from metadata
                                    
                                    weechat::avatar::save_to_cache(account, hash, avatar_data);
                                    
                                    // Update user profile
                                    weechat::user *user = weechat::user::search(&account, from_jid);
                                    if (user)
                                    {
                                        user->profile.avatar_data = image_data;
                                        user->profile.avatar_rendered = 
                                            weechat::avatar::render_unicode_blocks(image_data, 
                                                                                   avatar_data.meta.type);
                                    }
                                    
                                    weechat_printf_date_tags(account.buffer, 0, "xmpp_avatar",
                                                            "%sAvatar data received from %s (%d bytes, hash verified: %s)",
                                                            weechat_prefix("network"),
                                                            from_jid,
                                                            actual_len,
                                                            hash.c_str());
                                }
                                
                                xmpp_free(account.context, b64_data);
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

                // XEP-0490: Message Displayed Synchronization
                // <items node='urn:xmpp:mds:displayed:0'>
                //   <item id='thecontactjid@example.org'>
                //     <displayed xmlns='urn:xmpp:mds:displayed:0'>
                //       <stanza-id xmlns='urn:xmpp:sid:0' by='thecontactjid@example.org' id='stanza-id-of-last-displayed'/>
                //     </displayed>
                //   </item>
                // </items>
                if (items_node
                    && weechat_strcasecmp(items_node, "urn:xmpp:mds:displayed:0") == 0)
                {
                    for (item = xmpp_stanza_get_children(items);
                         item; item = xmpp_stanza_get_next(item))
                    {
                        if (weechat_strcasecmp(xmpp_stanza_get_name(item), "item") != 0)
                            continue;

                        const char *peer_jid = xmpp_stanza_get_id(item);
                        if (!peer_jid)
                            continue;

                        xmpp_stanza_t *displayed_elem = xmpp_stanza_get_child_by_name_and_ns(
                            item, "displayed", "urn:xmpp:mds:displayed:0");
                        if (!displayed_elem)
                            continue;

                        // Extract the stanza-id of the last displayed message
                        xmpp_stanza_t *sid_elem = xmpp_stanza_get_child_by_name_and_ns(
                            displayed_elem, "stanza-id", "urn:xmpp:sid:0");
                        const char *last_id = sid_elem
                            ? xmpp_stanza_get_id(sid_elem) : nullptr;

                        // Only act on events from other devices of our own account
                        const char *event_from = from ? from : account.jid().data();
                        const char *own_bare = xmpp_jid_bare(account.context, account.jid().data());
                        const char *event_bare = xmpp_jid_bare(account.context, event_from);
                        bool is_own = own_bare && event_bare &&
                            (weechat_strcasecmp(own_bare, event_bare) == 0);
                        xmpp_free(account.context, (void*)own_bare);
                        xmpp_free(account.context, (void*)event_bare);

                        if (!is_own)
                            continue;

                        // Find the channel for this peer and clear its unreads
                        // up to and including last_id (or all if id unknown)
                        weechat::channel *ch = account.channels.contains(peer_jid)
                            ? &account.channels.find(peer_jid)->second : nullptr;
                        if (!ch)
                            continue;

                        if (last_id)
                        {
                            // Remove all unreads up to and including the given stanza-id
                            auto &u = ch->unreads;
                            auto it = std::begin(u);
                            while (it != std::end(u))
                            {
                                bool match = (it->id == last_id);
                                it = u.erase(it);
                                if (match)
                                    break;
                            }
                        }
                        else
                        {
                            ch->unreads.clear();
                        }

                        weechat_printf_date_tags(ch->buffer, 0,
                                                 "xmpp_mds,notify_none",
                                                 "%s%sRead sync from another device (id: %.8s…)",
                                                 weechat_prefix("network"),
                                                 weechat_color("darkgray"),
                                                 last_id ? last_id : "all");
                    }
                }
            }
        }

        // XEP-0184: Message Delivery Receipts — handle incoming <received> from others
        {
            xmpp_stanza_t *receipt_received = xmpp_stanza_get_child_by_name_and_ns(
                stanza, "received", "urn:xmpp:receipts");
            if (receipt_received)
            {
                const char *receipt_from = xmpp_stanza_get_from(stanza);
                const char *acked_id = xmpp_stanza_get_id(receipt_received);
                if (receipt_from && acked_id)
                {
                    const char *bare = xmpp_jid_bare(account.context, receipt_from);
                    weechat::channel *ch = account.channels.contains(bare)
                        ? &account.channels.find(bare)->second : nullptr;
                    if (ch)
                    {
                        // Find the sent message line tagged id_<acked_id> and update glyph ⌛→✓
                        struct t_hdata *hdata_line      = weechat_hdata_get("line");
                        struct t_hdata *hdata_line_data = weechat_hdata_get("line_data");
                        struct t_hdata *hdata_lines     = weechat_hdata_get("lines");
                        struct t_hdata *hdata_buffer    = weechat_hdata_get("buffer");
                        void *lines     = weechat_hdata_pointer(hdata_buffer, ch->buffer, "lines");
                        void *last_line = lines ? weechat_hdata_pointer(hdata_lines, lines, "last_line") : nullptr;
                        std::string target_tag = std::string("id_") + acked_id;
                        while (last_line)
                        {
                            void *line_data = weechat_hdata_pointer(hdata_line, last_line, "data");
                            if (line_data)
                            {
                                int tags_count = weechat_hdata_integer(hdata_line_data, line_data, "tags_count");
                                char str_tag[24] = {0};
                                bool found = false;
                                for (int n = 0; n < tags_count && !found; n++)
                                {
                                    snprintf(str_tag, sizeof(str_tag), "%d|tags_array", n);
                                    const char *tag = weechat_hdata_string(hdata_line_data, line_data, str_tag);
                                    if (tag && weechat_strcasecmp(tag, target_tag.c_str()) == 0)
                                        found = true;
                                }
                                if (found)
                                {
                                    const char *cur_msg = weechat_hdata_string(hdata_line_data, line_data, "message");
                                    std::string new_msg = cur_msg ? cur_msg : "";
                                    // Strip any pending/delivered glyph suffix
                                    for (const char *glyph : { " ⌛", " ✓", " ✓✓" })
                                    {
                                        std::string g(glyph);
                                        if (new_msg.size() >= g.size() &&
                                            new_msg.compare(new_msg.size() - g.size(), g.size(), g) == 0)
                                        {
                                            new_msg.erase(new_msg.size() - g.size());
                                            break;
                                        }
                                    }
                                    new_msg += " ✓";
                                    struct t_hashtable *ht = weechat_hashtable_new(4,
                                        WEECHAT_HASHTABLE_STRING, WEECHAT_HASHTABLE_STRING, NULL, NULL);
                                    weechat_hashtable_set(ht, "message", new_msg.c_str());
                                    weechat_hdata_update(hdata_line_data, line_data, ht);
                                    weechat_hashtable_free(ht);
                                    break;
                                }
                            }
                            last_line = weechat_hdata_pointer(hdata_line, last_line, "prev_line");
                        }
                    }
                    xmpp_free(account.context, (void*)bare);
                }
                return 1;
            }
        }

        // XEP-0333: Chat Markers — handle <displayed> and <read> from others
        {
            xmpp_stanza_t *marker_displayed = xmpp_stanza_get_child_by_name_and_ns(
                stanza, "displayed", "urn:xmpp:chat-markers:0");
            xmpp_stanza_t *marker_read = xmpp_stanza_get_child_by_name_and_ns(
                stanza, "read", "urn:xmpp:chat-markers:0");
            xmpp_stanza_t *marker = marker_displayed ? marker_displayed : marker_read;
            if (marker)
            {
                const char *marker_from = xmpp_stanza_get_from(stanza);
                const char *acked_id = xmpp_stanza_get_id(marker);
                if (marker_from && acked_id)
                {
                    const char *bare = xmpp_jid_bare(account.context, marker_from);
                    weechat::channel *ch = account.channels.contains(bare)
                        ? &account.channels.find(bare)->second : nullptr;
                    if (ch)
                    {
                        // Find the sent message line tagged id_<acked_id> and update glyph ⌛/✓→✓✓
                        struct t_hdata *hdata_line      = weechat_hdata_get("line");
                        struct t_hdata *hdata_line_data = weechat_hdata_get("line_data");
                        struct t_hdata *hdata_lines     = weechat_hdata_get("lines");
                        struct t_hdata *hdata_buffer    = weechat_hdata_get("buffer");
                        void *lines     = weechat_hdata_pointer(hdata_buffer, ch->buffer, "lines");
                        void *last_line = lines ? weechat_hdata_pointer(hdata_lines, lines, "last_line") : nullptr;
                        std::string target_tag = std::string("id_") + acked_id;
                        while (last_line)
                        {
                            void *line_data = weechat_hdata_pointer(hdata_line, last_line, "data");
                            if (line_data)
                            {
                                int tags_count = weechat_hdata_integer(hdata_line_data, line_data, "tags_count");
                                char str_tag[24] = {0};
                                bool found = false;
                                for (int n = 0; n < tags_count && !found; n++)
                                {
                                    snprintf(str_tag, sizeof(str_tag), "%d|tags_array", n);
                                    const char *tag = weechat_hdata_string(hdata_line_data, line_data, str_tag);
                                    if (tag && weechat_strcasecmp(tag, target_tag.c_str()) == 0)
                                        found = true;
                                }
                                if (found)
                                {
                                    const char *cur_msg = weechat_hdata_string(hdata_line_data, line_data, "message");
                                    std::string new_msg = cur_msg ? cur_msg : "";
                                    // Strip any pending/delivered/seen glyph suffix
                                    for (const char *glyph : { " ⌛", " ✓", " ✓✓" })
                                    {
                                        std::string g(glyph);
                                        if (new_msg.size() >= g.size() &&
                                            new_msg.compare(new_msg.size() - g.size(), g.size(), g) == 0)
                                        {
                                            new_msg.erase(new_msg.size() - g.size());
                                            break;
                                        }
                                    }
                                    new_msg += " ✓✓";
                                    struct t_hashtable *ht = weechat_hashtable_new(4,
                                        WEECHAT_HASHTABLE_STRING, WEECHAT_HASHTABLE_STRING, NULL, NULL);
                                    weechat_hashtable_set(ht, "message", new_msg.c_str());
                                    weechat_hdata_update(hdata_line_data, line_data, ht);
                                    weechat_hashtable_free(ht);
                                    break;
                                }
                            }
                            last_line = weechat_hdata_pointer(hdata_line, last_line, "prev_line");
                        }
                    }
                    xmpp_free(account.context, (void*)bare);
                }
                return 1;
            }
        }

        // XEP-0224: Attention — handle incoming <attention> from others
        {
            xmpp_stanza_t *attention = xmpp_stanza_get_child_by_name_and_ns(
                stanza, "attention", "urn:xmpp:attention:0");
            if (attention)
            {
                const char *attn_from = xmpp_stanza_get_from(stanza);
                if (attn_from)
                {
                    const char *bare = xmpp_jid_bare(account.context, attn_from);
                    weechat::channel *ch = account.channels.contains(bare)
                        ? &account.channels.find(bare)->second : nullptr;
                    if (!ch)
                    {
                        // Create PM channel for the buzz
                        ch = &account.channels.emplace(
                            std::make_pair(bare, weechat::channel {
                                    account, weechat::channel::chat_type::PM, bare, bare
                                })).first->second;
                    }
                    weechat_printf_date_tags(ch->buffer, 0, "xmpp_attention,notify_highlight",
                                            "%s%s is requesting your attention! (/buzz)",
                                            weechat_prefix("network"),
                                            bare);
                    xmpp_free(account.context, (void*)bare);
                }
                return 1;
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
        weechat::channel::unread unread_val;
        unread_val.id = id;
        unread_val.thread = thread ? std::optional<std::string>(thread) : std::nullopt;
        auto unread = &unread_val;

        xmpp_stanza_t *message = xmpp_message_new(account.context, NULL,
                                                  channel->id.data(), NULL);

        if (request)
        {
            xmpp_stanza_t *message__received = xmpp_stanza_new(account.context);
            xmpp_stanza_set_name(message__received, "received");
            xmpp_stanza_set_ns(message__received, "urn:xmpp:receipts");
            xmpp_stanza_set_id(message__received, unread->id.c_str());

            xmpp_stanza_add_child(message, message__received);
            xmpp_stanza_release(message__received);
        }

        if (markable)
        {
            xmpp_stanza_t *message__received = xmpp_stanza_new(account.context);
            xmpp_stanza_set_name(message__received, "received");
            xmpp_stanza_set_ns(message__received, "urn:xmpp:chat-markers:0");
            xmpp_stanza_set_id(message__received, unread->id.c_str());

            xmpp_stanza_add_child(message, message__received);
            xmpp_stanza_release(message__received);
        }

        if (unread->thread.has_value())
        {
            xmpp_stanza_t *message__thread = xmpp_stanza_new(account.context);
            xmpp_stanza_set_name(message__thread, "thread");

            xmpp_stanza_t *message__thread__text = xmpp_stanza_new(account.context);
            xmpp_stanza_set_text(message__thread__text, unread->thread->c_str());
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

    // XEP-0071: XHTML-IM — prefer <html><body> rich rendering over plain <body>.
    // We apply it whenever XHTML is present AND the message is not encrypted
    // (encrypted messages have no usable XHTML anyway).
    // If plain <body> is also absent we use the XHTML as the sole text source.
    std::string xhtml_fallback;
    if (!encrypted && !x)
    {
        xmpp_stanza_t *html_elem = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "html", "http://jabber.org/protocol/xhtml-im");
        if (html_elem)
        {
            xmpp_stanza_t *html_body = xmpp_stanza_get_child_by_name(html_elem, "body");
            if (html_body)
            {
                xhtml_fallback = xhtml_to_weechat(html_body);
                // Trim leading/trailing whitespace
                auto trim_pos = xhtml_fallback.find_first_not_of(" \t\n\r");
                if (trim_pos != std::string::npos)
                    xhtml_fallback = xhtml_fallback.substr(trim_pos);
                trim_pos = xhtml_fallback.find_last_not_of(" \t\n\r");
                if (trim_pos != std::string::npos)
                    xhtml_fallback = xhtml_fallback.substr(0, trim_pos + 1);
                if (!xhtml_fallback.empty())
                {
                    // Free original intext (if any) and replace with rich version
                    if (intext) xmpp_free(account.context, intext);
                    // xmpp_strdup is deprecated but is the only allocator-paired
                    // string dup available; xmpp_alloc/xmpp_strndup are equally deprecated
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
                    intext = xmpp_strdup(account.context, xhtml_fallback.c_str());
#pragma GCC diagnostic pop
                }
            }
        }
    }
    
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
        cleartext = account.omemo.decode(&account, channel->buffer, from_bare, encrypted);
        if (!cleartext)
        {
            weechat_printf_date_tags(channel->buffer, 0, "notify_none", "%s%s (%s)",
                                     weechat_prefix("error"), "OMEMO Decryption Error", from);
            return 1;
        }
    }
    else
    {
        if (encrypted)
            weechat_printf(NULL, "%sOMEMO: encrypted message but account.omemo is NULL/false",
                           weechat_prefix("error"));
    }
    if (x)
    {
        char *ciphertext = xmpp_stanza_get_text(x);
        cleartext = account.pgp.decrypt(channel->buffer, ciphertext);
        xmpp_free(account.context, ciphertext);
    }
    text = cleartext ? cleartext : intext;

    // XEP-0428: Fallback Indication — if <fallback> is present the <body> is
    // just a compatibility string; suppress it so we don't double-print.
    // (The actual content was already rendered by reactions/retract/reply code.)
    xmpp_stanza_t *fallback_elem = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "fallback", "urn:xmpp:fallback:0");
    if (fallback_elem && text && !replace)
    {
        // Suppress if there's a meaningful stanza type alongside the body:
        // reactions, retract, reply, or apply-to (moderation / fastening)
        xmpp_stanza_t *reactions = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "reactions", "urn:xmpp:reactions:0");
        xmpp_stanza_t *retract = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "retract", "urn:xmpp:message-retract:1");
        if (!retract)
            retract = xmpp_stanza_get_child_by_name_and_ns(
                stanza, "retract", "urn:xmpp:message-retract:0");
        xmpp_stanza_t *reply = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "reply", "urn:xmpp:reply:0");
        xmpp_stanza_t *apply_to = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "apply-to", "urn:xmpp:fasten:0");
        if (reactions || retract || reply || apply_to)
            text = nullptr;  // suppress fallback body; the handler above will print
    }

    // XEP-0382: Spoiler Messages — display hint before the body
    xmpp_stanza_t *spoiler_elem = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "spoiler", "urn:xmpp:spoiler:0");
    const char *spoiler_hint = spoiler_elem ? xmpp_stanza_get_text(spoiler_elem) : nullptr;

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
        date = timegm(&time);
    }

    // XEP-0085: receiving a message implicitly clears the composing state
    // Only do this for live (non-delayed) messages from others
    if (!delay && from_bare && weechat_strcasecmp(from_bare, account.jid().data()) != 0)
    {
        auto *msg_user = user::search(&account, from);
        if (msg_user)
            channel->remove_typing(msg_user);
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
    {
        weechat_string_dyn_concat(dyn_tags, ",notify_message,log1", -1);

        // XEP-0372: References — check for explicit @mention targeting local JID
        // A <reference type="mention" uri="xmpp:user@server"> forces highlight.
        bool xep0372_mentioned = false;
        for (xmpp_stanza_t *ref = xmpp_stanza_get_children(stanza);
             ref && !xep0372_mentioned; ref = xmpp_stanza_get_next(ref))
        {
            const char *ref_ns = xmpp_stanza_get_ns(ref);
            const char *ref_name = xmpp_stanza_get_name(ref);
            if (!ref_ns || !ref_name) continue;
            if (strcmp(ref_name, "reference") != 0
                || strcmp(ref_ns, "urn:xmpp:reference:0") != 0)
                continue;
            const char *ref_type = xmpp_stanza_get_attribute(ref, "type");
            if (!ref_type || strcmp(ref_type, "mention") != 0) continue;
            const char *ref_uri = xmpp_stanza_get_attribute(ref, "uri");
            if (!ref_uri) continue;
            // URI is "xmpp:user@server" or "xmpp:user@server/resource"
            const char *colon = strchr(ref_uri, ':');
            if (!colon) continue;
            std::string mentioned_jid(colon + 1);
            // Strip resource if present
            auto slash = mentioned_jid.find('/');
            if (slash != std::string::npos)
                mentioned_jid = mentioned_jid.substr(0, slash);
            char *local_bare = xmpp_jid_bare(account.context, account.jid().data());
            if (local_bare && weechat_strcasecmp(mentioned_jid.c_str(), local_bare) == 0)
                xep0372_mentioned = true;
            xmpp_free(account.context, local_bare);
        }
        if (xep0372_mentioned)
            weechat_string_dyn_concat(dyn_tags, ",notify_highlight", -1);
    }

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

    // XEP-0385: Stateless Inline Media Sharing (SIMS)
    // Parse <reference type="data"><media-sharing xmlns="urn:xmpp:sims:1">
    std::string sims_suffix;
    for (xmpp_stanza_t *ref = xmpp_stanza_get_children(stanza);
         ref; ref = xmpp_stanza_get_next(ref))
    {
        const char *ref_name = xmpp_stanza_get_name(ref);
        const char *ref_ns   = xmpp_stanza_get_ns(ref);
        if (!ref_name || !ref_ns) continue;
        if (strcmp(ref_name, "reference") != 0
            || strcmp(ref_ns, "urn:xmpp:reference:0") != 0) continue;

        const char *ref_type = xmpp_stanza_get_attribute(ref, "type");
        if (!ref_type || strcmp(ref_type, "data") != 0) continue;

        xmpp_stanza_t *ms = xmpp_stanza_get_child_by_name_and_ns(
            ref, "media-sharing", "urn:xmpp:sims:1");
        if (!ms) continue;

        // Extract file info from <file xmlns='urn:xmpp:jingle:apps:file-transfer:5'>
        xmpp_stanza_t *file_elem = xmpp_stanza_get_child_by_name_and_ns(
            ms, "file", "urn:xmpp:jingle:apps:file-transfer:5");

        std::string sims_name, sims_mime, sims_size_str;
        if (file_elem)
        {
            xmpp_stanza_t *name_e = xmpp_stanza_get_child_by_name(file_elem, "name");
            xmpp_stanza_t *mime_e = xmpp_stanza_get_child_by_name(file_elem, "media-type");
            xmpp_stanza_t *size_e = xmpp_stanza_get_child_by_name(file_elem, "size");

            if (name_e) { char *t = xmpp_stanza_get_text(name_e); if (t) { sims_name = t; xmpp_free(account.context, t); } }
            if (mime_e) { char *t = xmpp_stanza_get_text(mime_e); if (t) { sims_mime = t; xmpp_free(account.context, t); } }
            if (size_e) { char *t = xmpp_stanza_get_text(size_e); if (t) { sims_size_str = t; xmpp_free(account.context, t); } }
        }

        // Extract first source URL from <sources>
        xmpp_stanza_t *sources = xmpp_stanza_get_child_by_name(ms, "sources");
        std::string sims_url;
        if (sources)
        {
            // Try <reference type="data" uri="https://...">
            for (xmpp_stanza_t *src = xmpp_stanza_get_children(sources);
                 src && sims_url.empty(); src = xmpp_stanza_get_next(src))
            {
                const char *sname = xmpp_stanza_get_name(src);
                const char *sns   = xmpp_stanza_get_ns(src);
                if (!sname || !sns) continue;
                if (strcmp(sname, "reference") == 0
                    && strcmp(sns, "urn:xmpp:reference:0") == 0)
                {
                    const char *uri = xmpp_stanza_get_attribute(src, "uri");
                    if (uri) sims_url = uri;
                }
                else if (strcmp(sname, "url-data") == 0)
                {
                    const char *target = xmpp_stanza_get_attribute(src, "target");
                    if (target) sims_url = target;
                }
            }
        }

        if (!sims_url.empty())
        {
            // Build display line: [File: name (mime, size) URL]
            sims_suffix += std::string("\n") + weechat_color("cyan") + "[File: ";
            if (!sims_name.empty())
                sims_suffix += sims_name;
            else
                sims_suffix += sims_url;

            if (!sims_mime.empty() || !sims_size_str.empty())
            {
                sims_suffix += " (";
                if (!sims_mime.empty())
                    sims_suffix += sims_mime;
                if (!sims_mime.empty() && !sims_size_str.empty())
                    sims_suffix += ", ";
                if (!sims_size_str.empty())
                {
                    // Human-readable size
                    long long sz = std::stoll(sims_size_str);
                    if (sz >= 1024 * 1024)
                        sims_suffix += fmt::format("{:.1f} MB", sz / 1048576.0);
                    else if (sz >= 1024)
                        sims_suffix += fmt::format("{:.1f} KB", sz / 1024.0);
                    else
                        sims_suffix += fmt::format("{} B", sz);
                }
                sims_suffix += ")";
            }
            sims_suffix += " " + sims_url;
            sims_suffix += "]" + std::string(weechat_color("resetcolor"));

            // If OOB already shows this same URL, suppress the OOB suffix to avoid duplication
            if (!oob_suffix.empty() && oob_suffix.find(sims_url) != std::string::npos)
                oob_suffix.clear();
        }
    }

    // XEP-0511: Link Metadata — parse <rdf:Description> containing OpenGraph metadata
    xmpp_stanza_t *rdf_desc = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "Description", "http://www.w3.org/1999/02/22-rdf-syntax-ns#");
    std::string og_preview;
    if (rdf_desc)
    {
        // Multiple <rdf:Description> children may appear (one per URL); collect all previews
        for (xmpp_stanza_t *desc = rdf_desc; desc;
             desc = xmpp_stanza_get_next(desc))
        {
            const char *desc_name = xmpp_stanza_get_name(desc);
            const char *desc_ns   = xmpp_stanza_get_ns(desc);
            if (!desc_name || !desc_ns) continue;
            if (strcmp(desc_name, "Description") != 0
                || strcmp(desc_ns, "http://www.w3.org/1999/02/22-rdf-syntax-ns#") != 0)
                continue;

            std::string og_title, og_desc, og_url, og_image;
            for (xmpp_stanza_t *prop = xmpp_stanza_get_children(desc);
                 prop; prop = xmpp_stanza_get_next(prop))
            {
                const char *prop_name = xmpp_stanza_get_name(prop);
                const char *prop_ns   = xmpp_stanza_get_ns(prop);
                if (!prop_name || !prop_ns) continue;
                if (strcmp(prop_ns, "https://ogp.me/ns#") != 0) continue;

                char *val = xmpp_stanza_get_text(prop);
                if (!val) continue;

                if (strcmp(prop_name, "title") == 0 && og_title.empty())
                    og_title = val;
                else if (strcmp(prop_name, "description") == 0 && og_desc.empty())
                    og_desc = val;
                else if (strcmp(prop_name, "url") == 0 && og_url.empty())
                    og_url = val;
                else if (strcmp(prop_name, "image") == 0 && og_image.empty())
                {
                    // Only store HTTP(S) image URLs; skip cid:, ni:, data: URIs
                    if (strncmp(val, "http", 4) == 0)
                        og_image = val;
                }
                xmpp_free(account.context, val);
            }

            if (!og_title.empty() || !og_desc.empty() || !og_url.empty())
            {
                og_preview += "\n\t";
                og_preview += weechat_color("darkgray");
                og_preview += "┌ ";
                og_preview += weechat_color("bold");
                og_preview += og_title.empty() ? (og_url.empty() ? "Link" : og_url) : og_title;
                og_preview += weechat_color("-bold");
                if (!og_desc.empty())
                {
                    og_preview += "\n\t";
                    og_preview += weechat_color("darkgray");
                    og_preview += "│ ";
                    og_preview += weechat_color("resetcolor");
                    og_preview += weechat_color("darkgray");
                    // Truncate long descriptions to ~120 chars
                    if (og_desc.size() > 120)
                    {
                        og_preview += og_desc.substr(0, 117);
                        og_preview += "...";
                    }
                    else
                    {
                        og_preview += og_desc;
                    }
                }
                if (!og_url.empty() && og_url != og_title)
                {
                    og_preview += "\n\t";
                    og_preview += weechat_color("darkgray");
                    og_preview += "└ ";
                    og_preview += weechat_color("blue");
                    og_preview += og_url;
                }
                if (!og_image.empty())
                {
                    og_preview += " ";
                    og_preview += weechat_color("darkgray");
                    og_preview += "[img]";
                }
                og_preview += weechat_color("resetcolor");
            }
        }
    }

    // Apply XEP-0393 Message Styling
    // Skip if <unstyled xmlns='urn:xmpp:styling:0'/> hint is present
    const char *display_text = text;
    std::string styled_text;
    bool has_unstyled = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "unstyled", "urn:xmpp:styling:0") != nullptr;
    if (text && !difftext && !has_unstyled)  // Don't style diffs (already styled)
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

    // XEP-0382: Spoiler Messages — prepend spoiler warning before the body
    if (spoiler_hint)
    {
        std::string spoiler_prefix = std::string(weechat_color("yellow"))
            + "[Spoiler"
            + (spoiler_hint && strlen(spoiler_hint) > 0
               ? std::string(": ") + spoiler_hint
               : std::string(""))
            + "] "
            + weechat_color("resetcolor");
        if (final_text.empty())
            final_text = display_text ? display_text : "";
        final_text = spoiler_prefix + final_text;
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

    // Append SIMS file info if present (XEP-0385)
    if (!sims_suffix.empty())
    {
        if (final_text.empty())
            final_text = display_text ? display_text : "";
        final_text += sims_suffix;
        display_text = final_text.c_str();
    }

    // Append XEP-0511 link preview if present
    if (!og_preview.empty())
    {
        if (final_text.empty())
            final_text = display_text ? display_text : "";
        final_text += og_preview;
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

    // Smart filter: record that this nick spoke, so future presence lines are shown.
    // Only for live (non-delayed) messages not sent by self.
    if (!date && nick && channel
        && weechat_strcasecmp(from_bare, account.jid().data()) != 0)
    {
        // For MUC, nick is the resource (occupant nick); that is what we track.
        channel->record_speak(nick);
        // After the user speaks, clear the joining flag so we don't keep suppressing
        channel->joining = false;
    }

    weechat_string_dyn_free(dyn_tags, 1);

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
    FEATURE("urn:xmpp:blocking");      // XEP-0191: Blocking Command
    FEATURE("urn:xmpp:bookmarks:1");  // XEP-0402: PEP Native Bookmarks
    FEATURE("urn:xmpp:bookmarks:1+notify");  // Subscribe to bookmark updates
    FEATURE("urn:xmpp:carbons:2");     // XEP-0280: Message Carbons
    FEATURE("urn:xmpp:ping");
    FEATURE("urn:xmpp:receipts");
    FEATURE("urn:xmpp:time");
    FEATURE("urn:xmpp:attention:0");   // XEP-0224: Attention
    FEATURE("urn:xmpp:spoiler:0");     // XEP-0382: Spoiler Messages
    FEATURE("urn:xmpp:fallback:0");    // XEP-0428: Fallback Indication
    FEATURE("vcard-temp:x:update");    // XEP-0153: vCard-Based Avatars
    FEATURE("urn:xmpp:reference:0");   // XEP-0372: References (mentions)
    FEATURE("http://jabber.org/protocol/commands");  // XEP-0050: Ad-Hoc Commands
    FEATURE("urn:xmpp:mam:2");                               // XEP-0313: Message Archive Management
    FEATURE("urn:xmpp:mds:displayed:0");             // XEP-0490: Message Displayed Synchronization
    FEATURE("urn:xmpp:mds:displayed:0+notify");      // Subscribe to MDS events
    FEATURE("urn:xmpp:channel-search:0:search");     // XEP-0433: Extended Channel Search (searcher)
    FEATURE("urn:ietf:params:xml:ns:vcard-4.0");     // XEP-0292: vCard4 Over XMPP
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

// XEP-0004: Data Forms — render a <x xmlns='jabber:x:data'> form to a buffer
static void render_data_form(struct t_gui_buffer *buf, xmpp_stanza_t *x_form,
                              const char *jid, const char *node, const char *session_id)
{
    if (!x_form || !buf) return;

    xmpp_stanza_t *title_elem = xmpp_stanza_get_child_by_name(x_form, "title");
    const char *title = title_elem ? xmpp_stanza_get_text_ptr(title_elem) : NULL;
    xmpp_stanza_t *instr_elem = xmpp_stanza_get_child_by_name(x_form, "instructions");
    const char *instr = instr_elem ? xmpp_stanza_get_text_ptr(instr_elem) : NULL;

    weechat_printf_date_tags(buf, 0, "xmpp_adhoc,notify_none",
                             "%s%s── Ad-Hoc Form%s%s%s ──%s",
                             weechat_prefix("network"),
                             weechat_color("bold"),
                             title ? ": " : "",
                             title ? title : "",
                             title ? "" : "",
                             weechat_color("-bold"));
    if (instr)
        weechat_printf_date_tags(buf, 0, "xmpp_adhoc,notify_none",
                                 "%s  %s%s%s",
                                 weechat_prefix("network"),
                                 weechat_color("italic"),
                                 instr,
                                 weechat_color("-italic"));

    int field_index = 0;

    // Print each field
    for (xmpp_stanza_t *field = xmpp_stanza_get_children(x_form);
         field; field = xmpp_stanza_get_next(field))
    {
        const char *fname = xmpp_stanza_get_name(field);
        if (!fname || strcmp(fname, "field") != 0) continue;
        const char *var   = xmpp_stanza_get_attribute(field, "var");
        const char *label = xmpp_stanza_get_attribute(field, "label");
        const char *ftype = xmpp_stanza_get_attribute(field, "type");

        // Skip hidden fields — not user-visible
        if (ftype && strcmp(ftype, "hidden") == 0) continue;

        field_index++;

        // Collect all <value> children (text-multi and list-multi can have several)
        std::vector<std::string> values;
        for (xmpp_stanza_t *v = xmpp_stanza_get_children(field);
             v; v = xmpp_stanza_get_next(v))
        {
            const char *vname = xmpp_stanza_get_name(v);
            if (!vname || strcmp(vname, "value") != 0) continue;
            const char *vtext = xmpp_stanza_get_text_ptr(v);
            if (vtext) values.push_back(vtext);
        }

        // Check if field is required
        bool required = (xmpp_stanza_get_child_by_name(field, "required") != nullptr);

        // Determine display value string
        bool is_password = ftype && strcmp(ftype, "text-private") == 0;
        bool is_boolean  = ftype && strcmp(ftype, "boolean") == 0;
        bool is_fixed    = ftype && strcmp(ftype, "fixed") == 0;
        bool is_list     = ftype && (strcmp(ftype, "list-single") == 0
                                  || strcmp(ftype, "list-multi") == 0);

        // Fixed fields are just informational text — print them differently
        if (is_fixed)
        {
            for (auto &v : values)
                weechat_printf_date_tags(buf, 0, "xmpp_adhoc,notify_none",
                                         "%s  %s%s%s",
                                         weechat_prefix("network"),
                                         weechat_color("darkgray"),
                                         v.c_str(),
                                         weechat_color("resetcolor"));
            continue;
        }

        // Build current-value display
        std::string val_display;
        if (is_password)
        {
            val_display = values.empty() ? "(empty)" : "********";
        }
        else if (is_boolean)
        {
            if (values.empty())
                val_display = "false";
            else
            {
                const auto &v = values[0];
                bool on = (v == "1" || v == "true");
                val_display  = std::string(weechat_color(on ? "green" : "red"));
                val_display += on ? "true" : "false";
                val_display += weechat_color("resetcolor");
            }
        }
        else if (values.empty())
        {
            val_display = std::string(weechat_color("darkgray")) + "(empty)"
                        + weechat_color("resetcolor");
        }
        else if (values.size() == 1)
        {
            val_display = values[0];
        }
        else
        {
            // text-multi / list-multi: join with " | "
            for (size_t i = 0; i < values.size(); ++i)
            {
                if (i) val_display += std::string(weechat_color("darkgray"))
                                    + " | " + weechat_color("resetcolor");
                val_display += values[i];
            }
        }

        // Collect available options for list-single/list-multi
        // For the selected value(s), highlight them
        std::string options_str;
        if (is_list)
        {
            for (xmpp_stanza_t *opt = xmpp_stanza_get_children(field);
                 opt; opt = xmpp_stanza_get_next(opt))
            {
                const char *oname = xmpp_stanza_get_name(opt);
                if (!oname || strcmp(oname, "option") != 0) continue;

                xmpp_stanza_t *oval_elem = xmpp_stanza_get_child_by_name(opt, "value");
                const char *oval  = oval_elem ? xmpp_stanza_get_text_ptr(oval_elem) : nullptr;
                const char *olabel = xmpp_stanza_get_attribute(opt, "label");

                if (!options_str.empty()) options_str += "  ";

                // Is this option currently selected?
                bool selected = false;
                for (auto &v : values)
                    if (oval && v == oval) { selected = true; break; }

                if (selected)
                    options_str += weechat_color("green");
                else
                    options_str += weechat_color("darkgray");

                options_str += oval ? oval : "?";
                if (olabel && oval && strcmp(olabel, oval) != 0)
                {
                    options_str += '(';
                    options_str += olabel;
                    options_str += ')';
                }
                if (selected)
                    options_str += weechat_color("resetcolor");
                else
                    options_str += weechat_color("resetcolor");
            }
        }

        // Field type badge color
        const char *type_color = "gray";
        if (ftype)
        {
            if (strcmp(ftype, "text-single") == 0 || strcmp(ftype, "text-multi") == 0)
                type_color = "cyan";
            else if (is_boolean)
                type_color = "yellow";
            else if (is_list)
                type_color = "214"; // orange
            else if (is_password)
                type_color = "red";
        }

        // Print: "  N. Label [var/type] = value"
        weechat_printf_date_tags(buf, 0, "xmpp_adhoc,notify_none",
                                 "%s  %s%d.%s %s%s%s%s %s[%s%s%s%s%s]%s = %s%s",
                                 weechat_prefix("network"),
                                 // index
                                 weechat_color("darkgray"), field_index, weechat_color("resetcolor"),
                                 // label (bold), required marker
                                 weechat_color("bold"),
                                 label ? label : (var ? var : "?"),
                                 required ? "*" : "",
                                 weechat_color("-bold"),
                                 // [var/type]
                                 weechat_color("darkgray"),
                                 weechat_color(type_color),
                                 var ? var : "?",
                                 ftype ? "/" : "",
                                 ftype ? ftype : "",
                                 weechat_color("darkgray"),
                                 weechat_color("resetcolor"),
                                 // value
                                 val_display.c_str(),
                                 // options on same line for lists
                                 options_str.empty() ? "" : (std::string("  ") + options_str).c_str());
    }

    // Show how to submit
    if (session_id && node && jid)
        weechat_printf_date_tags(buf, 0, "xmpp_adhoc,notify_none",
                                 "%s  %sSubmit:%s /adhoc %s %s %s %svar%s=%svalue%s ...",
                                 weechat_prefix("network"),
                                 weechat_color("gray"), weechat_color("resetcolor"),
                                 jid, node, session_id,
                                 weechat_color("cyan"), weechat_color("resetcolor"),
                                 weechat_color("yellow"), weechat_color("resetcolor"));
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
    // XEP-0283: Moved — detect JID migration notice
    {
        xmpp_stanza_t *moved_elem = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "moved", "urn:xmpp:moved:1");
        if (moved_elem)
        {
            const char *new_jid = xmpp_stanza_get_attribute(moved_elem, "new-jid");
            if (!new_jid)
            {
                // Some implementations put the new JID as text content
                new_jid = xmpp_stanza_get_text(moved_elem);
            }
            const char *old_jid_full = binding.from ? binding.from->full.data() : "unknown";
            char *old_jid_bare = binding.from
                ? xmpp_jid_bare(account.context, binding.from->full.data()) : nullptr;
            if (new_jid)
                weechat_printf_date_tags(account.buffer, 0, "xmpp_presence,notify_highlight",
                                         "%s%sContact %s%s%s has moved to %s%s%s — update your roster",
                                         weechat_prefix("network"),
                                         weechat_color("yellow"),
                                         weechat_color("bold"),
                                         old_jid_bare ? old_jid_bare : old_jid_full,
                                         weechat_color("reset"),
                                         weechat_color("bold"),
                                         new_jid,
                                         weechat_color("reset"));
            if (old_jid_bare)
                xmpp_free(account.context, old_jid_bare);
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

        // Check if this is a /setvcard read-merge response (self-fetch before update).
        if (id)
        {
            auto sv_it = account.setvcard_queries.find(id);
            if (sv_it != account.setvcard_queries.end())
            {
                auto &sv = sv_it->second;
                struct t_gui_buffer *sv_buf = sv.buffer;

                // Build a vcard_fields struct pre-populated from the server's vCard.
                auto ctext = [&](xmpp_stanza_t *parent, const char *name) -> std::string {
                    xmpp_stanza_t *ch = xmpp_stanza_get_child_by_name(parent, name);
                    if (!ch) return {};
                    char *txt = xmpp_stanza_get_text(ch);
                    if (!txt) return {};
                    std::string s(txt); xmpp_free(account.context, txt); return s;
                };
                ::xmpp::xep0054::vcard_fields f;
                f.fn       = ctext(vcard, "FN");
                f.nickname = ctext(vcard, "NICKNAME");
                f.url      = ctext(vcard, "URL");
                f.desc     = ctext(vcard, "DESC");
                f.bday     = ctext(vcard, "BDAY");
                f.note     = ctext(vcard, "NOTE");
                f.title    = ctext(vcard, "TITLE");
                {
                    xmpp_stanza_t *org_el = xmpp_stanza_get_child_by_name(vcard, "ORG");
                    if (org_el) f.org = ctext(org_el, "ORGNAME");
                }
                {
                    xmpp_stanza_t *email_el = xmpp_stanza_get_child_by_name(vcard, "EMAIL");
                    if (email_el) f.email = ctext(email_el, "USERID");
                }
                {
                    xmpp_stanza_t *tel_el = xmpp_stanza_get_child_by_name(vcard, "TEL");
                    if (tel_el) f.tel = ctext(tel_el, "NUMBER");
                }

                // Apply the requested override.
                const std::string &fld = sv.field;
                const std::string &val = sv.value;
                if      (fld == "fn")       f.fn       = val;
                else if (fld == "nickname") f.nickname = val;
                else if (fld == "email")    f.email    = val;
                else if (fld == "url")      f.url      = val;
                else if (fld == "desc")     f.desc     = val;
                else if (fld == "org")      f.org      = val;
                else if (fld == "title")    f.title    = val;
                else if (fld == "tel")      f.tel      = val;
                else if (fld == "bday")     f.bday     = val;
                else if (fld == "note")     f.note     = val;

                // Publish the merged vCard.
                xmpp_stanza_t *set_iq = ::xmpp::xep0054::vcard_set(account.context, f);
                account.connection.send(set_iq);
                xmpp_stanza_release(set_iq);
                weechat_printf(sv_buf, "%svCard field %s updated",
                               weechat_prefix("network"), fld.c_str());

                account.setvcard_queries.erase(sv_it);
                return true;
            }
        }

        // Determine which buffer to print into: the one that issued /whois, or
        // the account buffer for auto-fetched vCards (XEP-0153 trigger).
        struct t_gui_buffer *target_buf = account.buffer;
        if (id)
        {
            auto it = account.whois_queries.find(id);
            if (it != account.whois_queries.end())
            {
                target_buf = it->second.buffer;
                account.whois_queries.erase(it);
            }
        }

        // Helper: get direct text content of a child element
        auto child_text = [&](xmpp_stanza_t *parent, const char *name) -> std::string {
            xmpp_stanza_t *child = xmpp_stanza_get_child_by_name(parent, name);
            if (!child) return {};
            char *txt = xmpp_stanza_get_text(child);
            if (!txt) return {};
            std::string s(txt);
            xmpp_free(account.context, txt);
            return s;
        };

        // Helper: print a labelled line only if value is non-empty
        auto print_field = [&](const char *label, const std::string &val) {
            if (!val.empty())
                weechat_printf(target_buf, "  %s%s%s %s",
                               weechat_color("bold"), label,
                               weechat_color("reset"), val.c_str());
        };

        weechat_printf(target_buf, "%svCard for %s:",
                       weechat_prefix("network"), from_jid);

        std::string fn       = child_text(vcard, "FN");
        std::string nickname = child_text(vcard, "NICKNAME");
        std::string url      = child_text(vcard, "URL");
        std::string desc     = child_text(vcard, "DESC");
        std::string bday     = child_text(vcard, "BDAY");
        std::string note     = child_text(vcard, "NOTE");
        std::string jabbid   = child_text(vcard, "JABBERID");
        std::string title    = child_text(vcard, "TITLE");
        std::string role_vc  = child_text(vcard, "ROLE");

        // ORG: <ORG><ORGNAME>…</ORGNAME></ORG>
        std::string org;
        xmpp_stanza_t *org_el = xmpp_stanza_get_child_by_name(vcard, "ORG");
        if (org_el) org = child_text(org_el, "ORGNAME");

        // EMAIL: <EMAIL><USERID>…</USERID></EMAIL>  (first occurrence)
        std::string email_val;
        xmpp_stanza_t *email_el = xmpp_stanza_get_child_by_name(vcard, "EMAIL");
        if (email_el) email_val = child_text(email_el, "USERID");

        // TEL: <TEL><NUMBER>…</NUMBER></TEL>  (first occurrence)
        std::string tel;
        xmpp_stanza_t *tel_el = xmpp_stanza_get_child_by_name(vcard, "TEL");
        if (tel_el) tel = child_text(tel_el, "NUMBER");

        // ADR: <ADR><STREET>…</STREET><LOCALITY>…</LOCALITY><CTRY>…</CTRY></ADR>
        std::string adr;
        xmpp_stanza_t *adr_el = xmpp_stanza_get_child_by_name(vcard, "ADR");
        if (adr_el)
        {
            for (const char *part : {"STREET", "LOCALITY", "REGION", "PCODE", "CTRY"})
            {
                std::string p = child_text(adr_el, part);
                if (!p.empty())
                {
                    if (!adr.empty()) adr += ", ";
                    adr += p;
                }
            }
        }

        print_field("Full name:",    fn);
        print_field("Nickname:",     nickname);
        print_field("Birthday:",     bday);
        print_field("Organisation:", org);
        print_field("Title:",        title);
        print_field("Role:",         role_vc);
        print_field("Email:",        email_val);
        print_field("Phone:",        tel);
        print_field("Address:",      adr);
        print_field("URL:",          url);
        print_field("JID:",          jabbid);
        print_field("Note:",         note);
        print_field("Description:",  desc);

        // Store into user profile for future reference
        weechat::user *u = weechat::user::search(&account, from_jid);
        if (u)
        {
            if (!fn.empty())       u->profile.fn        = fn;
            if (!nickname.empty()) u->profile.nickname  = nickname;
            if (!email_val.empty()) u->profile.email    = email_val;
            if (!url.empty())      u->profile.url       = url;
            if (!desc.empty())     u->profile.description = desc;
            if (!org.empty())      u->profile.org       = org;
            if (!title.empty())    u->profile.title     = title;
            if (!tel.empty())      u->profile.tel       = tel;
            if (!bday.empty())     u->profile.bday      = bday;
            if (!note.empty())     u->profile.note      = note;
            if (!jabbid.empty())   u->profile.jabberid  = jabbid;
            u->profile.vcard_fetched = true;
        }

        return true;
    }

    // Handle vCard4 PubSub responses (XEP-0292)
    // Arrives as: <iq type='result'><pubsub xmlns='..pubsub'><items node='urn:xmpp:vcard4'>
    //               <item id='current'><vcard xmlns='urn:ietf:params:xml:ns:vcard-4.0'>…</vcard>
    {
        xmpp_stanza_t *pubsub_vc4 = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "pubsub", "http://jabber.org/protocol/pubsub");
        if (pubsub_vc4 && type && weechat_strcasecmp(type, "result") == 0)
        {
            xmpp_stanza_t *items = xmpp_stanza_get_child_by_name(pubsub_vc4, "items");
            if (items)
            {
                const char *node = xmpp_stanza_get_attribute(items, "node");
                if (node && strcmp(node, NS_VCARD4_PUBSUB) == 0)
                {
                    const char *from_jid = from ? from : account.jid().data();

                    struct t_gui_buffer *target_buf = account.buffer;
                    if (id)
                    {
                        auto it = account.whois_queries.find(id);
                        if (it != account.whois_queries.end())
                        {
                            target_buf = it->second.buffer;
                            account.whois_queries.erase(it);
                        }
                    }

                    xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(items, "item");
                    if (item)
                    {
                        xmpp_stanza_t *vcard4 = xmpp_stanza_get_child_by_name_and_ns(
                            item, "vcard", NS_VCARD4);
                        if (vcard4)
                        {
                            weechat_printf(target_buf, "%svCard4 for %s:",
                                           weechat_prefix("network"), from_jid);

                            // Helper: get text of first child matching name inside parent
                            auto vc4_text = [&](xmpp_stanza_t *p, const char *name) -> std::string {
                                xmpp_stanza_t *el = xmpp_stanza_get_child_by_name(p, name);
                                if (!el) return {};
                                // vCard4 wraps values: <text>…</text> or <uri>…</uri>
                                xmpp_stanza_t *val = xmpp_stanza_get_child_by_name(el, "text");
                                if (!val) val = xmpp_stanza_get_child_by_name(el, "uri");
                                if (!val) return {};
                                char *t = xmpp_stanza_get_text(val);
                                if (!t) return {};
                                std::string s(t);
                                xmpp_free(account.context, t);
                                return s;
                            };

                            auto print_vc4 = [&](const char *label, const std::string &val) {
                                if (!val.empty())
                                    weechat_printf(target_buf, "  %s%s%s %s",
                                                   weechat_color("bold"), label,
                                                   weechat_color("reset"), val.c_str());
                            };

                            // vCard4 uses lowercase element names
                            std::string fn       = vc4_text(vcard4, "fn");
                            std::string nickname = vc4_text(vcard4, "nickname");
                            std::string url      = vc4_text(vcard4, "url");
                            std::string note     = vc4_text(vcard4, "note");
                            std::string bday     = vc4_text(vcard4, "bday");
                            std::string title    = vc4_text(vcard4, "title");
                            std::string role_vc4 = vc4_text(vcard4, "role");

                            // email: <email><text>…</text></email>
                            std::string email_v4 = vc4_text(vcard4, "email");

                            // tel: <tel><uri>tel:…</uri></tel>
                            std::string tel_v4 = vc4_text(vcard4, "tel");

                            // org: <org><text>…</text></org>
                            std::string org_v4 = vc4_text(vcard4, "org");

                            print_vc4("Full name:",    fn);
                            print_vc4("Nickname:",     nickname);
                            print_vc4("Birthday:",     bday);
                            print_vc4("Organisation:", org_v4);
                            print_vc4("Title:",        title);
                            print_vc4("Role:",         role_vc4);
                            print_vc4("Email:",        email_v4);
                            print_vc4("Phone:",        tel_v4);
                            print_vc4("URL:",          url);
                            print_vc4("Note:",         note);

                            return true;
                        }
                    }
                }
            }
        }
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
                
                // Async HTTP PUT upload via pipe + worker thread.
                // The worker thread does all blocking I/O (file read, SHA-256,
                // curl PUT) and writes 1 byte to the pipe write-end when done.
                // weechat_hook_fd fires on the read-end in the main thread,
                // which processes the result and sends the XMPP message.

                int pipe_fds[2];
                if (pipe(pipe_fds) != 0)
                {
                    weechat_printf(account.buffer, "%s%s: failed to create pipe for upload",
                                  weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                    account.upload_requests.erase(req_it);
                    return 1;
                }

                // Build the completion context (everything the callback needs)
                auto ctx = std::make_shared<weechat::account::upload_completion>();
                ctx->channel_id    = req_it->second.channel_id;
                ctx->filename      = req_it->second.filename;
                ctx->content_type  = content_type;
                ctx->pipe_write_fd = pipe_fds[1];

                // Copy strings that will be used by the worker thread (the
                // upload_requests entry will be erased below, so we must copy
                // before erasing).
                std::string filepath_copy  = req_it->second.filepath;
                std::string put_url_copy   = put_url;
                std::string get_url_copy   = get_url;

                // Erase the upload_requests entry now (before thread starts)
                account.upload_requests.erase(req_it);

                // Register WeeChat hook on the read-end fd
                ctx->hook = weechat_hook_fd(pipe_fds[0], 1, 0, 0,
                                            &weechat::account::upload_fd_cb,
                                            &account, nullptr);

                // Store in pending_uploads keyed by read-end fd
                account.pending_uploads[pipe_fds[0]] = ctx;

                // Capture everything needed for the thread by value
                std::shared_ptr<weechat::account::upload_completion> ctx_copy = ctx;
                std::vector<std::string> put_headers_copy = put_headers;
                std::string content_type_copy = content_type;

                ctx->worker = std::thread([ctx_copy, filepath_copy,
                                           put_url_copy, get_url_copy,
                                           put_headers_copy, content_type_copy]()
                {
                    auto &c = *ctx_copy;

                    // Open file
                    FILE *upload_file = fopen(filepath_copy.c_str(), "rb");
                    if (!upload_file)
                    {
                        c.success   = false;
                        c.curl_error = "failed to open file";
                        ::write(c.pipe_write_fd, "x", 1);
                        return;
                    }

                    // Get file size
                    fseek(upload_file, 0, SEEK_END);
                    long file_size = ftell(upload_file);
                    fseek(upload_file, 0, SEEK_SET);
                    c.file_size = static_cast<size_t>(file_size);

                    // Calculate SHA-256 hash for SIMS using EVP API
                    unsigned char hash[EVP_MAX_MD_SIZE];
                    unsigned int  hash_len = 0;
                    EVP_MD_CTX *sha256_ctx = EVP_MD_CTX_new();
                    EVP_DigestInit_ex(sha256_ctx, EVP_sha256(), nullptr);
                    unsigned char buf[8192];
                    size_t bytes_read;
                    while ((bytes_read = fread(buf, 1, sizeof(buf), upload_file)) > 0)
                        EVP_DigestUpdate(sha256_ctx, buf, bytes_read);
                    EVP_DigestFinal_ex(sha256_ctx, hash, &hash_len);
                    EVP_MD_CTX_free(sha256_ctx);

                    // Base64-encode the hash
                    BIO *bio = BIO_new(BIO_s_mem());
                    BIO *b64 = BIO_new(BIO_f_base64());
                    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
                    bio = BIO_push(b64, bio);
                    BIO_write(bio, hash, static_cast<int>(hash_len));
                    BIO_flush(bio);
                    BUF_MEM *bptr;
                    BIO_get_mem_ptr(bio, &bptr);
                    c.sha256_hash = std::string(bptr->data, bptr->length);
                    BIO_free_all(bio);

                    // Reset file position for upload
                    fseek(upload_file, 0, SEEK_SET);

                    // Initialize curl
                    CURL *curl = curl_easy_init();
                    if (!curl)
                    {
                        fclose(upload_file);
                        c.success    = false;
                        c.curl_error = "failed to initialize curl";
                        ::write(c.pipe_write_fd, "x", 1);
                        return;
                    }

                    curl_easy_setopt(curl, CURLOPT_URL, put_url_copy.c_str());
                    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
                    curl_easy_setopt(curl, CURLOPT_READDATA, upload_file);
                    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,
                                     static_cast<curl_off_t>(file_size));

                    struct curl_slist *headers = nullptr;
                    headers = curl_slist_append(
                        headers,
                        fmt::format("Content-Type: {}", content_type_copy).c_str());
                    for (const auto &hdr : put_headers_copy)
                        headers = curl_slist_append(headers, hdr.c_str());
                    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

                    CURLcode res = curl_easy_perform(curl);

                    long http_code = 0;
                    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

                    curl_slist_free_all(headers);
                    curl_easy_cleanup(curl);
                    fclose(upload_file);

                    c.http_code = http_code;
                    c.get_url   = get_url_copy;
                    if (res != CURLE_OK || http_code != 201)
                    {
                        c.success    = false;
                        c.curl_error = curl_easy_strerror(res);
                    }
                    else
                    {
                        c.success = true;
                    }

                    // Signal the main thread
                    ::write(c.pipe_write_fd, "x", 1);
                });
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

    // XEP-0191: server-pushed block/unblock IQ sets (§8.4, §8.5)
    if (block && type && weechat_strcasecmp(type, "set") == 0)
    {
        xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(block, "item");
        while (item)
        {
            const char *jid = xmpp_stanza_get_attribute(item, "jid");
            if (jid)
                weechat_printf(account.buffer, "%s%s was blocked",
                               weechat_prefix("network"), jid);
            item = xmpp_stanza_get_next(item);
        }
        // Acknowledge the server push
        xmpp_stanza_t *ack = xmpp_iq_new(account.context, "result", id);
        if (from) xmpp_stanza_set_to(ack, from);
        if (to)   xmpp_stanza_set_from(ack, to);
        account.connection.send(ack);
        xmpp_stanza_release(ack);
        return true;
    }

    if (unblock && type && weechat_strcasecmp(type, "set") == 0)
    {
        xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(unblock, "item");
        if (item)
        {
            while (item)
            {
                const char *jid = xmpp_stanza_get_attribute(item, "jid");
                if (jid)
                    weechat_printf(account.buffer, "%s%s was unblocked",
                                   weechat_prefix("network"), jid);
                item = xmpp_stanza_get_next(item);
            }
        }
        else
        {
            weechat_printf(account.buffer, "%sAll JIDs unblocked",
                           weechat_prefix("network"));
        }
        // Acknowledge the server push
        xmpp_stanza_t *ack = xmpp_iq_new(account.context, "result", id);
        if (from) xmpp_stanza_set_to(ack, from);
        if (to)   xmpp_stanza_set_from(ack, to);
        account.connection.send(ack);
        xmpp_stanza_release(ack);
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

        // XEP-0050: Ad-Hoc Commands — handle list and execute/form results
        const char *items_node = xmpp_stanza_get_attribute(items_query, "node");
        bool is_commands_node = items_node
            && strcmp(items_node, "http://jabber.org/protocol/commands") == 0;
        const char *iq_id = xmpp_stanza_get_id(stanza);
        bool is_adhoc_query = iq_id && account.adhoc_queries.count(iq_id);

        if (is_commands_node && is_adhoc_query)
        {
            auto &adhoc_info = account.adhoc_queries[iq_id];
            struct t_gui_buffer *adhoc_buf = adhoc_info.buffer
                ? adhoc_info.buffer : account.buffer;

            weechat_printf_date_tags(adhoc_buf, 0, "xmpp_adhoc,notify_none",
                                     "%s%sCommands available on %s%s:",
                                     weechat_prefix("network"),
                                     weechat_color("bold"),
                                     adhoc_info.target_jid.c_str(),
                                     weechat_color("reset"));

            xmpp_stanza_t *cmd_item = xmpp_stanza_get_child_by_name(items_query, "item");
            int count = 0;
            while (cmd_item)
            {
                const char *cmd_jid = xmpp_stanza_get_attribute(cmd_item, "jid");
                const char *cmd_node = xmpp_stanza_get_attribute(cmd_item, "node");
                const char *cmd_name = xmpp_stanza_get_attribute(cmd_item, "name");
                weechat_printf_date_tags(adhoc_buf, 0, "xmpp_adhoc,notify_none",
                                         "%s  %s%-40s%s  %s%s",
                                         weechat_prefix("network"),
                                         weechat_color("bold"),
                                         cmd_name ? cmd_name : "(unnamed)",
                                         weechat_color("reset"),
                                         cmd_node ? cmd_node : "",
                                         cmd_jid && cmd_jid != adhoc_info.target_jid
                                             ? (std::string(" [") + cmd_jid + "]").c_str() : "");
                count++;
                cmd_item = xmpp_stanza_get_next(cmd_item);
            }
            if (count == 0)
                weechat_printf_date_tags(adhoc_buf, 0, "xmpp_adhoc,notify_none",
                                         "%s  (no commands available)",
                                         weechat_prefix("network"));
            else
                weechat_printf_date_tags(adhoc_buf, 0, "xmpp_adhoc,notify_none",
                                         "%s  Use /adhoc %s <node> to execute a command",
                                         weechat_prefix("network"),
                                         adhoc_info.target_jid.c_str());
            account.adhoc_queries.erase(iq_id);
        }
    }

    // XEP-0050: Ad-Hoc Commands — handle command execute/form result (type=result/error)
    xmpp_stanza_t *adhoc_command = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "command", "http://jabber.org/protocol/commands");
    if (adhoc_command && type)
    {
        const char *iq_id = xmpp_stanza_get_id(stanza);
        bool is_adhoc_query = iq_id && account.adhoc_queries.count(iq_id);
        struct t_gui_buffer *adhoc_buf = is_adhoc_query
            ? (account.adhoc_queries[iq_id].buffer
               ? account.adhoc_queries[iq_id].buffer : account.buffer)
            : account.buffer;
        const char *cmd_node = xmpp_stanza_get_attribute(adhoc_command, "node");
        const char *cmd_status = xmpp_stanza_get_attribute(adhoc_command, "status");
        const char *session_id = xmpp_stanza_get_attribute(adhoc_command, "sessionid");
        const char *from_jid = xmpp_stanza_get_from(stanza);

        if (weechat_strcasecmp(type, "error") == 0)
        {
            weechat_printf_date_tags(adhoc_buf, 0, "xmpp_adhoc,notify_none",
                                     "%s[adhoc] Error executing command %s",
                                     weechat_prefix("error"),
                                     cmd_node ? cmd_node : "(unknown)");
        }
        else if (weechat_strcasecmp(type, "result") == 0)
        {
            // Check for a data form to display
            xmpp_stanza_t *x_form = xmpp_stanza_get_child_by_name_and_ns(
                adhoc_command, "x", "jabber:x:data");

            if (x_form)
            {
                const char *form_type = xmpp_stanza_get_attribute(x_form, "type");
                if (form_type && strcmp(form_type, "result") == 0)
                {
                    // Display result form (read-only)
                    render_data_form(adhoc_buf, x_form, from_jid, cmd_node, NULL);
                }
                else
                {
                    // Input form — render and prompt for submission
                    render_data_form(adhoc_buf, x_form, from_jid, cmd_node, session_id);
                }
            }
            else if (cmd_status && strcmp(cmd_status, "completed") == 0)
            {
                // Command completed with no form — check for <note>
                xmpp_stanza_t *note = xmpp_stanza_get_child_by_name(adhoc_command, "note");
                const char *note_text = note ? xmpp_stanza_get_text_ptr(note) : NULL;
                weechat_printf_date_tags(adhoc_buf, 0, "xmpp_adhoc,notify_none",
                                         "%s[adhoc] Command %s completed%s%s",
                                         weechat_prefix("network"),
                                         cmd_node ? cmd_node : "",
                                         note_text ? ": " : "",
                                         note_text ? note_text : "");
            }
            else if (cmd_status && strcmp(cmd_status, "executing") == 0 && !x_form)
            {
                weechat_printf_date_tags(adhoc_buf, 0, "xmpp_adhoc,notify_none",
                                         "%s[adhoc] Command %s in progress (no form)",
                                         weechat_prefix("network"),
                                         cmd_node ? cmd_node : "");
            }
        }

        if (is_adhoc_query)
            account.adhoc_queries.erase(iq_id);
    }

    // XEP-0433: Extended Channel Search — handle <result> or <search> IQ responses
    {
        const char *cs_id = xmpp_stanza_get_id(stanza);
        bool is_cs_query = cs_id && account.channel_search_queries.count(cs_id);

        if (is_cs_query && type)
        {
            auto &cs_info = account.channel_search_queries[cs_id];
            struct t_gui_buffer *cs_buf = cs_info.buffer ? cs_info.buffer : account.buffer;

            if (weechat_strcasecmp(type, "error") == 0)
            {
                // Try to extract a human-readable error
                xmpp_stanza_t *error_el = xmpp_stanza_get_child_by_name(stanza, "error");
                const char *err_text = NULL;
                if (error_el)
                {
                    xmpp_stanza_t *text_el = xmpp_stanza_get_child_by_name(error_el, "text");
                    if (text_el)
                        err_text = xmpp_stanza_get_text_ptr(text_el);
                }
                weechat_printf_date_tags(cs_buf, 0, "xmpp_channel_search,notify_none",
                                         "%s[search] Error from %s: %s",
                                         weechat_prefix("error"),
                                         cs_info.service_jid.c_str(),
                                         err_text ? err_text : "unknown error");
                account.channel_search_queries.erase(cs_id);
            }
            else if (weechat_strcasecmp(type, "result") == 0)
            {
                // Response wraps results in <result xmlns='urn:xmpp:channel-search:0:search'>
                xmpp_stanza_t *result_el = xmpp_stanza_get_child_by_name_and_ns(
                    stanza, "result", "urn:xmpp:channel-search:0:search");
                // Some services may reply with <search> instead of <result>
                if (!result_el)
                    result_el = xmpp_stanza_get_child_by_name_and_ns(
                        stanza, "search", "urn:xmpp:channel-search:0:search");

                if (result_el)
                {
                    int count = 0;
                    xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(result_el, "item");
                    while (item)
                    {
                        if (count == 0)
                        {
                            weechat_printf_date_tags(cs_buf, 0, "xmpp_channel_search,notify_none",
                                                     "%sMUC Rooms (via %s):",
                                                     weechat_prefix("network"),
                                                     cs_info.service_jid.c_str());
                        }

                        const char *address = xmpp_stanza_get_attribute(item, "address");
                        if (!address)
                        {
                            item = xmpp_stanza_get_next(item);
                            continue;
                        }

                        // Child elements: <name>, <nusers>, <description>, <is-open>, <language>
                        xmpp_stanza_t *name_el  = xmpp_stanza_get_child_by_name(item, "name");
                        xmpp_stanza_t *nusers_el = xmpp_stanza_get_child_by_name(item, "nusers");
                        xmpp_stanza_t *desc_el  = xmpp_stanza_get_child_by_name(item, "description");
                        xmpp_stanza_t *open_el  = xmpp_stanza_get_child_by_name(item, "is-open");

                        const char *name_raw    = name_el  ? xmpp_stanza_get_text_ptr(name_el)   : NULL;
                        const char *nusers_raw  = nusers_el ? xmpp_stanza_get_text_ptr(nusers_el) : NULL;
                        const char *desc_raw    = desc_el  ? xmpp_stanza_get_text_ptr(desc_el)   : NULL;

                        std::string display = address;
                        if (name_raw && name_raw[0])
                            display = std::string(name_raw) + " <" + address + ">";

                        std::string info_str;
                        if (nusers_raw && nusers_raw[0])
                            info_str = std::string(" (") + nusers_raw + " users)";
                        if (open_el)
                            info_str += " [open]";

                        weechat_printf_date_tags(cs_buf, 0, "xmpp_channel_search,notify_none",
                                                 "  %s%s%s%s",
                                                 weechat_color("chat_nick"),
                                                 display.c_str(),
                                                 weechat_color("reset"),
                                                 info_str.c_str());

                        // Truncate long descriptions
                        if (desc_raw && desc_raw[0])
                        {
                            std::string desc(desc_raw);
                            if (desc.length() > 120)
                                desc = desc.substr(0, 117) + "...";
                            weechat_printf_date_tags(cs_buf, 0, "xmpp_channel_search,notify_none",
                                                     "    %s", desc.c_str());
                        }

                        count++;
                        item = xmpp_stanza_get_next(item);
                    }

                    if (count == 0)
                    {
                        weechat_printf_date_tags(cs_buf, 0, "xmpp_channel_search,notify_none",
                                                 "%sNo rooms found matching your query",
                                                 weechat_prefix("network"));
                    }
                    else
                    {
                        weechat_printf_date_tags(cs_buf, 0, "xmpp_channel_search,notify_none",
                                                 "%sUse /enter <address> to join a room",
                                                 weechat_prefix("network"));
                    }
                }
                else
                {
                    weechat_printf_date_tags(cs_buf, 0, "xmpp_channel_search,notify_none",
                                             "%s[search] Unexpected response from %s (missing <result>)",
                                             weechat_prefix("error"),
                                             cs_info.service_jid.c_str());
                }

                account.channel_search_queries.erase(cs_id);
            }
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
                else if (category == "client")
                {
                    xmpp_stanza_t *children[2] = {NULL};
                    children[0] = stanza__iq_pubsub_items(account.context, NULL,
                            "eu.siacs.conversations.axolotl.devicelist");
                    children[0] = stanza__iq_pubsub(account.context, NULL,
                            children, with_noop("http://jabber.org/protocol/pubsub"));
                    children[0] = stanza__iq(account.context, NULL, children, NULL,
                            "fetch2", to, binding.from ? binding.from->bare.data() : NULL, "get");
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

    // RFC 6121 §2.1.6 — roster push: server sends IQ type="set" with a single item
    if (query && type && weechat_strcasecmp(type, "set") == 0)
    {
        xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(query, "item");
        if (item)
        {
            const char *jid          = xmpp_stanza_get_attribute(item, "jid");
            const char *roster_name  = xmpp_stanza_get_attribute(item, "name");
            const char *subscription = xmpp_stanza_get_attribute(item, "subscription");

            if (jid)
            {
                if (subscription && weechat_strcasecmp(subscription, "remove") == 0)
                {
                    account.roster.erase(jid);
                    weechat_printf(account.buffer, "%sRoster: %s removed",
                                   weechat_prefix("network"), jid);
                }
                else
                {
                    bool is_new = (account.roster.find(jid) == account.roster.end());
                    account.roster[jid].jid = jid;
                    account.roster[jid].name = roster_name ? roster_name : "";
                    account.roster[jid].subscription = subscription ? subscription : "none";
                    account.roster[jid].groups.clear();

                    xmpp_stanza_t *group;
                    for (group = xmpp_stanza_get_children(item);
                         group; group = xmpp_stanza_get_next(group))
                    {
                        const char *gname = xmpp_stanza_get_name(group);
                        if (weechat_strcasecmp(gname, "group") != 0) continue;
                        xmpp_stanza_t *gtxt = xmpp_stanza_get_children(group);
                        if (gtxt) {
                            char *text = xmpp_stanza_get_text(gtxt);
                            if (text) {
                                account.roster[jid].groups.push_back(text);
                                xmpp_free(account.context, text);
                            }
                        }
                    }

                    if (is_new)
                        weechat_printf(account.buffer, "%sRoster: %s added (%s)",
                                       weechat_prefix("network"), jid,
                                       subscription ? subscription : "none");
                    else
                        weechat_printf(account.buffer, "%sRoster: %s updated (subscription: %s)",
                                       weechat_prefix("network"), jid,
                                       subscription ? subscription : "none");
                }
            }
        }
        // Acknowledge the roster push
        xmpp_stanza_t *ack = xmpp_iq_new(account.context, "result", id);
        if (from) xmpp_stanza_set_to(ack, from);
        if (to)   xmpp_stanza_set_from(ack, to);
        account.connection.send(ack);
        xmpp_stanza_release(ack);
        return true;
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
                                                    bundle_node);
                            children[0] =
                            stanza__iq_pubsub(account.context, NULL, children,
                                                with_noop("http://jabber.org/protocol/pubsub"));
                            char *uuid = xmpp_uuid_gen(account.context);
                            children[0] =
                            stanza__iq(account.context, NULL, children, NULL, uuid,
                                to, from, "get");
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

                            weechat_printf(account.buffer, 
                                           "%sServer devicelist for %s:",
                                           weechat_prefix("network"), from);
                            weechat_printf(account.buffer,
                                           "%s  Device %u (weechat - this device)",
                                           weechat_prefix("network"), dev.id);

                            int device_count = 1;
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
                                
                                if (dev.id != account.omemo.device_id)
                                {
                                    weechat_printf(account.buffer,
                                                   "%s  Device %u",
                                                   weechat_prefix("network"), dev.id);
                                    device_count++;
                                }
                            }

                            weechat_printf(account.buffer,
                                           "%sTotal devices in devicelist: %d",
                                           weechat_prefix("network"), device_count);

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
                            uint32_t bundle_device_id = strtol(items_node+node_prefix, NULL, 10);
                            
                            // If this is our own device's bundle, print confirmation
                            if (from && account.jid() == from && 
                                bundle_device_id == account.omemo.device_id)
                            {
                                weechat_printf(account.buffer,
                                               "%sBundle found for device %u:",
                                               weechat_prefix("network"), bundle_device_id);
                                
                                // Count prekeys
                                xmpp_stanza_t *prekeys = xmpp_stanza_get_child_by_name(bundle, "prekeys");
                                int prekey_count = 0;
                                if (prekeys)
                                {
                                    for (xmpp_stanza_t *pk = xmpp_stanza_get_children(prekeys);
                                         pk; pk = xmpp_stanza_get_next(pk))
                                    {
                                        const char *name = xmpp_stanza_get_name(pk);
                                        if (weechat_strcasecmp(name, "preKeyPublic") == 0)
                                            prekey_count++;
                                    }
                                }
                                
                                weechat_printf(account.buffer,
                                               "%s  %d prekeys available",
                                               weechat_prefix("network"), prekey_count);
                                weechat_printf(account.buffer,
                                               "%s  ✓ Bundle is published and available for contacts",
                                               weechat_prefix("network"));
                            }
                            
                            account.omemo.handle_bundle(
                                from ? from : account.jid().data(),
                                bundle_device_id,
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
        if (account.mam_query_search(&mam_query, id))
        {
            // Check if this is a global MAM query (empty 'with')
            bool is_global_query = mam_query.with.empty();
            
            auto channel = account.channels.find(mam_query.with.data());

            set__last = set ? xmpp_stanza_get_child_by_name(set, "last") : nullptr;
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
                    account.mam_query_remove(mam_query.id);
                }
            }
            else if (is_global_query)
            {
                if (set__last__text)
                {
                    // More pages — issue next page of global MAM query with <after> token
                    account.mam_query_remove(mam_query.id);

                    char *next_id = xmpp_uuid_gen(account.context);
                    account.add_mam_query(next_id, "",
                                          mam_query.start, mam_query.end);

                    xmpp_stanza_t *next_iq = xmpp_iq_new(account.context, "set", next_id);

                    xmpp_stanza_t *next_query = xmpp_stanza_new(account.context);
                    xmpp_stanza_set_name(next_query, "query");
                    xmpp_stanza_set_ns(next_query, "urn:xmpp:mam:2");

                    // Data form
                    xmpp_stanza_t *nx = xmpp_stanza_new(account.context);
                    xmpp_stanza_set_name(nx, "x");
                    xmpp_stanza_set_ns(nx, "jabber:x:data");
                    xmpp_stanza_set_attribute(nx, "type", "submit");

                    // FORM_TYPE field
                    {
                        xmpp_stanza_t *f = xmpp_stanza_new(account.context);
                        xmpp_stanza_set_name(f, "field");
                        xmpp_stanza_set_attribute(f, "var", "FORM_TYPE");
                        xmpp_stanza_set_attribute(f, "type", "hidden");
                        xmpp_stanza_t *v = xmpp_stanza_new(account.context);
                        xmpp_stanza_set_name(v, "value");
                        xmpp_stanza_t *t = xmpp_stanza_new(account.context);
                        xmpp_stanza_set_text(t, "urn:xmpp:mam:2");
                        xmpp_stanza_add_child(v, t); xmpp_stanza_release(t);
                        xmpp_stanza_add_child(f, v); xmpp_stanza_release(v);
                        xmpp_stanza_add_child(nx, f); xmpp_stanza_release(f);
                    }

                    // Start time field (same window as original query)
                    if (mam_query.start)
                    {
                        xmpp_stanza_t *f = xmpp_stanza_new(account.context);
                        xmpp_stanza_set_name(f, "field");
                        xmpp_stanza_set_attribute(f, "var", "start");
                        xmpp_stanza_t *v = xmpp_stanza_new(account.context);
                        xmpp_stanza_set_name(v, "value");
                        xmpp_stanza_t *t = xmpp_stanza_new(account.context);
                        char tbuf[256];
                        time_t tval = *mam_query.start;
                        strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%SZ", gmtime(&tval));
                        xmpp_stanza_set_text(t, tbuf);
                        xmpp_stanza_add_child(v, t); xmpp_stanza_release(t);
                        xmpp_stanza_add_child(f, v); xmpp_stanza_release(v);
                        xmpp_stanza_add_child(nx, f); xmpp_stanza_release(f);
                    }

                    xmpp_stanza_add_child(next_query, nx);
                    xmpp_stanza_release(nx);

                    // RSM <after> element for paging
                    {
                        xmpp_stanza_t *rset = xmpp_stanza_new(account.context);
                        xmpp_stanza_set_name(rset, "set");
                        xmpp_stanza_set_ns(rset, "http://jabber.org/protocol/rsm");
                        xmpp_stanza_t *after_el = xmpp_stanza_new(account.context);
                        xmpp_stanza_set_name(after_el, "after");
                        xmpp_stanza_t *after_t = xmpp_stanza_new(account.context);
                        xmpp_stanza_set_text(after_t, set__last__text);
                        xmpp_stanza_add_child(after_el, after_t); xmpp_stanza_release(after_t);
                        xmpp_stanza_add_child(rset, after_el); xmpp_stanza_release(after_el);
                        xmpp_stanza_add_child(next_query, rset); xmpp_stanza_release(rset);
                    }

                    xmpp_stanza_add_child(next_iq, next_query);
                    xmpp_stanza_release(next_query);

                    this->send(next_iq);
                    xmpp_stanza_release(next_iq);
                    xmpp_free(account.context, next_id);
                }
                else
                {
                    // Global MAM query complete
                    account.mam_query_remove(mam_query.id);
                }
            }
            else
            {
                if (!set__last__text)
                    account.mam_query_remove(mam_query.id);
            }
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
        account.sm_outqueue.clear();

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
        uint32_t ack_h = 0;
        if (h)
        {
            ack_h = std::stoul(h);
            account.sm_last_ack = ack_h;
            weechat_printf(account.buffer, "%sStream resumed (h=%u)",
                          weechat_prefix("network"), ack_h);
        }
        else
        {
            weechat_printf(account.buffer, "%sStream resumed",
                          weechat_prefix("network"));
        }

        // Prune stanzas the server already acknowledged
        while (!account.sm_outqueue.empty() &&
               account.sm_outqueue.front().first <= ack_h)
        {
            account.sm_outqueue.pop_front();
        }

        // Retransmit all remaining unacknowledged stanzas
        if (!account.sm_outqueue.empty())
        {
            weechat_printf(account.buffer, "%sRetransmitting %zu unacknowledged stanza(s)…",
                          weechat_prefix("network"), account.sm_outqueue.size());
            for (auto& [seq, stanza_copy] : account.sm_outqueue)
            {
                m_conn.send(stanza_copy.get());
            }
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
        account.sm_outqueue.clear();
        
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

            // Prune all stanzas the server has confirmed receiving
            while (!account.sm_outqueue.empty() &&
                   account.sm_outqueue.front().first <= ack_count)
            {
                account.sm_outqueue.pop_front();
            }
            
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
        /* children layout: [0]=<c/> [1]=<status/> [2]=<x vcard-temp:x:update/> [3]=<x pgp/> [4]=NULL */
        auto children = std::unique_ptr<xmpp_stanza_t*[]>(new xmpp_stanza_t*[4 + 1]);

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

        /* XEP-0153: vCard-Based Avatars — broadcast own photo hash in presence */
        {
            xmpp_stanza_t *vcard_x = xmpp_stanza_new(account.context);
            xmpp_stanza_set_name(vcard_x, "x");
            xmpp_stanza_set_ns(vcard_x, "vcard-temp:x:update");

            xmpp_stanza_t *photo_elem = xmpp_stanza_new(account.context);
            xmpp_stanza_set_name(photo_elem, "photo");

            weechat::user *self_user = weechat::user::search(&account, account.jid().data());
            if (self_user && !self_user->profile.avatar_hash.empty())
            {
                xmpp_stanza_t *photo_text = xmpp_stanza_new(account.context);
                xmpp_stanza_set_text(photo_text, self_user->profile.avatar_hash.data());
                xmpp_stanza_add_child(photo_elem, photo_text);
                xmpp_stanza_release(photo_text);
            }

            xmpp_stanza_add_child(vcard_x, photo_elem);
            xmpp_stanza_release(photo_elem);

            children[2] = vcard_x;
        }

        children[3] = NULL;

        if (!account.pgp_keyid().empty())
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

            children[3] = pres__x;
            children[4] = NULL;
        }

        {
            xmpp_stanza_t *pres = stanza__presence(account.context, nullptr, children.get(),
                                                    nullptr, account.jid().data(), nullptr, nullptr);
            this->send(pres);
            xmpp_stanza_release(pres);
        }

        // XEP-0172: publish own nickname via PEP so contacts see a display name
        if (!account.nickname().empty())
        {
            xmpp_stanza_t *nick_iq = ::xmpp::xep0172::publish_nick(
                account.context, std::string(account.nickname()).c_str());
            xmpp_stanza_set_from(nick_iq, account.jid().data());
            this->send(nick_iq);
            xmpp_stanza_release(nick_iq);
        }

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
                                "eu.siacs.conversations.axolotl.devicelist");
        children[0] =
        stanza__iq_pubsub(account.context, NULL, children.get(),
                          with_noop("http://jabber.org/protocol/pubsub"));
        char *uuid = xmpp_uuid_gen(account.context);
        children[0] =
        stanza__iq(account.context, NULL, children.get(), NULL, uuid,
                   account.jid().data(), account.jid().data(),
                   "get");
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

std::string rand_string(int length)
{
    std::string s(length, '\0');
    for(int i = 0; i < length; ++i){
        s[i] = '0' + rand()%72; // starting on '0', ending on '}'
        if (!((s[i] >= '0' && s[i] <= '9') ||
              (s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= 'a' && s[i] <= 'z')))
            i--; // reroll
    }
    return s;
}

int weechat::connection::connect(std::string jid, std::string password, weechat::tls_policy tls)
{
    static const unsigned ka_timeout_sec = 60;
    static const unsigned ka_timeout_ivl = 1;

    m_conn.set_keepalive(ka_timeout_sec, ka_timeout_ivl);

    const char *resource = account.resource().data();
    if (!(resource && strlen(resource)))
    {
        const std::string rand = rand_string(8);
        char ident[64] = {0};
        snprintf(ident, sizeof(ident), "weechat.%s", rand.c_str());

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
