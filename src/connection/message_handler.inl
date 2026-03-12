bool weechat::connection::message_handler(xmpp_stanza_t *stanza, bool top_level)
{
    // SM counter incremented in libstrophe wrapper, not here
    // top_level parameter kept for nested/recursive calls

    (void) top_level;
    append_raw_xml_trace(account, "RECV", stanza);

    weechat::channel *channel, *parent_channel;
    xmpp_stanza_t *x, *body, *delay, *topic, *replace, *request, *markable, *composing, *sent, *received, *result, *forwarded, *event, *items, *item, *list, *encrypted;
    const char *type, *from, *nick, *from_bare, *to, *to_bare, *id, *thread, *replace_id, *timestamp;
    const char *text = nullptr;
    xmpp_string_guard intext_g { account.context, nullptr };
    char *&intext = intext_g.ptr;
    struct free_guard { char *ptr; ~free_guard() { if (ptr) ::free(ptr); } };
    free_guard cleartext_g { nullptr };
    char *&cleartext = cleartext_g.ptr;
    free_guard difftext_g { nullptr };
    char *&difftext = difftext_g.ptr;
    struct tm time = {0};
    time_t date = 0;

    auto binding = xml::message(account.context, stanza);
    body = xmpp_stanza_get_child_by_name(stanza, "body");
    xmpp_stanza_t *encrypted_without_body = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "encrypted", "urn:xmpp:omemo:2");
    if (!encrypted_without_body)
    {
        encrypted_without_body = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "encrypted", "eu.siacs.conversations.axolotl");
    }
    xmpp_stanza_t *pgp_without_body = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "x", "jabber:x:encrypted");

    if (body == NULL && !encrypted_without_body && !pgp_without_body)
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
            xmpp_string_guard from_bare_g { account.context, xmpp_jid_bare(account.context, from) };
            xmpp_string_guard from_resource_g { account.context, xmpp_jid_resource(account.context, from) };
            from_bare = from_bare_g.ptr;
            from = from_resource_g.ptr;
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
            xmpp_string_guard cs_from_bare_g { account.context, xmpp_jid_bare(account.context, from) };
            xmpp_string_guard cs_nick_g { account.context, xmpp_jid_resource(account.context, from) };
            from_bare = cs_from_bare_g.ptr;
            nick = cs_nick_g.ptr;
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
            xmpp_stanza_t *message = xmpp_stanza_get_child_by_name(forwarded, "message");
            if (message)
                return message_handler(message, false);  // Don't double-count nested stanza
            return 1;
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
                                          "urn:xmpp:omemo:2:devices") == 0)
                {
                    item = xmpp_stanza_get_child_by_name(items, "item");
                    if (item)
                    {
                        list = xmpp_stanza_get_child_by_name_and_ns(
                            item, "devices", "urn:xmpp:omemo:2");
                        if (list)
                        {
                            if (account.omemo)
                            {
                                account.omemo.handle_devicelist(
                                    &account, from ? from : account.jid().data(), items);
                            }

                            weechat_printf(account.buffer,
                                           "%somemo: [dbg] PEP devicelist from %s — omemo=%s",
                                           weechat_prefix("network"),
                                           from ? from : account.jid().data(),
                                           account.omemo ? "ready" : "NOT ready");
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
                                     bool hash_changed = true;
                                     if (user)
                                     {
                                         hash_changed = (user->profile.avatar_hash != info_id);
                                         user->profile.avatar_hash = info_id;
                                     }

                                     // Only fetch/load data when hash is new or user was unknown
                                     if (hash_changed || (user && user->profile.avatar_data.empty()))
                                     {
                                         weechat_printf_date_tags(account.buffer, 0, "xmpp_avatar",
                                                                 "%sAvatar update from %s (hash: %.8s..., type: %s, bytes: %s)",
                                                                 weechat_prefix("network"),
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
    xmpp_string_guard from_bare_main_g { account.context, const_cast<char*>(from_bare) };
    to = xmpp_stanza_get_to(stanza);
    if (to == NULL)
        to = account.jid().data();
    to_bare = to ? xmpp_jid_bare(account.context, to) : NULL;
    xmpp_string_guard to_bare_main_g { account.context, const_cast<char*>(to_bare) };
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

    if (account.omemo && channel && channel->type == weechat::channel::chat_type::PM)
    {
        account.omemo.note_peer_traffic(account.context, channel->id);
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
            xmpp_string_guard invite_from_bare_g { account.context,
                from ? xmpp_jid_bare(account.context, from) : nullptr };
            from_bare = invite_from_bare_g.ptr ? invite_from_bare_g.ptr : "unknown";
            
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
                                                     "urn:xmpp:omemo:2");
    if (!encrypted)
    {
        // Narrow compatibility fallback for legacy OMEMO clients.
        encrypted = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "encrypted", "eu.siacs.conversations.axolotl");
    }
    
    x = xmpp_stanza_get_child_by_name_and_ns(stanza, "x", "jabber:x:encrypted");
    
    // XEP-0380: Explicit Message Encryption
    xmpp_stanza_t *eme = xmpp_stanza_get_child_by_name_and_ns(stanza, "encryption",
                                                                "urn:xmpp:eme:0");
    const char *eme_namespace = eme ? xmpp_stanza_get_attribute(eme, "namespace") : NULL;
    const char *eme_name = eme ? xmpp_stanza_get_attribute(eme, "name") : NULL;
    
    intext = body ? xmpp_stanza_get_text(body) : nullptr;

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
        char *orig = NULL;
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
                            auto arraylist_deleter = [](struct t_arraylist *al) {
                                weechat_arraylist_free(al);
                            };
                            std::unique_ptr<struct t_arraylist, decltype(arraylist_deleter)>
                                orig_lines_ptr(weechat_arraylist_new(0, 0, 0, NULL, NULL, NULL, NULL),
                                               arraylist_deleter);
                            struct t_arraylist *orig_lines = orig_lines_ptr.get();
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
                            weechat_string_dyn_free(orig_message, 1); // transfers ownership of *orig_message to orig
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
        free(orig);
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
        return 1;
    }

    nick = from;
    const char *display_from = from_bare;
    if (weechat_strcasecmp(type, "groupchat") == 0)
    {
        xmpp_string_guard gc_bare_g { account.context, xmpp_jid_bare(account.context, from) };
        xmpp_string_guard gc_resource_g { account.context, xmpp_jid_resource(account.context, from) };
        nick = channel->name == gc_bare_g.ptr
            ? gc_resource_g.ptr
            : from;
        display_from = from;
    }
    else if (parent_channel && parent_channel->type == weechat::channel::chat_type::MUC)
    {
        xmpp_string_guard muc_resource_g { account.context, xmpp_jid_resource(account.context, from) };
        nick = channel->name == from
            ? muc_resource_g.ptr
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
        else if (strstr(eme_namespace, "omemo"))
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

    // XEP-0334: Message Processing Hints — if the sender marks the message
    // as <no-store> or <no-permanent-store>, add the WeeChat no_log tag so
    // it is not written to the buffer log file.
    {
        bool no_store = xmpp_stanza_get_child_by_name_and_ns(
                            stanza, "no-store", "urn:xmpp:hints") != nullptr
                     || xmpp_stanza_get_child_by_name_and_ns(
                            stanza, "no-permanent-store", "urn:xmpp:hints") != nullptr;
        if (no_store)
            weechat_string_dyn_concat(dyn_tags, ",no_log", -1);
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

xmpp_stanza_t *weechat::connection::get_caps(xmpp_stanza_t *reply, char **hash, const char *node)
{
    xmpp_stanza_t *query = xmpp_stanza_new(account.context);
    xmpp_stanza_set_name(query, "query");
    xmpp_stanza_set_ns(query, "http://jabber.org/protocol/disco#info");
    if (node && *node)
        xmpp_stanza_set_attribute(query, "node", node);

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

    const std::vector<std::string_view> advertised_features {
        "urn:xmpp:omemo:2",
        "urn:xmpp:omemo:2:devices+notify",
        "http://jabber.org/protocol/caps",
        "http://jabber.org/protocol/chatstates",
        "http://jabber.org/protocol/disco#info",
        "http://jabber.org/protocol/disco#items",
        "http://jabber.org/protocol/muc",
        "http://jabber.org/protocol/nick+notify",
        "http://jabber.org/protocol/pep",
        "jabber:iq:version",
        "jabber:x:conference",
        "jabber:x:oob",
        "storage:bookmarks",
        "urn:xmpp:avatar:metadata+notify",
        "urn:xmpp:chat-markers:0",
        "urn:xmpp:delay",
        "urn:xmpp:hints",
        "urn:xmpp:idle:1",
        "urn:xmpp:message-correct:0",
        "urn:xmpp:message-retract:1",
        "urn:xmpp:message-moderate:1",
        "urn:xmpp:fasten:0",
        "urn:xmpp:reactions:0",
        "urn:xmpp:reply:0",
        "urn:xmpp:sid:0",
        "urn:xmpp:styling:0",
        "urn:xmpp:eme:0",
        "http://jabber.org/protocol/mood",
        "http://jabber.org/protocol/mood+notify",
        "http://jabber.org/protocol/activity",
        "http://jabber.org/protocol/activity+notify",
        "urn:xmpp:blocking",
        "urn:xmpp:bookmarks:1",
        "urn:xmpp:bookmarks:1+notify",
        "urn:xmpp:carbons:2",
        "urn:xmpp:ping",
        "urn:xmpp:receipts",
        "urn:xmpp:time",
        "urn:xmpp:attention:0",
        "urn:xmpp:spoiler:0",
        "urn:xmpp:fallback:0",
        "vcard-temp:x:update",
        "urn:xmpp:reference:0",
        "http://jabber.org/protocol/commands",
        "urn:xmpp:mam:2",
        "urn:xmpp:mds:displayed:0",
        "urn:xmpp:mds:displayed:0+notify",
        "urn:xmpp:channel-search:0:search",
        "urn:ietf:params:xml:ns:vcard-4.0"
    };

    std::vector<std::string> sorted_features;
    sorted_features.reserve(advertised_features.size());
    for (const auto feature : advertised_features)
        sorted_features.emplace_back(feature);
    std::sort(sorted_features.begin(), sorted_features.end());

    xmpp_stanza_t *feature = nullptr;
    for (const auto &ns : sorted_features)
    {
        feature = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(feature, "feature");
        xmpp_stanza_set_attribute(feature, "var", ns.c_str());
        xmpp_stanza_add_child(query, feature);
        xmpp_stanza_release(feature);

        // XEP-0115 hash input must use sorted features.
        weechat_string_dyn_concat(serial, ns.c_str(), -1);
        weechat_string_dyn_concat(serial, "<", -1);
    }

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

