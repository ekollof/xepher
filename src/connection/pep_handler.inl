void weechat::connection::handle_pubsub_pep_event(xmpp_stanza_t *stanza, std::string_view own_jid_str)
{
    xmpp_stanza_t *event = nullptr, *items = nullptr, *item = nullptr;
    const char *from = nullptr, *to = nullptr;
    const std::string own_jid_storage(own_jid_str);
    const char *own_jid = own_jid_storage.c_str();
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
            XDEBUG("PEP event from {}: {}",
                   from ? from : own_jid,
                   items_node);
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
                                        info_type ? info_type : "unknown",
                                        info_bytes ? info_bytes : "unknown");

                                 // request_data() checks cache before sending IQ
                                 weechat::avatar::request_data(account, from_jid, info_id);
                             }
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
                const std::string b64_data = stanza_element_text(data_elem);
                        if (!b64_data.empty())
                        {
                            // Decode base64 avatar data
                            BIO *bio, *b64;
                            size_t decode_len = b64_data.size();
                        std::vector<uint8_t> image_data(decode_len);
                        
                        bio = BIO_new_mem_buf(b64_data.data(), -1);
                        b64 = BIO_new(BIO_f_base64());
                        bio = BIO_push(b64, bio);
                        BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
                        
                        int actual_len = BIO_read(bio, image_data.data(), decode_len);
                        BIO_free_all(bio);
                        
                        if (actual_len > 0)
                        {
                            image_data.resize(actual_len);
                            
                            const char *from_jid = from ? from : own_jid;
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
                                user->cached_prefix_raw.clear();
                         }
                         
                         XDEBUG("Avatar data received from {} ({} bytes, hash verified: {})",
                                from_jid,
                                actual_len,
                                hash);
                         }
                     }
                }
            }
        }
        
        // XEP-0402: PEP Native Bookmarks — incremental push notification.
        // The server sends individual <item> or <retract> events; this is
        // NOT a full dump, so we MUST NOT clear the bookmarks map here.
        if (items_node
            && weechat_strcasecmp(items_node, "urn:xmpp:bookmarks:1") == 0)
        {
            for (item = xmpp_stanza_get_children(items);
                 item; item = xmpp_stanza_get_next(item))
            {
                const char *item_name = xmpp_stanza_get_name(item);
                if (!item_name)
                    continue;

                // XEP-0402 §4: on <retract> notification, leave the room
                // immediately and remove the bookmark from the local map.
                if (weechat_strcasecmp(item_name, "retract") == 0)
                {
                    const char *retract_id = xmpp_stanza_get_id(item);
                    if (!retract_id)
                        continue;

                    account.bookmarks.erase(retract_id);

                    if (auto ch_it = account.channels.find(retract_id); ch_it != account.channels.end())
                    {
                        auto& [_, ch] = *ch_it;
                        if (ch.buffer)
                        {
                            weechat_printf(ch.buffer,
                                           "%sBookmark removed — leaving room",
                                           weechat_prefix("network"));
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
                const std::string nick_text_str = stanza_element_text(nick_elem);
                const char *nick_text = nick_text_str.empty()
                    ? nullptr : nick_text_str.c_str();

                bool do_autojoin = autojoin &&
                    (weechat_strcasecmp(autojoin, "true") == 0 ||
                     weechat_strcasecmp(autojoin, "1") == 0);
                
                // Upsert bookmark
                account.bookmarks[item_id].jid = item_id;
                account.bookmarks[item_id].name = name ? name : "";
                account.bookmarks[item_id].nick = nick_text ? nick_text : "";
                account.bookmarks[item_id].autojoin = do_autojoin;

                // XEP-0492: read notification setting from <extensions><notify>.
                xmpp_stanza_t *extensions_elem = xmpp_stanza_get_child_by_name(
                    conference, "extensions");
                if (extensions_elem)
                {
                    xmpp_stanza_t *notify_elem = xmpp_stanza_get_child_by_name_and_ns(
                        extensions_elem, "notify", "urn:xmpp:notification-settings:1");
                    if (notify_elem)
                    {
                        // Pick the most specific fallback element (no identity attrs).
                        // Priority: <always>, <on-mention>, <never> (fallback without attrs).
                        static constexpr const char *levels[] = { "always", "on-mention", "never" };
                        for (const char *lvl : levels)
                        {
                            xmpp_stanza_t *child = xmpp_stanza_get_children(notify_elem);
                            while (child)
                            {
                                const char *cname = xmpp_stanza_get_name(child);
                                if (cname && weechat_strcasecmp(cname, lvl) == 0
                                    && !xmpp_stanza_get_attribute(child, "identity-category"))
                                {
                                    account.bookmarks[item_id].notify_setting = lvl;
                                    goto xep0492_done;
                                }
                                child = xmpp_stanza_get_next(child);
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
                         std::string cmd = fmt::format("/enter {}{}{}",
                                                       item_id,
                                                       !bookmark.nick.empty() ? "/" : "",
                                                       bookmark.nick);
                         weechat_command(account.buffer, cmd.c_str());
                     }
                    else if (is_biboumi)
                    {
                        weechat_printf(account.buffer,
                                      "%sSkipping autojoin for IRC gateway room: %s",
                                      weechat_prefix("network"), item_id);
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
                            weechat_printf(ch.buffer,
                                           "%sBookmark autojoin disabled — leaving room",
                                           weechat_prefix("network"));
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
        const std::string_view node_sv = items_node ? std::string_view(items_node) : std::string_view{};

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

            for (xmpp_stanza_t *child = xmpp_stanza_get_children(items);
                 child; child = xmpp_stanza_get_next(child))
            {
                const char *child_name = xmpp_stanza_get_name(child);
                if (!child_name)
                    continue;

                if (weechat_strcasecmp(child_name, "item") == 0)
                {
                    has_item = true;

                    // Dedup: skip items we have already rendered (XEP-0060 push)
                    const char *item_id_raw = xmpp_stanza_get_id(child);
                    if (item_id_raw && account.feed_item_seen(feed_key, item_id_raw))
                        continue;

                    // Extract Atom payload (http://www.w3.org/2005/Atom)
                    xmpp_stanza_t *entry = xmpp_stanza_get_child_by_name_and_ns(
                        child, "entry", "http://www.w3.org/2005/Atom");
                    if (!entry)
                        entry = xmpp_stanza_get_child_by_name(child, "entry");

                    xmpp_stanza_t *feed = xmpp_stanza_get_child_by_name_and_ns(
                        child, "feed", "http://www.w3.org/2005/Atom");
                    if (!feed)
                        feed = xmpp_stanza_get_child_by_name(child, "feed");

                    if (!entry && feed)
                    {
                        atom_feed af = parse_atom_feed(account.context, feed);
                        if (!af.empty())
                        {
                            if (!af.title.empty())
                            {
                                feed_ch.update_name(af.title.c_str());
                                weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                    "%sFeed title:%s %s",
                                    weechat_prefix("network"),
                                    weechat_color("reset"),
                                    af.title.c_str());
                            }
                            if (!af.author.empty())
                                weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                    "  %sAuthor:%s %s",
                                    weechat_color("darkgray"),
                                    weechat_color("reset"),
                                    af.author.c_str());
                            if (!af.subtitle.empty())
                                weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                    "  %s", af.subtitle.c_str());
                        }
                        if (item_id_raw)
                            account.feed_item_mark_seen(feed_key, item_id_raw);
                        continue;
                    }

                    const char *publisher = xmpp_stanza_get_attribute(child, "publisher");
                    atom_entry ae = parse_atom_entry(account.context, entry, publisher);
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
                            weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_message",
                                "%s[%s] New item (no content)",
                                weechat_prefix("network"),
                                std::string(node_sv).c_str());
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
}
}
