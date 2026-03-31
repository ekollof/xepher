bool weechat::connection::presence_handler(xmpp_stanza_t *stanza, bool top_level)
{
    // SM counter incremented in libstrophe wrapper, not here
    // top_level parameter kept for nested/recursive calls

    (void) top_level;
    append_raw_xml_trace(account, "RECV", stanza);

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
            // Not cached, send disco query and mark for caching.
            // Only query full JIDs (with a non-empty resource) — caps are
            // per-client.  A bare JID or a trailing-slash JID (empty resource)
            // is invalid as a disco target and causes the server to return an
            // error that disconnects us.
            bool has_resource = binding.from && !binding.from->resource.empty();
            if (has_resource)
            {
                xmpp_string_guard disco_id_g(account.context, xmpp_uuid_gen(account.context));
                const char *disco_id = disco_id_g.ptr;
                account.caps_disco_queries[disco_id] = ver;  // Track this query for caching

                account.connection.send(stanza::iq()
                            .from(binding.to ? binding.to->full : "")
                            .to(binding.from->full)
                            .type("get")
                            .id(disco_id)
                            .xep0030()
                            .query()
                            .build(account.context)
                            .get());

                // freed by disco_id_g
            }
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
            if (std::string_view(error_reason) != "Unspecified" || error->description)
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
                    {
                        channel->joining = false;

                        // MAM catch-up for MUC: fetch messages missed since last
                        // disconnect.  Mirror the same pattern used for PM channels
                        // in channel.cpp so we recover missed messages on reconnect.
                        if (channel->type == weechat::channel::chat_type::MUC)
                        {
                            time_t now = time(NULL);
                            time_t start;

                            // Load persisted last-fetch timestamp from LMDB.
                            if (channel->last_mam_fetch == 0)
                                channel->last_mam_fetch =
                                    account.mam_cache_get_last_timestamp(channel->id);

                            // -1 means "channel was closed cleanly" — treat as fresh.
                            if (channel->last_mam_fetch == static_cast<time_t>(-1))
                            {
                                channel->last_mam_fetch = 0;
                                account.mam_cache_set_last_timestamp(channel->id, 0);
                            }

                            // Replay any LMDB-cached messages into the buffer first.
                            if (channel->last_mam_fetch > 0)
                                account.mam_cache_load_messages(channel->id, channel->buffer);

                            // If we fetched recently, only request new messages.
                            if (channel->last_mam_fetch > 0 &&
                                (now - channel->last_mam_fetch) < 300)
                                start = channel->last_mam_fetch;
                            else
                                start = now - (7 * 86400); // fallback: last 7 days

                            time_t end = now;
                            xmpp_string_guard mam_uuid_g(account.context,
                                xmpp_uuid_gen(account.context));
                            if (mam_uuid_g.ptr)
                                channel->fetch_mam(mam_uuid_g.ptr, &start, &end, nullptr);
                        }
                    }
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
                                                                    channel && weechat_strcasecmp(binding.from->bare.data(), channel->id.data()) == 0
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
                if (auto signature = binding.signature();
                        signature && channel->type != weechat::channel::chat_type::MUC)
                {
                    user->profile.pgp_id = account.pgp.verify(channel->buffer, signature->data());
                    if (user->profile.pgp_id.has_value())
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
            xmpp_string_guard owner_id_g(account.context, xmpp_uuid_gen(account.context));
            const char *owner_id = owner_id_g.ptr;
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
            // freed by owner_id_g
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
                                                                channel && weechat_strcasecmp(binding.from->bare.data(), channel->id.data()) == 0
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
            if (auto signature = binding.signature();
                    signature && channel->type != weechat::channel::chat_type::MUC)
            {
                user->profile.pgp_id = account.pgp.verify(channel->buffer, signature->data());
                if (user->profile.pgp_id.has_value())
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
                if (photo_hash && !std::string_view(photo_hash).empty())
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

// XEP-0071: XHTML-IM rendering utilities — now in src/xmpp/xhtml.hh/.cpp.
#include "xmpp/xhtml.hh"

// css_color_to_weechat, css_style_to_weechat, and xhtml_to_weechat are now
// defined in src/xmpp/xhtml.cpp and declared in src/xmpp/xhtml.hh above.

