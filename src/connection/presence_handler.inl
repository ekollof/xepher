bool weechat::connection::presence_handler(xmpp_stanza_t *stanza, bool top_level)
{
    // SM counter incremented in libstrophe wrapper, not here
    // top_level parameter kept for nested/recursive calls

    (void) top_level;
    append_raw_xml_trace(account, "RECV", stanza);

    weechat::user *user;
    weechat::channel *channel;

    auto binding = std::make_unique<xml::presence>(account.context, stanza);
    if (!binding->from)
        return 1;

    std::string clientid;
    if (auto caps = binding->capabilities())
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
            bool has_resource = binding->from && !binding->from->resource.empty();
            if (has_resource)
            {
                std::string disco_id = stanza::uuid(account.context);
                account.caps_disco_queries[disco_id] = ver;  // Track this query for caching

                account.connection.send(stanza::iq()
                            .from(binding->to ? binding->to->full : "")
                            .to(binding->from->full)
                            .type("get")
                            .id(disco_id)
                            .xep0030()
                            .query()
                            .build(account.context)
                            .get());
            }
        }
    }

    {
        channel = nullptr;
        if (auto ch_it = account.channels.find(binding->from->bare.data()); ch_it != account.channels.end())
        {
            auto& [_, ch] = *ch_it;
            channel = &ch;
        }
    }
    
    // Note: We don't auto-create PM channels from presence anymore.
    // PM channels are only created when:
    // - User explicitly opens with /query
    // - We receive a message from them
    // This allows roster contacts to appear in account nicklist instead of creating buffers

    if (binding->type && *binding->type == "error" && channel)
    {
        if (auto error = binding->error())
        {
            const char *error_reason = error->reason();
            const char *from_str = binding->from ? binding->from->full.data() : "unknown";
            
            // Only log if the error has meaningful information
            // Skip generic "Unspecified" errors that don't have useful context
            if (std::string_view(error_reason) != "Unspecified" || error->description)
            {
                std::string msg = fmt::format("[!]\t{}{}{}{}{}{}",
                                              weechat::xmpp_color("gray").c_str(),
                                              binding->muc() ? "MUC " : "",
                                              error_reason,
                                              error->description ? " (" : "",
                                              error->description ? error->description->data() : "",
                                              error->description ? ")" : "");
                weechat_printf(channel->buffer, "%s", msg.c_str());
            }
            else
            {
                // Debug: log unspecified errors with JID
                std::string msg = fmt::format("{}[DEBUG] Received unspecified error from {} (presence)",
                                              weechat_prefix("network"), from_str);
                weechat_printf(account.buffer, "%s", msg.c_str());
            }
        }
        return 1;
    }

    if (auto& x_opt = binding->muc_user(); x_opt.has_value())
    {
        auto& x = *x_opt;
        bool is_nick_change = false;
        bool is_new_room = false;
        bool is_server_nick = false;
        for (int& status : x.statuses)
        {
            switch (status)
            {
                case 100: // Non-Anonymous: [message | Entering a room]: Inform user that any occupant is allowed to see the user's full JID
                    break;
                case 101: // Affiliation change: room visibility changed, JID now visible to all
                    if (channel)
                    {
                        std::string msg = fmt::format("{}{}[Room] Your affiliation changed; your full JID is now visible to all occupants",
                                                      weechat_prefix("network"), weechat::xmpp_color("gray").c_str());
                        weechat_printf_date_tags(channel->buffer, 0, "xmpp_presence,notify_none,no_trigger", "%s", msg.c_str());
                    }
                    break;
                case 102: // : [message | Configuration change]: Inform occupants that room now shows unavailable members
                    break;
                case 103: // : [message | Configuration change]: Inform occupants that room now does not show unavailable members
                    break;
                case 104: // : [message | Configuration change]: Inform occupants that a non-privacy-related room configuration change has occurred
                    break;
                case 110: // Self-Presence: [presence | Any room presence]: Inform user that presence refers to one of its own room occupants
                    // Status 110 is sent last in the initial presence flood — clear joining flag.
                    // Status 110 is also delivered when any other resource joins the same room
                    // (the server re-sends the full occupant list to all members).  Guard against
                    // spurious re-triggering by only running the MAM catch-up when we were
                    // actually in the joining state (joining == true means this is the first
                    // status-110 for this session).  If joining is already false we are already
                    // in the room and this is just a roster update — do nothing.
                    if (channel)
                    {
                        const bool was_joining = channel->joining;
                        channel->joining = false;

                        // MAM catch-up for MUC: fetch messages missed since last
                        // disconnect.  Mirror the same pattern used for PM channels
                        // in channel.cpp so we recover missed messages on reconnect.
                        if (was_joining && channel->type == weechat::channel::chat_type::MUC)
                        {
                            time_t now = time(nullptr);
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
                            // Exception: on the very first join of this room in the current
                            // WeeChat session (was_joining), always do the full configured
                            // history fetch. The 300s throttle is only for subsequent
                            // presence updates while already joined.
                            if (channel->last_mam_fetch > 0 &&
                                (now - channel->last_mam_fetch) < 300 &&
                                !was_joining)
                                start = channel->last_mam_fetch;
                            else {
                                time_t fetch_days = weechat::config::instance
                                    ? static_cast<time_t>(weechat::config::instance->look.mam_fetch_days.integer())
                                    : 3;
                                start = now - (fetch_days * 86400);
                            }

                            time_t end = now;
                            std::string mam_uuid = stanza::uuid(account.context);
                            if (!mam_uuid.empty())
                                channel->fetch_mam(mam_uuid.c_str(), &start, &end, nullptr);
                        }

                        // docs/planning-muc-omemo.md §2.2: After initial self-presence (110)
                        // on MUC join, kick off member list discovery so we can obtain
                        // real JIDs for all occupants (required for MUC OMEMO).
                        // We use disco#items here; results are handled in iq_handler.
                        if (was_joining && channel->type == weechat::channel::chat_type::MUC)
                        {
                            std::string disco_id = stanza::uuid(account.context);
                            account.connection.send(
                                stanza::iq()
                                    .type("get")
                                    .to(channel->id)
                                    .id(disco_id)
                                    .xep0030()
                                    .query_items()
                                    .build(account.context)
                                    .get()
                            );

                            // docs/planning-muc-omemo.md §2.2: Send XEP-0045 muc#admin affiliation
                            // queries (member/admin/owner) as fallback to obtain real JIDs for
                            // more occupants. Results are handled by the muc#admin handler in
                            // iq_handler (which feeds them through add_member + centralized
                            // devicelist request).
                            {
                                for (const char *aff : {"member", "admin", "owner"}) {
                                    std::string admin_id = stanza::uuid(account.context);
                                    stanza::xep0045admin::query q;
                                    stanza::xep0045admin::item_by_affiliation it(aff);
                                    q.item(it);

                                    auto adm_iq = stanza::iq()
                                        .type("get")
                                        .to(channel->id)
                                        .id(admin_id);
                                    adm_iq.muc_admin(q);
                                    account.connection.send(adm_iq.build(account.context).get());
                                }
                            }

                            // docs/planning-muc-omemo.md §2.3: For occupants whose real JID we already
                            // know from presence, start fetching their OMEMO devicelist now.
                            // This is done in parallel with the full disco#items for completeness.
                            for (const auto& [occ_id, member] : channel->members)
                            {
                                if (member.real_jid && !member.real_jid->empty())
                                {
                                    account.omemo.request_axolotl_devicelist(account, *member.real_jid);
                                }
                            }
                        }
                    }
                    break;
                case 170: // Logging Active: room logging enabled — privacy notice
                    if (channel)
                    {
                        std::string msg = fmt::format("{}{}[Room] Warning: room logging is now {}enabled{} — messages are being stored",
                                                      weechat_prefix("network"), weechat::xmpp_color("yellow").c_str(),
                                                      weechat::xmpp_color("yellow,bold").c_str(), weechat::xmpp_color("reset").c_str());
                        weechat_printf_date_tags(channel->buffer, 0, "xmpp_presence,notify_none,no_trigger", "%s", msg.c_str());
                    }
                    break;
                case 171: // Logging disabled
                    if (channel)
                    {
                        std::string msg = fmt::format("{}{}[Room] Room logging is now disabled",
                                                      weechat_prefix("network"), weechat::xmpp_color("gray").c_str());
                        weechat_printf_date_tags(channel->buffer, 0, "xmpp_presence,notify_none,no_trigger", "%s", msg.c_str());
                    }
                    break;
                case 172: // Room is now non-anonymous (full JIDs visible)
                    if (channel)
                    {
                        std::string msg = fmt::format("{}{}[Room] Room is now {}non-anonymous{} — full JIDs are visible to all occupants",
                                                      weechat_prefix("network"), weechat::xmpp_color("yellow").c_str(),
                                                      weechat::xmpp_color("yellow,bold").c_str(), weechat::xmpp_color("reset").c_str());
                        weechat_printf_date_tags(channel->buffer, 0, "xmpp_presence,notify_none,no_trigger", "%s", msg.c_str());
                    }
                    break;
                case 173: // Room is now semi-anonymous
                    if (channel)
                    {
                        std::string msg = fmt::format("{}{}[Room] Room is now semi-anonymous — full JIDs visible to moderators only",
                                                      weechat_prefix("network"), weechat::xmpp_color("gray").c_str());
                        weechat_printf_date_tags(channel->buffer, 0, "xmpp_presence,notify_none,no_trigger", "%s", msg.c_str());
                    }
                    break;
                case 174: // Room is now fully-anonymous
                    if (channel)
                    {
                        std::string msg = fmt::format("{}{}[Room] Room is now fully-anonymous — JIDs are hidden from all occupants",
                                                      weechat_prefix("network"), weechat::xmpp_color("gray").c_str());
                        weechat_printf_date_tags(channel->buffer, 0, "xmpp_presence,notify_none,no_trigger", "%s", msg.c_str());
                    }
                    break;
                case 201: // New room created — must accept default config to unlock it
                    is_new_room = true;
                    if (channel)
                    {
                        std::string msg = fmt::format("{}{}[Room] New room created; accepting default configuration",
                                                      weechat_prefix("network"), weechat::xmpp_color("gray").c_str());
                        weechat_printf_date_tags(channel->buffer, 0, "xmpp_presence,notify_none,no_trigger", "%s", msg.c_str());
                    }
                    break;
                case 210: // Server assigned/modified nick
                    is_server_nick = true;
                    break;
                case 301: // : [presence | Removal from room]: Inform user that he or she has been banned from the room
                    if (channel)
                    {
                        std::string msg = fmt::format("[!]\t{}Banned from Room", weechat::xmpp_color("gray").c_str());
                        weechat_printf_date_tags(channel->buffer, 0, "xmpp_presence,notify_none,no_trigger", "%s", msg.c_str());
                    }
                    break;
                case 303: // Nick change — this unavailable presence means occupant is changing nick
                    is_nick_change = true;
                    break;
                case 307: // : [presence | Removal from room]: Inform user that he or she has been kicked from the room
                    if (channel)
                    {
                        std::string msg = fmt::format("[!]\t{}Kicked from room", weechat::xmpp_color("gray").c_str());
                        weechat_printf_date_tags(channel->buffer, 0, "xmpp_presence,notify_none,no_trigger", "%s", msg.c_str());
                    }
                    break;
                case 321: // : [presence | Removal from room]: Inform user that he or she is being removed from the room because of an affiliation change
                    if (channel)
                    {
                        std::string msg = fmt::format("[!]\t{}Room Affiliation changed, kicked", weechat::xmpp_color("gray").c_str());
                        weechat_printf_date_tags(channel->buffer, 0, "xmpp_presence,notify_none,no_trigger", "%s", msg.c_str());
                    }
                    break;
                case 322: // : [presence | Removal from room]: Inform user that he or she is being removed from the room because the room has been changed to members-only and the user is not a member
                    if (channel)
                    {
                        std::string msg = fmt::format("[!]\t{}Room now members-only, kicked", weechat::xmpp_color("gray").c_str());
                        weechat_printf_date_tags(channel->buffer, 0, "xmpp_presence,notify_none,no_trigger", "%s", msg.c_str());
                    }
                    break;
                case 332: // : [presence | Removal from room]: Inform user that he or she is being removed from the room because of a system shutdown
                    if (channel)
                    {
                        std::string msg = fmt::format("[!]\t{}Room Shutdown", weechat::xmpp_color("gray").c_str());
                        weechat_printf_date_tags(channel->buffer, 0, "xmpp_presence,notify_none,no_trigger", "%s", msg.c_str());
                    }
                    break;
                default:
                    break;
            }
        }

        for (auto& item_ptr : x.items)
        {
            auto& item = *item_ptr;
            using xml::xep0045;

            std::string role(item.role ? xep0045::format_role(*item.role) : "");
            std::string affiliation(item.affiliation ? xep0045::format_affiliation(*item.affiliation) : "");
            std::string jid = item.target ? item.target->full : clientid;

            user = weechat::user::search(&account, binding->from->full.data());
            if (!user)
            {
                auto name = binding->from->full.data();
                auto [it_u, _ins_u] = account.users.emplace(std::piecewise_construct,
                                              std::forward_as_tuple(name),
                                              std::forward_as_tuple(&account, channel, name,
                                                                    channel && weechat_strcasecmp(binding->from->bare.data(), channel->id.data()) == 0
                                                                    ? (binding->from->resource.size() ? binding->from->resource.data() : "")
                                                                    : binding->from->full.data()));
                auto& [_, u] = *it_u;
                user = &u;
            }
            auto status = binding->status();
            auto show = binding->show();
            auto idle = binding->idle_since();
            user->profile.status_text = status ? std::optional<std::string>(status->data()) : std::nullopt;
            user->profile.status = show ? std::optional<std::string>(show->data()) : std::nullopt;
            user->profile.idle = idle ? fmt::format("{}", *idle) : std::string();
            user->is_away = show ? *show == "away" : false;
            user->profile.role = role.size() ? std::optional<std::string>(role.data()) : std::nullopt;
            user->profile.affiliation = (affiliation.size() && affiliation != "none")
                ? std::optional<std::string>(affiliation.data()) : std::nullopt;
            if (channel)
            {
                if (auto signature = binding->signature();
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
                        const char *old_nick = binding->from->resource.size()
                            ? binding->from->resource.data() : binding->from->full.data();
                        // Smart filter: suppress nick-change for users who haven't spoken recently
                        const char *nick_tags = channel->smart_filter_nick(old_nick)
                            ? "xmpp_presence,nick,log4,xmpp_smart_filter,no_trigger"
                            : "xmpp_presence,nick,log4,no_trigger";
                        std::string msg = fmt::format("{}{}{}{} is now known as {}{}",
                                                      weechat_prefix("network"),
                                                      weechat::xmpp_color("irc.color.nick_change").c_str(),
                                                      old_nick,
                                                      weechat::xmpp_color("reset").c_str(),
                                                      weechat::xmpp_color("irc.color.nick_change").c_str(),
                                                      item.nick->data());
                        weechat_printf_date_tags(channel->buffer, 0, nick_tags, "%s", msg.c_str());
                        // Still remove from nicklist so the new presence can re-add
                        weechat::user *leaving = weechat::user::search(&account, binding->from->full.data());
                        if (leaving)
                            leaving->nicklist_remove(&account, channel);
                    }
                    else
                        channel->remove_member(binding->from->full.data(), status ? status->data() : nullptr);
                }
                else
                {
                    channel->add_member(binding->from->full.data(), jid.data(),
                                        item.target ? std::optional(std::string_view(item.target->full))
                                                    : std::nullopt,
                                        user);

                    // docs/planning-muc-omemo.md §2.3: Whenever we learn (or re-learn) a
                    // real JID for a MUC occupant via presence, proactively request their
                    // OMEMO devicelist so we can fetch bundles and build sessions early.
                    if (item.target && channel &&
                        channel->type == weechat::channel::chat_type::MUC &&
                        account.omemo)
                    {
                        account.omemo.request_axolotl_devicelist(account, item.target->full);
                    }

                    if (is_server_nick && channel)
                    {
                        // Status 210: server assigned a different nick than requested
                        const char *assigned = binding->from->resource.size()
                            ? binding->from->resource.data() : binding->from->full.data();
                        std::string msg = fmt::format("{}{}[Room] Server assigned you the nick: {}{}{}",
                                                      weechat_prefix("network"), weechat::xmpp_color("gray").c_str(),
                                                      weechat::xmpp_color("irc.color.nick_change").c_str(),
                                                      assigned,
                                                      weechat::xmpp_color("reset").c_str());
                        weechat_printf_date_tags(channel->buffer, 0, "xmpp_presence,notify_none,no_trigger", "%s", msg.c_str());
                    }
                }
            }
        }

        if (is_new_room && channel)
        {
            // Status 201: new room was created — send empty config submit to unlock it
            std::string room_jid = jid(nullptr, binding->from->full).bare;

            struct x_data_submit : stanza::spec {
                x_data_submit() : spec("x") {
                    xmlns<jabber::x::data>();
                    attr("type", "submit");
                }
            };

            struct muc_owner_query : stanza::spec {
                muc_owner_query(x_data_submit &x) : spec("query") {
                    xmlns<jabber_org::protocol::muc::owner>();
                    child(x);
                }
            };

            struct room_unlock_iq : stanza::spec {
                room_unlock_iq(std::string_view to_, std::string_view id_,
                               muc_owner_query &q) : spec("iq") {
                    attr("type", "set");
                    attr("to", to_);
                    attr("id", id_);
                    child(q);
                }
            };

            x_data_submit xd;
            muc_owner_query owner_q(xd);
            std::string unlock_id = stanza::uuid(account.context);
            room_unlock_iq unlock_iq(room_jid, unlock_id, owner_q);
            account.connection.send(unlock_iq.build(account.context).get());
        }
    }
    else
    {
        user = user::search(&account, binding->from->full.data());
        if (!user)
        {
            auto name = binding->from->full.data();
            auto [it_u, _ins_u] = account.users.emplace(std::piecewise_construct,
                                          std::forward_as_tuple(name),
                                          std::forward_as_tuple(&account, channel, name,
                                                                channel && weechat_strcasecmp(binding->from->bare.data(), channel->id.data()) == 0
                                                                ? (binding->from->resource.size() ? binding->from->resource.data() : "")
                                                                : binding->from->full.data()));
            auto& [_, u] = *it_u;
            user = &u;
        }
        auto status = binding->status();
        auto show = binding->show();
        auto idle = binding->idle_since();
        user->profile.status_text = status ? std::optional<std::string>(status->data()) : std::nullopt;
        user->profile.status = show ? std::optional<std::string>(show->data()) : std::nullopt;
        user->profile.idle = idle ? fmt::format("{}", *idle) : std::string();
        user->is_away = show ? *show == "away" : false;
        user->profile.role = std::nullopt;
        user->profile.affiliation = std::nullopt;
        
        // For roster contacts (not in a MUC), manage account buffer nicklist
        if (!channel)
        {
            if (binding->type && *binding->type == "unavailable")
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
            if (auto signature = binding->signature();
                    signature && channel->type != weechat::channel::chat_type::MUC)
            {
                user->profile.pgp_id = account.pgp.verify(channel->buffer, signature->data());
                if (user->profile.pgp_id.has_value())
                    channel->pgp.ids.emplace(user->profile.pgp_id.value());
            }

            if (user->profile.role.has_value())
                channel->remove_member(binding->from->full.data(), status ? status->data() : nullptr);
            else
                channel->add_member(binding->from->full.data(), clientid.data(), std::nullopt, user);
        }
    }

    // XEP-0283: Moved — detect JID migration notice in subscription requests.
    // Per §4.3, when we receive <presence type='subscribe'> containing
    // <moved xmlns='urn:xmpp:moved:1'><old-jid>…</old-jid></moved>, the
    // sender's new JID is binding->from->bare and the old JID is in <old-jid>.
    // We display a notice; full verification (PEP fetch) is left to the user.
    if (binding->type && *binding->type == "subscribe")
    {
        xmpp_stanza_t *moved_elem = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "moved", "urn:xmpp:moved:1");
        if (moved_elem)
        {
            xmpp_stanza_t *old_jid_elem = xmpp_stanza_get_child_by_name(
                moved_elem, "old-jid");
            std::string old_jid_str;
            if (old_jid_elem)
            {
                xmpp_string_guard old_jid_g(account.context,
                                            xmpp_stanza_get_text(old_jid_elem));
                if (old_jid_g.ptr)
                    old_jid_str = old_jid_g.ptr;
            }
            const char *new_jid = binding->from ? binding->from->bare.data() : nullptr;
             if (!old_jid_str.empty() && new_jid)
             {
                 std::string msg = fmt::format("{}{}Contact {}{}{} has moved to {}{}{} — verify and update your roster",
                                               weechat_prefix("network"),
                                               weechat::xmpp_color("yellow").c_str(),
                                               weechat::xmpp_color("bold").c_str(), old_jid_str, weechat::xmpp_color("reset").c_str(),
                                               weechat::xmpp_color("bold").c_str(), new_jid, weechat::xmpp_color("reset").c_str());
                 weechat_printf_date_tags(account.buffer, 0,
                     "xmpp_presence,notify_highlight", "%s", msg.c_str());
             }
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
                xmpp_string_guard photo_hash_g(account.context,
                                               xmpp_stanza_get_text(photo));
                std::string_view photo_hash = photo_hash_g.ptr ? photo_hash_g.ptr : "";
                if (!photo_hash.empty())
                {
                    // Store the vCard avatar hash if we don't already have one
                    // from XEP-0084 (prefer PEP avatar over legacy vCard avatar)
                    if (user->profile.avatar_hash.empty())
                        user->profile.avatar_hash = photo_hash;
                    // Auto-fetch vCard to retrieve the photo and profile info if
                    // we haven't fetched it yet, or if the hash changed.
                    bool hash_changed = (user->profile.avatar_hash != photo_hash);
                    if (!user->profile.vcard_fetched || hash_changed)
                    {
                        // Use the bare JID for the vCard request
                        const char *from_full = xmpp_stanza_get_from(stanza);
                        if (from_full)
                        {
                            std::string bare = jid(nullptr, from_full).bare;
                            if (!bare.empty())
                            {
                                std::shared_ptr<xmpp_stanza_t> iq {
                                    ::xmpp::xep0054::vcard_request(account.context, bare.c_str()),
                                    xmpp_stanza_release};
                                send(iq.get());
                            }
                        }
                    }
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

