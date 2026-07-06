bool weechat::connection::presence_handler(xmpp_stanza_t *stanza, bool top_level)
{
    // SM counter incremented in libstrophe wrapper, not here
    // top_level parameter kept for nested/recursive calls

    (void) top_level;
    append_raw_xml_trace(account, "RECV", stanza);

    weechat::user *user;
    weechat::channel *channel;

    const auto pres = ::xmpp::parse_presence(::xmpp::StanzaView(stanza));
    if (!pres.from)
        return 1;

    std::string clientid;
    if (pres.caps)
    {
        const auto &caps = *pres.caps;
        clientid = fmt::format("{}#{}", caps.node, caps.verification);

        // Check if we have this capability hash cached (XEP-0115)
        std::vector<std::string> cached_features;
        if (account.caps_cache_get(caps.verification, cached_features))
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
            const bool has_resource = !pres.from->resource.empty();
            if (has_resource)
            {
                std::string disco_id = stanza::uuid(account.context);
                account.caps_disco_queries[disco_id] = caps.verification;

                account.connection.send(stanza::iq()
                            .from(pres.to ? pres.to->full : "")
                            .to(pres.from->full)
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
        if (auto ch_it = account.channels.find(pres.from->bare); ch_it != account.channels.end())
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

    if (pres.type && *pres.type == "error" && channel && pres.error)
    {
        const auto &error = *pres.error;
        const char *error_reason = error.reason.c_str();
        const char *from_str = pres.from->full.c_str();

        // Only log if the error has meaningful information
        // Skip generic "Unspecified" errors that don't have useful context
        if (error.reason != "Unspecified" || error.description)
        {
            std::string msg = fmt::format("[!]\t{}{}{}{}{}{}",
                                          weechat::RuntimePort::default_runtime().xmpp_color("gray").c_str(),
                                          pres.has_muc ? "MUC " : "",
                                          error_reason,
                                          error.description ? " (" : "",
                                          error.description ? error.description->c_str() : "",
                                          error.description ? ")" : "");
            weechat::UiPort::for_buffer(channel->buffer)->printf(msg);
            if (pres.has_muc
                && error.reason == "Not on Member List")
                {
                    auto ui = weechat::UiPort::for_buffer(channel->buffer);
                    ui->printf_error(fmt::format(
                        "{}: this room may require membership — "
                        "try /mucregister [nick] or /mucregister query",
                        WEECHAT_XMPP_PLUGIN_NAME));
                }
        }
        else
        {
            // Debug: log unspecified errors with JID
            weechat::UiPort::for_buffer(account.buffer)->printf_network(
                fmt::format("[DEBUG] Received unspecified error from {} (presence)", from_str));
        }
        return 1;
    }

    if (pres.muc_user)
    {
        const auto &x = *pres.muc_user;
        bool is_nick_change = false;
        bool is_new_room = false;
        bool is_server_nick = false;
        bool presence_is_self = false;
        std::string self_real_jid_storage;
        for (int status : x.statuses)
        {
            switch (status)
            {
                case 100: // Non-Anonymous: [message | Entering a room]: Inform user that any occupant is allowed to see the user's full JID
                    break;
                case 101: // Affiliation change: room visibility changed, JID now visible to all
                    if (channel)
                    {
                        weechat::UiPort::for_buffer(channel->buffer)->printf_date_tags_network(
                            0, "xmpp_presence,notify_none,no_trigger",
                            fmt::format("{}[Room] Your affiliation changed; your full JID is now visible to all occupants",
                                weechat::RuntimePort::default_runtime().xmpp_color("gray")));
                    }
                    break;
                case 102: // : [message | Configuration change]: Inform occupants that room now shows unavailable members
                    if (channel)
                        channel->set_show_unavailable_members(true);
                    break;
                case 103: // : [message | Configuration change]: Inform occupants that room now does not show unavailable members
                    if (channel)
                        channel->set_show_unavailable_members(false);
                    break;
                case 104: // : [message | Configuration change]: Inform occupants that a non-privacy-related room configuration change has occurred
                    // Re-fetch disco#info so the buffer's "modes" string reflects
                    // the new configuration. Drop the idempotency marker so the
                    // query actually goes out (the in-flight map is keyed by IQ id,
                    // not room JID, so concurrent queries for the same room are
                    // already de-duplicated there).
                    if (channel && channel->type == weechat::channel::chat_type::MUC)
                    {
                        account.muc_modes_fetched.erase(channel->id);
                        std::string modes_id = stanza::uuid(account.context);
                        account.muc_modes_queries[modes_id] = channel->id;
                        account.muc_modes_fetched.insert(channel->id);
                        account.connection.send(
                            stanza::iq()
                                .type("get")
                                .to(channel->id)
                                .id(modes_id)
                                .xep0030()
                                .query()
                                .build(account.context)
                                .get()
                        );
                    }
                    break;
                case 110: // Self-Presence: [presence | Any room presence]: Inform user that presence refers to one of its own room occupants
                    presence_is_self = true;
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

                            // XEP-0045 §6.4 / §6.5: discover the room's mode flags
                            // (muc_moderated, muc_membersonly, …) and muc#roominfo
                            // metadata so we can render the buffer's "modes" property
                            // IRC-style. Idempotent: only one in-flight query per room
                            // at a time. Mark the room as "fetched" once we have sent
                            // the request so subsequent joins don't re-query.
                            if (!account.muc_modes_fetched.contains(channel->id))
                            {
                                std::string modes_id = stanza::uuid(account.context);
                                account.muc_modes_queries[modes_id] = channel->id;
                                account.muc_modes_fetched.insert(channel->id);
                                account.connection.send(
                                    stanza::iq()
                                        .type("get")
                                        .to(channel->id)
                                        .id(modes_id)
                                        .xep0030()
                                        .query()
                                        .build(account.context)
                                        .get()
                                );
                            }
                        }
                    }
                    break;
                case 170: // Logging Active: room logging enabled — privacy notice
                    if (channel)
                    {
                        weechat::UiPort::for_buffer(channel->buffer)->printf_date_tags_network(
                            0, "xmpp_presence,notify_none,no_trigger",
                            fmt::format("{}[Room] Warning: room logging is now {}enabled{} — messages are being stored",
                                weechat::RuntimePort::default_runtime().xmpp_color("yellow"),
                                weechat::RuntimePort::default_runtime().xmpp_color("yellow,bold"),
                                weechat::RuntimePort::default_runtime().xmpp_color("reset")));
                    }
                    break;
                case 171: // Logging disabled
                    if (channel)
                    {
                        weechat::UiPort::for_buffer(channel->buffer)->printf_date_tags_network(
                            0, "xmpp_presence,notify_none,no_trigger",
                            fmt::format("{}[Room] Room logging is now disabled",
                                weechat::RuntimePort::default_runtime().xmpp_color("gray")));
                    }
                    break;
                case 172: // Room is now non-anonymous (full JIDs visible)
                    if (channel)
                    {
                        channel->set_muc_anonymity(
                            weechat::channel::muc_info::anonymity::nonanonymous);
                        weechat::UiPort::for_buffer(channel->buffer)->printf_date_tags_network(
                            0, "xmpp_presence,notify_none,no_trigger",
                            fmt::format("{}[Room] Room is now {}non-anonymous{} — full JIDs are visible to all occupants",
                                weechat::RuntimePort::default_runtime().xmpp_color("yellow"),
                                weechat::RuntimePort::default_runtime().xmpp_color("yellow,bold"),
                                weechat::RuntimePort::default_runtime().xmpp_color("reset")));
                    }
                    break;
                case 173: // Room is now semi-anonymous
                    if (channel)
                    {
                        channel->set_muc_anonymity(
                            weechat::channel::muc_info::anonymity::semianonymous);
                        weechat::UiPort::for_buffer(channel->buffer)->printf_date_tags_network(
                            0, "xmpp_presence,notify_none,no_trigger",
                            fmt::format("{}[Room] Room is now semi-anonymous — full JIDs visible to moderators only",
                                weechat::RuntimePort::default_runtime().xmpp_color("gray")));
                    }
                    break;
                case 174: // Room is now fully-anonymous
                    if (channel)
                    {
                        channel->set_muc_anonymity(
                            weechat::channel::muc_info::anonymity::anonymous);
                        weechat::UiPort::for_buffer(channel->buffer)->printf_date_tags_network(
                            0, "xmpp_presence,notify_none,no_trigger",
                            fmt::format("{}[Room] Room is now fully-anonymous — JIDs are hidden from all occupants",
                                weechat::RuntimePort::default_runtime().xmpp_color("gray")));
                    }
                    break;
                case 201: // New room created — must accept default config to unlock it
                    is_new_room = true;
                    if (channel)
                    {
                        weechat::UiPort::for_buffer(channel->buffer)->printf_date_tags_network(
                            0, "xmpp_presence,notify_none,no_trigger",
                            fmt::format("{}[Room] New room created; accepting default configuration",
                                weechat::RuntimePort::default_runtime().xmpp_color("gray")));
                    }
                    break;
                case 210: // Server assigned/modified nick
                    is_server_nick = true;
                    break;
                case 301: // : [presence | Removal from room]: Inform user that he or she has been banned from the room
                    if (channel)
                    {
                        std::string msg = fmt::format("[!]\t{}Banned from Room", weechat::RuntimePort::default_runtime().xmpp_color("gray").c_str());
                        weechat::UiPort::for_buffer(channel->buffer)->printf_date_tags(
    0, "xmpp_presence,notify_none,no_trigger", msg);
                    }
                    break;
                case 303: // Nick change — this unavailable presence means occupant is changing nick
                    is_nick_change = true;
                    break;
                case 307: // : [presence | Removal from room]: Inform user that he or she has been kicked from the room
                    if (channel)
                    {
                        std::string msg = fmt::format("[!]\t{}Kicked from room", weechat::RuntimePort::default_runtime().xmpp_color("gray").c_str());
                        weechat::UiPort::for_buffer(channel->buffer)->printf_date_tags(
    0, "xmpp_presence,notify_none,no_trigger", msg);
                    }
                    break;
                case 321: // : [presence | Removal from room]: Inform user that he or she is being removed from the room because of an affiliation change
                    if (channel)
                    {
                        std::string msg = fmt::format("[!]\t{}Room Affiliation changed, kicked", weechat::RuntimePort::default_runtime().xmpp_color("gray").c_str());
                        weechat::UiPort::for_buffer(channel->buffer)->printf_date_tags(
    0, "xmpp_presence,notify_none,no_trigger", msg);
                    }
                    break;
                case 322: // : [presence | Removal from room]: Inform user that he or she is being removed from the room because the room has been changed to members-only and the user is not a member
                    if (channel)
                    {
                        std::string msg = fmt::format("[!]\t{}Room now members-only, kicked", weechat::RuntimePort::default_runtime().xmpp_color("gray").c_str());
                        weechat::UiPort::for_buffer(channel->buffer)->printf_date_tags(
    0, "xmpp_presence,notify_none,no_trigger", msg);
                    }
                    break;
                case 332: // : [presence | Removal from room]: Inform user that he or she is being removed from the room because of a system shutdown
                    if (channel)
                    {
                        std::string msg = fmt::format("[!]\t{}Room Shutdown", weechat::RuntimePort::default_runtime().xmpp_color("gray").c_str());
                        weechat::UiPort::for_buffer(channel->buffer)->printf_date_tags(
    0, "xmpp_presence,notify_none,no_trigger", msg);
                    }
                    break;
                default:
                    break;
            }
        }

        for (const auto &item : x.items)
        {
            const std::string role = item.role.value_or("");
            const std::string affiliation = item.affiliation.value_or("");
            const std::string client_jid = item.real_jid ? *item.real_jid : clientid;

            user = weechat::user::search(&account, pres.from->full);
            if (!user)
            {
                const std::string name = pres.from->full;
                const std::string nick = ::xmpp::presence_display_name(
                    pres.from->bare, pres.from->resource, pres.from->full,
                    channel ? std::string_view(channel->id) : std::string_view{});
                auto [it_u, _ins_u] = account.users.emplace(std::piecewise_construct,
                                              std::forward_as_tuple(name),
                                              std::forward_as_tuple(&account, channel, name, nick));
                auto& [_, u] = *it_u;
                user = &u;
            }
            user->profile.status_text = pres.status;
            user->profile.status = pres.show;
            user->profile.idle = pres.idle_since
                ? fmt::format("{}", *pres.idle_since) : std::string();
            user->is_away = pres.show && *pres.show == "away";
            user->profile.role = role.empty() ? std::nullopt : std::optional<std::string>(role);
            user->profile.affiliation = (!affiliation.empty() && affiliation != "none")
                ? std::optional<std::string>(affiliation) : std::nullopt;

            // XEP-0384 §5.8.1: drop banned / de-affiliated users from encrypt targets.
            if (channel
                && channel->type == weechat::channel::chat_type::MUC
                && item.real_jid
                && (affiliation == "none" || affiliation == "outcast"))
            {
                channel->unregister_omemo_recipient(*item.real_jid);
            }

            if (channel)
            {
                if (pres.signature && channel->type != weechat::channel::chat_type::MUC)
                {
                    user->profile.pgp_id = account.pgp.verify(channel->buffer, pres.signature->c_str());
                    if (user->profile.pgp_id.has_value())
                        channel->pgp.ids.emplace(user->profile.pgp_id.value());
                }

                if (weechat_strcasecmp(role.c_str(), "none") == 0)
                {
                    if (is_nick_change && item.nick && channel)
                    {
                        // Status 303: nick change — print rename notice instead of "left"
                        const char *old_nick = !pres.from->resource.empty()
                            ? pres.from->resource.c_str() : pres.from->full.c_str();
                        const char *nick_tags = channel->smart_filter_nick(old_nick)
                            ? "xmpp_presence,nick,log4,xmpp_smart_filter,no_trigger"
                            : "xmpp_presence,nick,log4,no_trigger";
                        weechat::UiPort::for_buffer(channel->buffer)->printf_date_tags_network(
                            0, nick_tags,
                            fmt::format("{}{}{} is now known as {}{}",
                                weechat::RuntimePort::default_runtime().xmpp_color("irc.color.nick_change"),
                                old_nick,
                                weechat::RuntimePort::default_runtime().xmpp_color("reset"),
                                weechat::RuntimePort::default_runtime().xmpp_color("irc.color.nick_change"),
                                *item.nick));
                        weechat::user *leaving = weechat::user::search(&account, pres.from->full);
                        if (leaving)
                            leaving->nicklist_remove(&account, channel);
                    }
                    else if (user->profile.affiliation.has_value())
                        channel->set_member_offline(pres.from->full, user);
                    else
                        channel->remove_member(pres.from->full,
                                               pres.status.value_or(""));
                }
                else
                {
                    std::optional<std::string_view> real_jid_sv;
                    if (item.real_jid)
                        real_jid_sv = *item.real_jid;
                    else if (presence_is_self)
                    {
                        self_real_jid_storage = account.jid();
                        if (!self_real_jid_storage.empty())
                            real_jid_sv = self_real_jid_storage;
                    }

                    channel->add_member(pres.from->full, client_jid,
                                        real_jid_sv,
                                        user,
                                        {.announce_join = true, .online = true});

                    if (is_server_nick && channel)
                    {
                        const char *assigned = !pres.from->resource.empty()
                            ? pres.from->resource.c_str() : pres.from->full.c_str();
                        weechat::UiPort::for_buffer(channel->buffer)->printf_date_tags_network(
                            0, "xmpp_presence,notify_none,no_trigger",
                            fmt::format("{}[Room] Server assigned you the nick: {}{}{}",
                                weechat::RuntimePort::default_runtime().xmpp_color("gray"),
                                weechat::RuntimePort::default_runtime().xmpp_color("irc.color.nick_change"),
                                assigned,
                                weechat::RuntimePort::default_runtime().xmpp_color("reset")));
                    }
                }
            }
        }

        if (is_new_room && channel)
        {
            // Status 201: new room was created.
            const std::string room_jid = pres.from->bare;

            // XEP-0045 §10.1 reserved room flow: /create --reserved suppresses
            // the auto-empty submit so the room stays locked. The user is
            // expected to run /setmodes (or /affiliation, /destroy) to
            // configure it before anyone else joins.
            if (account.muc_reserved_pending.contains(room_jid))
            {
                account.muc_reserved_pending.erase(room_jid);
                // Trigger a form fetch so /setmodes --confirm has something
                // to mutate immediately on the first invocation.
                std::string get_id = stanza::uuid(account.context);
                weechat::account::muc_owner_query_info info{
                    room_jid,
                    channel->buffer,
                    weechat::account::muc_owner_kind::config_get
                };
                account.muc_owner_queries[get_id] = info;
                stanza::xep0045::xep0045owner::query q;
                auto get_iq = stanza::iq().type("get").to(room_jid).id(get_id);
                get_iq.muc_owner(q);
                account.connection.send(get_iq.build(account.context).get());
                weechat::UiPort::for_buffer(channel->buffer)->printf_date_tags_network(
                    0, "xmpp_presence,notify_none,no_trigger",
                    fmt::format("{}[Room] reserved room created (locked) — use /setmodes, "
                        "/affiliation, or /destroy to configure",
                        weechat::RuntimePort::default_runtime().xmpp_color("yellow")));
            }
            else
            {
                // Instant room: send empty config submit to unlock it with
                // server defaults.
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
    }
    else
    {
        user = user::search(&account, pres.from->full);
        if (!user)
        {
            const std::string name = pres.from->full;
            const std::string nick = ::xmpp::presence_display_name(
                pres.from->bare, pres.from->resource, pres.from->full,
                channel ? std::string_view(channel->id) : std::string_view{});
            auto [it_u, _ins_u] = account.users.emplace(std::piecewise_construct,
                                          std::forward_as_tuple(name),
                                          std::forward_as_tuple(&account, channel, name, nick));
            auto& [_, u] = *it_u;
            user = &u;
        }
        user->profile.status_text = pres.status;
        user->profile.status = pres.show;
        user->profile.idle = pres.idle_since
            ? fmt::format("{}", *pres.idle_since) : std::string();
        user->is_away = pres.show && *pres.show == "away";
        user->profile.role = std::nullopt;
        user->profile.affiliation = std::nullopt;

        // For roster contacts (not in a MUC), manage account buffer nicklist
        if (!channel)
        {
            const std::string &bare_jid = pres.from->bare;
            const bool unavailable = pres.type && *pres.type == "unavailable";

            if (!unavailable)
            {
                user->is_online = true;
                user->is_away = pres.show && *pres.show == "away";
            }
            else
            {
                user->is_online = false;
            }

            if (account.roster.contains(bare_jid)
                && account.roster[bare_jid].subscription != "none")
            {
                account.update_roster_nicklist_entry(bare_jid);
            }
            else if (!unavailable)
            {
                user->nicklist_remove(&account, nullptr);
                user->nicklist_add(&account, nullptr);
            }
            else
            {
                user->nicklist_remove(&account, nullptr);
            }
        }
        
        if (channel)
        {
            if (pres.signature && channel->type != weechat::channel::chat_type::MUC)
            {
                user->profile.pgp_id = account.pgp.verify(channel->buffer, pres.signature->c_str());
                if (user->profile.pgp_id.has_value())
                    channel->pgp.ids.emplace(user->profile.pgp_id.value());
            }

            if (user->profile.role.has_value())
                channel->remove_member(pres.from->full, pres.status.value_or(""));
            else
                channel->add_member(pres.from->full, clientid, std::nullopt, user);
        }
    }

    // XEP-0283: Moved — detect JID migration notice in subscription requests.
    // Per §4.3, when we receive <presence type='subscribe'> containing
    // <moved xmlns='urn:xmpp:moved:1'><old-jid>…</old-jid></moved>, the
    // sender's new JID is pres.from->bare and the old JID is in <old-jid>.
    // We display a notice; full verification (PEP fetch) is left to the user.
    if (pres.type && *pres.type == "subscribe")
    {
        const auto presence_view = ::xmpp::StanzaView(stanza);
        const auto moved_elem = presence_view.child("moved", "urn:xmpp:moved:1");
        if (moved_elem.valid())
        {
            const std::string old_jid_str = moved_elem.child("old-jid").text();
            const char *new_jid = pres.from->bare.c_str();
             if (!old_jid_str.empty() && new_jid)
             {
                 weechat::UiPort::for_buffer(account.buffer)->printf_date_tags_network(
                     0, "xmpp_presence,notify_highlight",
                     fmt::format("{}Contact {}{}{} has moved to {}{}{} — verify and update your roster",
                         weechat::RuntimePort::default_runtime().xmpp_color("yellow"),
                         weechat::RuntimePort::default_runtime().xmpp_color("bold"),
                         old_jid_str,
                         weechat::RuntimePort::default_runtime().xmpp_color("reset"),
                         weechat::RuntimePort::default_runtime().xmpp_color("bold"),
                         new_jid,
                         weechat::RuntimePort::default_runtime().xmpp_color("reset")));
             }
        }
    }

    // XEP-0153: vCard-Based Avatars — parse photo hash from presence <x>
    // Only fetch vCards for roster contacts (non-MUC presences).
    // MUC occupant presences come from room@conference.example/nick — the bare
    // JID is the room itself, not a real user, so vcard-temp requests there
    // are meaningless and would spam the server.
    {
        const auto presence_view = ::xmpp::StanzaView(stanza);
        const auto vcard_x = presence_view.child("x", "vcard-temp:x:update");
        bool is_muc_presence = channel && channel->type == weechat::channel::chat_type::MUC;
        if (vcard_x.valid() && user && !is_muc_presence)
        {
            const auto photo = vcard_x.child("photo");
            if (photo.valid())
            {
                const std::string photo_hash_str = photo.text();
                std::string_view photo_hash = photo_hash_str;
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
                        if (auto from_full = presence_view.from())
                        {
                            std::string bare = jid(nullptr, from_full->data()).bare;
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

