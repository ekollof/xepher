void weechat::connection::handle_pubsub_pep_event(xmpp_stanza_t *stanza, std::string_view own_jid_str)
{
    const ::xmpp::StanzaView view(stanza);
    const std::string iq_id_str = view.attr_string("id");
    const std::string iq_from_str = view.attr_string("from");
    const std::string iq_to_str = view.attr_string("to");
    const std::string iq_type_str = view.attr_string("type");
    const char *id = iq_id_str.empty() ? nullptr : iq_id_str.c_str();
    const char *from = iq_from_str.empty() ? nullptr : iq_from_str.c_str();
    const char *to = iq_to_str.empty() ? nullptr : iq_to_str.c_str();
    const char *type = iq_type_str.empty() ? nullptr : iq_type_str.c_str();
    (void)id; (void)type;
    const std::string own_jid_storage(own_jid_str);
    const char *own_jid = own_jid_storage.c_str();
    const ::xmpp::StanzaView event = view.child("event", "http://jabber.org/protocol/pubsub#event");
    if (!event.valid())
        return;

    const ::xmpp::StanzaView items = event.child("items");
        if (!items.valid())
        return;

    const std::string items_node = items.attr_string("node");
        
        
        
        // Log all PEP events for debugging/future features (XEP-0163)
        if (!items_node.empty())
        {
            XDEBUG("PEP event from {}: {}",
                   from ? from : own_jid,
                   items_node);
        }
        
        // XEP-0084: User Avatar - Metadata  
        if (!items_node.empty() && weechat_strcasecmp(items_node.c_str(), "urn:xmpp:avatar:metadata") == 0)
        {
            const ::xmpp::StanzaView item = items.child("item");
            if (item.valid())
            {
                const ::xmpp::StanzaView metadata = item.child("metadata", "urn:xmpp:avatar:metadata");
                
                if (metadata.valid())
                {
                    const ::xmpp::StanzaView info = metadata.child("info");
                    if (info.valid())
                    {
                        const std::string info_id_str = info.attr_string("id");
                const char *info_id = info_id_str.empty() ? nullptr : info_id_str.c_str();
                        const std::string info_type = info.attr_string("type");
                        const std::string info_bytes = info.attr_string("bytes");
                        
                         if (info_id)
                         {
                             const char *from_jid = from ? from : own_jid;

                             // Update user's avatar hash
                             weechat::user *user = weechat::user::search(&account, from_jid);
                             bool hash_changed = true;
                             if (user)
                             {
                                 hash_changed = (user->profile.avatar_hash != info_id);
                                 user->profile.avatar_hash = info_id;
                             }

                             // Only fetch/load data when hash is new or user was unknown
                             if (hash_changed || (user && user->profile.avatar_data.empty()))
                             {
                                 XDEBUG("Avatar update from {} (hash: {:.8}..., type: {}, bytes: {})",
                                        from_jid,
                                        info_id,
                                        info_type.empty() ? "unknown" : info_type.c_str(),
                                        info_bytes.empty() ? "unknown" : info_bytes.c_str());

                                 // request_data() checks cache before sending IQ
                                 weechat::avatar::request_data(account, from_jid, info_id);
                             }
                         }
                    }
                }
            }
        }
        
        // XEP-0084: User Avatar - Data
        if (!items_node.empty() && weechat_strcasecmp(items_node.c_str(), "urn:xmpp:avatar:data") == 0)
        {
            const ::xmpp::StanzaView item = items.child("item");
            if (item.valid())
            {
                const ::xmpp::StanzaView data_elem = item.child("data", "urn:xmpp:avatar:data");
                
                    if (data_elem.valid())
                    {
                const std::string b64_data = data_elem.text();
                if (!b64_data.empty())
                {
                    const char *from_jid = from ? from : own_jid;
                    if (weechat::avatar::apply_pep_data_b64(account, from_jid, b64_data))
                    {
                        XDEBUG("Avatar data received from {} (PEP push)", from_jid);
                    }
                }
                }
            }
        }
        
        // XEP-0402: PEP Native Bookmarks — incremental push notification.
        // The server sends individual <item> or <retract> events; this is
        // NOT a full dump, so we MUST NOT clear the bookmarks map here.
        if (!items_node.empty() && weechat_strcasecmp(items_node.c_str(), "urn:xmpp:bookmarks:1") == 0)
        {
            for (const ::xmpp::StanzaView item : ::xmpp::StanzaView(items))
            {
                const char *item_name = item.name().data();
                if (!item_name)
                    continue;

                // XEP-0402 §4: on <retract> notification, leave the room
                // immediately and remove the bookmark from the local map.
                if (weechat_strcasecmp(item_name, "retract") == 0)
                {
                    const std::string retract_id_str = item.attr_string("id");
                const char *retract_id = retract_id_str.empty() ? nullptr : retract_id_str.c_str();
                    if (!retract_id)
                        continue;

                    account.bookmarks.erase(retract_id);

                    if (auto ch_it = account.channels.find(retract_id); ch_it != account.channels.end())
                    {
                        auto& [_, ch] = *ch_it;
                        if (ch.buffer)
                        {
                            weechat::UiPort::for_buffer(ch.buffer)->printf_network(
                                "Bookmark removed — leaving room");
                            weechat_buffer_close(ch.buffer);
                        }
                    }
                    continue;
                }

                if (weechat_strcasecmp(item_name, "item") != 0)
                    continue;
                
                // <item id='roomjid@conference.example.org'>
                //   <conference xmlns='urn:xmpp:bookmarks:1'
                //               name='The Play Room'
                //               autojoin='true'>
                //     <nick>FeatherBrain</nick>
                //   </conference>
                // </item>
                
                const std::string item_id_str = item.attr_string("id");
                const char *item_id = item_id_str.empty() ? nullptr : item_id_str.c_str();
                if (!item_id)
                    continue;  // Item ID is the room JID
                
                const ::xmpp::StanzaView conference = item.child("conference", "urn:xmpp:bookmarks:1");
                if (!conference.valid())
                    continue;
                
                const std::string name = conference.attr_string("name");
                const std::string autojoin_str = conference.attr_string("autojoin");
                const char *autojoin = autojoin_str.empty() ? nullptr : autojoin_str.c_str();
                
                const ::xmpp::StanzaView nick_elem = conference.child("nick");
                const std::string nick_text_str = nick_elem.text();
                const char *nick_text = nick_text_str.empty()
                    ? nullptr : nick_text_str.c_str();

                bool do_autojoin = autojoin &&
                    (weechat_strcasecmp(autojoin, "true") == 0 ||
                     weechat_strcasecmp(autojoin, "1") == 0);
                
                // Upsert bookmark
                account.bookmarks[item_id].jid = item_id;
                account.bookmarks[item_id].name = conference.attr_string("name");
                account.bookmarks[item_id].nick = nick_text ? nick_text : "";
                account.bookmarks[item_id].autojoin = do_autojoin;

                // XEP-0492: read notification setting from <extensions><notify>.
                const ::xmpp::StanzaView extensions_elem = conference.child("extensions");
                if (extensions_elem.valid())
                {
                    const ::xmpp::StanzaView notify_elem = extensions_elem.child("notify", "urn:xmpp:notification-settings:1");
                    if (notify_elem.valid())
                    {
                        // Pick the most specific fallback element (no identity attrs).
                        // Priority: <always>, <on-mention>, <never> (fallback without attrs).
                        static constexpr const char *levels[] = { "always", "on-mention", "never" };
                        for (const char *lvl : levels)
                        {
                            for (const ::xmpp::StanzaView notify_child : ::xmpp::StanzaView(notify_elem))
                            {
                                const char *cname = notify_child.name().data();
                                if (cname && weechat_strcasecmp(cname, lvl) == 0
                                    && notify_child.attr_string("identity-category").empty())
                                {
                                    account.bookmarks[item_id].notify_setting = lvl;
                                    goto xep0492_done;
                                }
                            }
                        }
                        xep0492_done:;
                    }
                }
                
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
                
                if (do_autojoin)
                {
                    // XEP-0402 §4: join immediately on autojoin=true notification.
                    // Skip biboumi (IRC gateway) rooms.
                    std::string_view item_sv(item_id);
                    bool is_biboumi = item_sv.contains('%') ||
                                    item_sv.contains("biboumi") ||
                                    item_sv.contains("@irc.");

                    // Guard: if we are already in this room (buffer exists and
                    // join completed), do not re-enter.  The server echoes our
                    // own PEP publishes back to us as push notifications, so
                    // without this guard every bookmark publish during a session
                    // triggers a redundant /enter, producing a flood of join
                    // presences visible to other clients (e.g. Movim).
                    bool already_joined = false;
                    if (auto existing_ch = account.channels.find(item_id); existing_ch != account.channels.end())
                    {
                        auto& [_, ch] = *existing_ch;
                        already_joined = ch.buffer && !ch.joining;
                    }

                     if (!is_biboumi && !already_joined)
                     {
                         const auto& bookmark = account.bookmarks[item_id];
                         const std::string cmd =
                             ::xmpp::bookmark_enter_command(item_id, bookmark.nick);
                         weechat_command(account.buffer, cmd.c_str());
                     }
                    else if (is_biboumi)
                    {
                        weechat::UiPort::for_buffer(account.buffer)->printf_network(fmt::format(
                            "Skipping autojoin for IRC gateway room: {}", item_id));
                    }
                    // else: already_joined — PEP echo of our own publish, ignore silently
                }
                else
                {
                    // XEP-0402 §4 (v1.2.0): autojoin is false (or absent).
                    // Only close the buffer if we have already *fully* joined
                    // the room (joining==false, i.e. status 110 was received).
                    // During initial session setup the server sends the full
                    // bookmark list as individual PEP notifications; closing
                    // buffers here at that point would destroy channels that
                    // were just created by the XEP-0049 autojoin flow.
                    if (auto ch_it = account.channels.find(item_id); ch_it != account.channels.end())
                    {
                        auto& [_, ch] = *ch_it;
                        if (ch.buffer && !ch.joining)
                        {
                            weechat::UiPort::for_buffer(ch.buffer)->printf_network(
                                "Bookmark autojoin disabled — leaving room");
                            weechat_buffer_close(ch.buffer);
                        }
                    }
                }
            }
            
            XDEBUG("Processed bookmark PEP push; {} bookmarks total", account.bookmarks.size());
        }

        // XEP-0490: Message Displayed Synchronization
        // <items node='urn:xmpp:mds:displayed:0'>
        //   <item id='thecontactjid@example.org'>
        //     <displayed xmlns='urn:xmpp:mds:displayed:0'>
        //       <stanza-id xmlns='urn:xmpp:sid:0' by='thecontactjid@example.org' id='stanza-id-of-last-displayed'/>
        //     </displayed>
        //   </item>
        // </items>
        if (!items_node.empty() && weechat_strcasecmp(items_node.c_str(), "urn:xmpp:mds:displayed:0") == 0)
        {
            for (const ::xmpp::StanzaView item : ::xmpp::StanzaView(items))
            {
                if (weechat_strcasecmp(item.name().data(), "item") != 0)
                    continue;

                const std::string peer_jid_str = item.attr_string("id");
                const char *peer_jid = peer_jid_str.empty() ? nullptr : peer_jid_str.c_str();
                if (!peer_jid)
                    continue;

                const ::xmpp::StanzaView displayed_elem = item.child("displayed", "urn:xmpp:mds:displayed:0");
                if (!displayed_elem.valid())
                    continue;

                // Extract the stanza-id of the last displayed message
                const ::xmpp::StanzaView sid_elem = displayed_elem.child("stanza-id", "urn:xmpp:sid:0");
                const std::string last_id_str = sid_elem.valid() ? sid_elem.attr_string("id") : std::string{};
                const char *last_id = last_id_str.empty() ? nullptr : last_id_str.c_str();

                // Only act on events from other devices of our own account
                const char *event_from = from ? from : own_jid;
                std::string own_bare_s  = ::jid(nullptr, own_jid).bare;
                std::string event_bare_s = ::jid(nullptr, event_from).bare;
                bool is_own = !own_bare_s.empty() && !event_bare_s.empty() &&
                    (weechat_strcasecmp(own_bare_s.c_str(), event_bare_s.c_str()) == 0);

                if (!is_own)
                    continue;

                // Find the channel for this peer and clear its unreads
                // up to and including last_id (or all if id unknown)
                weechat::channel *ch = nullptr;
                if (auto it = account.channels.find(peer_jid); it != account.channels.end())
                {
                    auto& [_, c] = *it;
                    ch = &c;
                }
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

            }
        }

        // Fallback: unknown PubSub node — treat as a generic feed.
        // Handles both <item> (publish) and <retract> events from
        // pubsub services like news.movim.eu (XEP-0060 §7.1 / §7.2).
        //
        // Guard: only treat nodes as feeds when:
        //   1. The node name is not a URI/namespace (no "://" and not "urn:")
        //      — PEP nodes always use namespace URIs, feed nodes use plain names.
        //   2. The stanza comes from a different bare JID than our own account
        //      — PEP events from self are internal protocol nodes (nick, mood, …)
        //
        const std::string_view feed_service_sv = from ? std::string_view(from) : std::string_view{};
        const std::string_view node_sv = !items_node.empty() ? std::string_view(items_node) : std::string_view{};

        const auto feed_gate = ::xmpp::classify_generic_pubsub_feed(
            node_sv, feed_service_sv, own_jid_str);
        if (feed_gate.drop_legacy_omemo)
        {
            XDEBUG("Dropping PEP event for legacy OMEMO node (not a user feed): {} from {}",
                   node_sv, feed_service_sv);
        }

        if (feed_gate.is_generic_feed)
        {
            // Buffer key: "service/node", e.g. "news.movim.eu/Phoronix"
            std::string feed_key = fmt::format("{}/{}", feed_service_sv, node_sv);

            // Ignore pushes for feeds the user closed (/feed close or /close).
            if (!account.feed_is_open(feed_key))
                return;

            // Ensure a FEED buffer exists for this node
            auto [ch_it, inserted] = account.channels.try_emplace(
                feed_key,
                account,
                weechat::channel::chat_type::FEED,
                feed_key,
                feed_key);
            auto& [_, feed_ch] = *ch_it;
            if (inserted)
                account.feed_open_register(feed_key);

            bool has_item    = false;
            bool has_retract = false;

            for (const ::xmpp::StanzaView child : ::xmpp::StanzaView(items))
            {
                const char *child_name = child.name().data();
                if (!child_name)
                    continue;

                if (weechat_strcasecmp(child_name, "item") == 0)
                {
                    has_item = true;

                    // Dedup: skip items we have already rendered (XEP-0060 push)
                    const std::string item_id_raw_str = child.attr_string("id");
                const char *item_id_raw = item_id_raw_str.empty() ? nullptr : item_id_raw_str.c_str();
                    if (item_id_raw && account.feed_item_seen(feed_key, item_id_raw))
                        continue;

                    // Extract Atom payload (http://www.w3.org/2005/Atom)
                    ::xmpp::StanzaView entry = child.child("entry", "http://www.w3.org/2005/Atom");
                    if (!entry.valid()) entry = child.child("entry");
                    ::xmpp::StanzaView feed = child.child("feed", "http://www.w3.org/2005/Atom");
                    if (!feed.valid()) feed = child.child("feed");
                    if (!entry.valid() && feed.valid())
                    {
                        atom_feed af = parse_atom_feed(feed);
                        if (!af.empty())
                        {
                            if (!af.title.empty())
                            {
                                feed_ch.update_name(af.title.c_str());
                                weechat::UiPort::for_buffer(feed_ch.buffer)->printf_date_tags_network(
                                    0, "xmpp_feed,notify_none",
                                    fmt::format("Feed title:{} {}",
                                        weechat::RuntimePort::default_runtime().color("reset"),
                                        af.title));
                            }
                            if (!af.author.empty())
                                weechat::UiPort::for_buffer(feed_ch.buffer)->printf_date_tags(
                                    0, "xmpp_feed,notify_none",
                                    fmt::format("  {}Author:{} {}",
                                        weechat::RuntimePort::default_runtime().color("darkgray"),
                                        weechat::RuntimePort::default_runtime().color("reset"),
                                        af.author));
                            if (!af.subtitle.empty())
                                weechat::UiPort::for_buffer(feed_ch.buffer)->printf_date_tags(
                                    0, "xmpp_feed,notify_none",
                                    fmt::format("  {}", af.subtitle));
                        }
                        if (item_id_raw)
                            account.feed_item_mark_seen(feed_key, item_id_raw);
                        continue;
                    }

                    const std::string publisher = child.attr_string("publisher");
                    atom_entry ae = parse_atom_entry(entry, publisher);
                    if (item_id_raw && !ae.item_id.empty())
                        account.feed_atom_id_set(feed_key, item_id_raw, ae.item_id);
                    if (item_id_raw && !ae.replies_link.empty())
                        account.feed_replies_link_set(feed_key, item_id_raw, ae.replies_link);

                    // Assign a short #N alias for this item.
                    int item_alias = -1;
                    if (item_id_raw && *item_id_raw)
                        item_alias = account.feed_alias_assign(feed_key, item_id_raw);

                    // When true, display was deferred to the IQ result handler;
                    // do NOT mark the item seen here — the IQ handler will do it.
                    bool deferred_to_iq = false;

                    if (ae.empty())
                    {
                        // Notification-only push (no Atom payload in stanza).
                        // Fetch the specific item by ID so we can display it
                        // properly once the IQ result arrives (XEP-0060 §6.5.7).
                        if (item_id_raw)
                        {
                            std::string feed_service_str(feed_service_sv);
                            std::string node_str(node_sv);
                            std::string uid = stanza::uuid(account.context);
                            stanza::xep0060::items its(node_str);
                            its.item(stanza::xep0060::item().id(item_id_raw));
                            stanza::xep0060::pubsub ps;
                            ps.items(its);
                            account.pubsub_fetch_ids[uid] = {feed_service_str, node_str, "", 0};
                            account.connection.send(stanza::iq()
                                .from(to ? to : account.jid())
                                .to(feed_service_str)
                                .type("get")
                                .id(uid)
                                .xep0060()
                                .pubsub(ps)
                                .build(account.context)
                                .get());
                            deferred_to_iq = true;
                        }
                        else
                        {
                            // No ID at all — nothing useful to show or fetch
                            weechat::UiPort::for_buffer(feed_ch.buffer)->printf_date_tags_network(
                                0, "xmpp_feed,notify_message",
                                fmt::format("[{}] New item (no content)", std::string(node_sv)));
                        }
                    }
                    else
                    {
                        ::xmpp::render_atom_entry_to_feed(
                            feed_ch,
                            account,
                            feed_key,
                            feed_service_sv,
                            node_sv,
                            item_id_raw ? std::string_view(item_id_raw) : std::string_view{},
                            item_alias,
                            ae);
                    }
                    // Mark this item seen so push duplicates are suppressed.
                    // Skip if we deferred display to the IQ result handler —
                    // it will call mark_seen after actually rendering the item.
                    if (item_id_raw && !deferred_to_iq)
                        account.feed_item_mark_seen(feed_key, item_id_raw);
                }
                else if (weechat_strcasecmp(child_name, "retract") == 0)
                {
                    has_retract = true;
                }
            }

            // On retract: fetch current items via IQ get (XEP-0060 §6.5.2)
            if (has_retract && !has_item)
            {
                std::string feed_service_str(feed_service_sv);
                std::string node_str(node_sv);
                std::string uid = stanza::uuid(account.context);
                stanza::xep0060::items its(node_str);
                stanza::xep0060::pubsub ps;
                ps.items(its);
                account.pubsub_fetch_ids[uid] = {feed_service_str, node_str, "", 0};
                account.connection.send(stanza::iq()
                    .from(to ? to : account.jid())
                    .to(feed_service_str)
                    .type("get")
                    .id(uid)
                    .xep0060()
                    .pubsub(ps)
                    .build(account.context)
                    .get());
            }
        }
    }
