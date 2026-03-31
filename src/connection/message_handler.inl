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
    std::unique_ptr<char, decltype(&free)> cleartext_g(nullptr, &free);
    char *cleartext = nullptr;
    std::string difftext;
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

    auto stanza_has_user_message_payload = [](xmpp_stanza_t *msg) -> bool
    {
        if (!msg)
            return false;

        xmpp_stanza_t *msg_body = xmpp_stanza_get_child_by_name(msg, "body");
        if (msg_body)
        {
            const char *body_text = xmpp_stanza_get_text_ptr(msg_body);
            if (body_text && *body_text)
                return true;
        }

        xmpp_stanza_t *msg_encrypted = xmpp_stanza_get_child_by_name_and_ns(
            msg, "encrypted", "urn:xmpp:omemo:2");
        if (!msg_encrypted)
        {
            msg_encrypted = xmpp_stanza_get_child_by_name_and_ns(
                msg, "encrypted", "eu.siacs.conversations.axolotl");
        }
        if (msg_encrypted)
        {
            xmpp_stanza_t *payload = xmpp_stanza_get_child_by_name(msg_encrypted, "payload");
            const char *payload_text = payload ? xmpp_stanza_get_text_ptr(payload) : nullptr;
            if (payload_text && *payload_text)
                return true;
        }

        xmpp_stanza_t *msg_pgp = xmpp_stanza_get_child_by_name_and_ns(
            msg, "x", "jabber:x:encrypted");
        if (msg_pgp)
        {
            const char *pgp_text = xmpp_stanza_get_text_ptr(msg_pgp);
            if (pgp_text && *pgp_text)
                return true;
        }

        return false;
    };

    // Payloadless OMEMO stanzas are control/key-transport messages and must
    // not create PM buffers for inactive roster contacts.
    if (body == nullptr && encrypted_without_body && !stanza_has_user_message_payload(stanza))
        return 1;

    if (body == nullptr && !encrypted_without_body && !pgp_without_body)
    {
        topic = xmpp_stanza_get_child_by_name(stanza, "subject");
        if (topic != nullptr)
        {
            intext = xmpp_stanza_get_text(topic);
            type = xmpp_stanza_get_type(stanza);
            if (type != nullptr && strcmp(type, "error") == 0)
                return 1;
            from = xmpp_stanza_get_from(stanza);
            if (from == nullptr)
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
            if (from == nullptr)
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
        if ((sent || received) && forwarded != nullptr)
        {
            // XEP-0280 §8.5: MUST verify the outer stanza's `from` is the
            // account's own bare JID (server-stamped). Drop spoofed carbons.
            const char *carbon_from = xmpp_stanza_get_from(stanza);
            if (!carbon_from)
                return 1;
            xmpp_string_guard carbon_from_bare_g { account.context,
                xmpp_jid_bare(account.context, carbon_from) };
            if (!carbon_from_bare_g.ptr ||
                weechat_strcasecmp(carbon_from_bare_g.ptr, account.jid().data()) != 0)
                return 1;

            xmpp_stanza_t *message = xmpp_stanza_get_child_by_name(forwarded, "message");
            if (message)
                return message_handler(message, false);  // Don't double-count nested stanza
            return 1;
        }

        // XEP-0452: MUC Mention Notifications
        // The server sends a <message> containing:
        //   <addresses xmlns='http://jabber.org/protocol/address'>
        //     <address type='mentioned' jid='room@muc.example.com'/>
        //   </addresses>
        //   <forwarded xmlns='urn:xmpp:forward:0'>...the original groupchat message...</forwarded>
        // Render the forwarded body to the MUC buffer with notify_highlight.
        {
            xmpp_stanza_t *mmn_addrs = xmpp_stanza_get_child_by_name_and_ns(
                stanza, "addresses", "http://jabber.org/protocol/address");
            if (mmn_addrs)
            {
                // Find <address type='mentioned'> child
                std::string muc_jid;
                for (xmpp_stanza_t *addr = xmpp_stanza_get_children(mmn_addrs);
                     addr; addr = xmpp_stanza_get_next(addr))
                {
                    const char *addr_name = xmpp_stanza_get_name(addr);
                    if (!addr_name || weechat_strcasecmp(addr_name, "address") != 0)
                        continue;
                    const char *addr_type = xmpp_stanza_get_attribute(addr, "type");
                    if (!addr_type || weechat_strcasecmp(addr_type, "mentioned") != 0)
                        continue;
                    const char *addr_jid = xmpp_stanza_get_attribute(addr, "jid");
                    if (addr_jid && *addr_jid)
                    {
                        muc_jid = addr_jid;
                        break;
                    }
                }

                if (!muc_jid.empty())
                {
                    xmpp_stanza_t *mmn_fwd = xmpp_stanza_get_child_by_name_and_ns(
                        stanza, "forwarded", "urn:xmpp:forward:0");
                    if (mmn_fwd)
                    {
                        xmpp_stanza_t *mmn_msg = xmpp_stanza_get_child_by_name(mmn_fwd, "message");
                        xmpp_stanza_t *mmn_delay = xmpp_stanza_get_child_by_name_and_ns(
                            mmn_fwd, "delay", "urn:xmpp:delay");
                        if (mmn_msg)
                        {
                            // Ensure MUC channel exists (may not be joined)
                            if (!account.channels.contains(muc_jid))
                                account.channels.emplace(
                                    std::make_pair(muc_jid, weechat::channel{
                                        account, weechat::channel::chat_type::MUC,
                                        muc_jid, muc_jid}));

                            weechat::channel *muc_ch = &account.channels.at(muc_jid);
                            struct t_gui_buffer *muc_buf = muc_ch->buffer;

                            xmpp_stanza_t *mmn_body =
                                xmpp_stanza_get_child_by_name(mmn_msg, "body");
                            xmpp_string_guard mmn_body_text_g(account.context,
                                mmn_body ? xmpp_stanza_get_text(mmn_body) : nullptr);
                            const char *mmn_text = mmn_body_text_g.ptr;

                            const char *mmn_from = xmpp_stanza_get_from(mmn_msg);
                            xmpp_string_guard mmn_nick_g(account.context,
                                mmn_from ? xmpp_jid_resource(account.context, mmn_from)
                                         : nullptr);
                            const char *mmn_nick = mmn_nick_g.ptr
                                ? mmn_nick_g.ptr : "(unknown)";

                            // Parse timestamp from <delay> if present
                            time_t mmn_ts = 0;
                            if (mmn_delay)
                            {
                                const char *stamp =
                                    xmpp_stanza_get_attribute(mmn_delay, "stamp");
                                if (stamp)
                                {
                                    struct tm mmn_tm = {};
                                    strptime(stamp, "%FT%T", &mmn_tm);
                                    mmn_ts = timegm(&mmn_tm);
                                }
                            }

                            if (mmn_text && *mmn_text)
                            {
                                weechat_printf_date_tags(
                                    muc_buf, mmn_ts,
                                    "xmpp_message,notify_highlight,log1",
                                    "%s%s\t%s",
                                    weechat_prefix("action"),
                                    mmn_nick,
                                    mmn_text);
                            }
                        }
                    }
                    return 1;
                }
            }
        }

        result = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "result", "urn:xmpp:mam:2");
        if (result)
        {
            // XEP-0442: pubsub MAM result — forwarded stanza contains a pubsub
            // <event> with the original item payload. Process it here so we can
            // build the correct feed_key from the query context (service JID from
            // our IQ) rather than from the inner message's from= attribute.
            const char *queryid = xmpp_stanza_get_attribute(result, "queryid");
            if (queryid && account.pubsub_mam_queries.count(queryid))
            {
                const auto &pq = account.pubsub_mam_queries.at(queryid);
                std::string feed_service = pq.service;
                std::string node_name    = pq.node;
                std::string feed_key     = fmt::format("{}/{}", feed_service, node_name);

                auto [ch_it, inserted] = account.channels.try_emplace(
                    feed_key,
                    account, weechat::channel::chat_type::FEED,
                    feed_key, feed_key);
                if (inserted)
                    account.feed_open_register(feed_key);
                weechat::channel &feed_ch = ch_it->second;

                forwarded = xmpp_stanza_get_child_by_name_and_ns(
                    result, "forwarded", "urn:xmpp:forward:0");
                if (forwarded)
                {
                    xmpp_stanza_t *fwd_msg = xmpp_stanza_get_child_by_name(forwarded, "message");
                    if (fwd_msg)
                    {
                        // Find the <event> or direct <item> child
                        xmpp_stanza_t *fwd_event = xmpp_stanza_get_child_by_name_and_ns(
                            fwd_msg, "event", "http://jabber.org/protocol/pubsub#event");
                        xmpp_stanza_t *fwd_items = fwd_event
                            ? xmpp_stanza_get_child_by_name(fwd_event, "items") : nullptr;

                        if (fwd_items)
                        {
                            // Extract the publisher (from= of the inner message or publisher= on item)
                            const char *publisher_jid = xmpp_stanza_get_from(fwd_msg);

                            for (xmpp_stanza_t *fwd_item = xmpp_stanza_get_children(fwd_items);
                                 fwd_item; fwd_item = xmpp_stanza_get_next(fwd_item))
                            {
                                const char *child_name = xmpp_stanza_get_name(fwd_item);
                                if (!child_name || weechat_strcasecmp(child_name, "item") != 0)
                                    continue;

                                const char *item_id_raw = xmpp_stanza_get_id(fwd_item);
                                if (item_id_raw && account.feed_item_seen(feed_key, item_id_raw))
                                    continue;

                                xmpp_stanza_t *entry = xmpp_stanza_get_child_by_name_and_ns(
                                    fwd_item, "entry", "http://www.w3.org/2005/Atom");
                                if (!entry)
                                    entry = xmpp_stanza_get_child_by_name(fwd_item, "entry");
                                if (!entry) continue;

                                const char *pub = xmpp_stanza_get_attribute(fwd_item, "publisher");
                                if (!pub) pub = publisher_jid;

                                atom_entry ae = parse_atom_entry(account.context, entry, pub);
                                if (item_id_raw && !ae.item_id.empty())
                                    account.feed_atom_id_set(feed_key, item_id_raw, ae.item_id);
                                if (item_id_raw && !ae.replies_link.empty())
                                    account.feed_replies_link_set(feed_key, item_id_raw, ae.replies_link);

                                int item_alias = -1;
                                if (item_id_raw && *item_id_raw)
                                    item_alias = account.feed_alias_assign(feed_key, item_id_raw);

                                if (ae.empty()) continue;

                                const std::string &title    = ae.title;
                                const std::string &pubdate  = ae.pubdate;
                                const std::string &author   = ae.author;
                                const std::string &reply_to = ae.reply_to;
                                const std::string &via_link = ae.via_link;
                                const std::string &body     = ae.body();
                                const char *pfx  = weechat_prefix("join");
                                const char *bold = weechat_color("bold");
                                const char *rst  = weechat_color("reset");
                                const char *dim  = weechat_color("darkgray");
                                const char *grn  = weechat_color("green");

                                std::string link = ae.link;
                                if (link.empty() && item_id_raw && *item_id_raw)
                                    link = fmt::format("xmpp:{}?;node={};item={}",
                                                       feed_service, node_name, item_id_raw);

                                std::string alias_pfx;
                                if (item_alias > 0)
                                    alias_pfx = fmt::format("#{}", item_alias);

                                if (!title.empty())
                                {
                                    if (!author.empty() && !pubdate.empty())
                                        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_message",
                                            "%s%s%s%s %s%s%s  [%s%s%s] — %s",
                                            pfx,
                                            alias_pfx.empty() ? "" : grn,
                                            alias_pfx.c_str(),
                                            alias_pfx.empty() ? "" : rst,
                                            bold, title.c_str(), rst,
                                            dim, author.c_str(), rst,
                                            pubdate.c_str());
                                    else if (!author.empty())
                                        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_message",
                                            "%s%s%s%s %s%s%s  [%s%s%s]",
                                            pfx,
                                            alias_pfx.empty() ? "" : grn,
                                            alias_pfx.c_str(),
                                            alias_pfx.empty() ? "" : rst,
                                            bold, title.c_str(), rst,
                                            dim, author.c_str(), rst);
                                    else if (!pubdate.empty())
                                        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_message",
                                            "%s%s%s%s %s%s%s — %s",
                                            pfx,
                                            alias_pfx.empty() ? "" : grn,
                                            alias_pfx.c_str(),
                                            alias_pfx.empty() ? "" : rst,
                                            bold, title.c_str(), rst,
                                            pubdate.c_str());
                                    else
                                        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_message",
                                            "%s%s%s%s %s%s%s",
                                            pfx,
                                            alias_pfx.empty() ? "" : grn,
                                            alias_pfx.c_str(),
                                            alias_pfx.empty() ? "" : rst,
                                            bold, title.c_str(), rst);
                                }
                                else
                                {
                                    if (!author.empty() && !pubdate.empty())
                                        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_message",
                                            "%s%s%s%s  [%s%s%s] — %s",
                                            pfx,
                                            alias_pfx.empty() ? "" : grn,
                                            alias_pfx.c_str(),
                                            alias_pfx.empty() ? "" : rst,
                                            dim, author.c_str(), rst,
                                            pubdate.c_str());
                                    else if (!author.empty())
                                        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_message",
                                            "%s%s%s%s  [%s%s%s]",
                                            pfx,
                                            alias_pfx.empty() ? "" : grn,
                                            alias_pfx.c_str(),
                                            alias_pfx.empty() ? "" : rst,
                                            dim, author.c_str(), rst);
                                    else if (!pubdate.empty())
                                        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_message",
                                            "%s%s%s%s  — %s",
                                            pfx,
                                            alias_pfx.empty() ? "" : grn,
                                            alias_pfx.c_str(),
                                            alias_pfx.empty() ? "" : rst,
                                            pubdate.c_str());
                                }

                                if (!reply_to.empty())
                                {
                                    std::string reply_label;
                                    auto item_eq = reply_to.rfind("item=");
                                    if (item_eq != std::string::npos)
                                    {
                                        std::string reply_uuid = reply_to.substr(item_eq + 5);
                                        int ralias = account.feed_alias_lookup(feed_key, reply_uuid);
                                        if (ralias > 0)
                                            reply_label = fmt::format("#{}", ralias);
                                        else
                                            reply_label = reply_uuid;
                                    }
                                    else
                                        reply_label = reply_to;
                                    weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                        "  %sIn reply to:%s %s", dim, rst, reply_label.c_str());
                                }

                                if (!via_link.empty())
                                    weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                        "  %sRepeated from:%s %s", dim, rst, via_link.c_str());

                                if (!link.empty())
                                    weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                        "  %s", link.c_str());

                                if (!ae.replies_link.empty())
                                {
                                    const std::string &comments_ref =
                                        alias_pfx.empty()
                                            ? (item_id_raw ? std::string(item_id_raw) : std::string())
                                            : alias_pfx;
                                    if (!comments_ref.empty())
                                    {
                                        if (ae.comments_count >= 0)
                                            weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                                "  %sComments (%d):%s /feed comments %s",
                                                dim, ae.comments_count, rst, comments_ref.c_str());
                                        else
                                            weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                                "  %sComments:%s /feed comments %s",
                                                dim, rst, comments_ref.c_str());
                                    }
                                }

                                if (!body.empty())
                                {
                                    weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                        "  %s\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500%s",
                                        dim, rst);
                                    std::string_view bv(body);
                                    for (std::string_view::size_type pos = 0;;)
                                    {
                                        auto nl = bv.find('\n', pos);
                                        auto line = bv.substr(pos, nl == std::string_view::npos ? nl : nl - pos);
                                        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                            "  %.*s", static_cast<int>(line.size()), line.data());
                                        if (nl == std::string_view::npos) break;
                                        pos = nl + 1;
                                    }
                                    weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none", "");
                                }

                                if (item_id_raw)
                                    account.feed_item_mark_seen(feed_key, item_id_raw);
                            }
                        }
                    }
                }
                return 1;
            }

            // Regular MAM result (chat/MUC message archive)
            forwarded = xmpp_stanza_get_child_by_name_and_ns(
                result, "forwarded", "urn:xmpp:forward:0");
            if (forwarded != nullptr)
            {
                xmpp_stanza_t *message = xmpp_stanza_get_child_by_name(forwarded, "message");
                if (message)
                {
                    const char *debug_from = xmpp_stanza_get_from(message);
                    const char *debug_to = xmpp_stanza_get_to(message);
                    const char *debug_type = xmpp_stanza_get_type(message);
                    
                    // For global MAM queries, create PM channels based on conversation partners.
                    // We create the channel for any non-groupchat stanza (including receipts,
                    // chat-states, and reactions) — not just messages with a body — so that
                    // contacts whose most-recent archived stanza had no payload (e.g. a read
                    // receipt) still get their buffer restored on reconnect.
                    // Payloadless OMEMO key-transport stanzas are already dropped at the top
                    // of this function (lines ~76-77) before reaching here.
                    if (!debug_type || weechat_strcasecmp(debug_type, "groupchat") != 0)
                    {
                        const char *from_bare = debug_from ? xmpp_jid_bare(account.context, debug_from) : nullptr;
                        const char *to_bare = debug_to ? xmpp_jid_bare(account.context, debug_to) : nullptr;
                        
                        // Determine the conversation partner JID
                        const char *partner_jid = nullptr;
                        if (from_bare && weechat_strcasecmp(from_bare, account.jid().data()) != 0)
                            partner_jid = from_bare;  // Message FROM someone else
                        else if (to_bare && weechat_strcasecmp(to_bare, account.jid().data()) != 0)
                            partner_jid = to_bare;  // Message TO someone else (sent by us)
                        
                        // Create PM channel if it doesn't exist
                        if (partner_jid && !account.channels.contains(partner_jid))
                        {
                            XDEBUG("MAM: discovered conversation with {}", partner_jid);
                            
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
                    char *msg_text = msg_body ? xmpp_stanza_get_text(msg_body) : nullptr;
                    
                    delay = xmpp_stanza_get_child_by_name_and_ns(
                        forwarded, "delay", "urn:xmpp:delay");
                    const char *timestamp_str = delay ? xmpp_stanza_get_attribute(delay, "stamp") : nullptr;
                    
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
                        const char *to_bare = msg_to ? xmpp_jid_bare(account.context, msg_to) : nullptr;
                        
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

                    // XEP-0313: Dedup MAM results against already-displayed live messages.
                    // The <result id='...'> archive ID equals the server-assigned stanza-id
                    // that live delivery tags each buffer line with ("stanza_id_<id>").
                    // If a line with that tag already exists, the message was already shown.
                    const char *archive_id = xmpp_stanza_get_attribute(result, "id");
                    if (archive_id && *archive_id)
                    {
                        // Resolve channel JID from the forwarded message addresses
                        const char *chk_from = msg_from
                            ? xmpp_jid_bare(account.context, msg_from) : nullptr;
                        const char *chk_to   = msg_to
                            ? xmpp_jid_bare(account.context, msg_to)   : nullptr;

                        const char *channel_jid_chk = chk_from;
                        if (chk_to && weechat_strcasecmp(chk_to, account.jid().data()) != 0)
                            channel_jid_chk = chk_to;

                        struct t_gui_buffer *chk_buf = nullptr;
                        if (channel_jid_chk && account.channels.contains(channel_jid_chk))
                            chk_buf = account.channels.at(channel_jid_chk).buffer;

                        bool already_displayed = false;
                        if (chk_buf)
                        {
                            // Build the tag we're looking for
                            std::string needle = std::string("stanza_id_") + archive_id;

                            struct t_hdata *hdata_line      = weechat_hdata_get("line");
                            struct t_hdata *hdata_line_data = weechat_hdata_get("line_data");
                            struct t_gui_lines *own_lines = (struct t_gui_lines*)
                                weechat_hdata_pointer(weechat_hdata_get("buffer"),
                                                      chk_buf, "own_lines");
                            if (own_lines)
                            {
                                struct t_gui_line *ln = (struct t_gui_line*)
                                    weechat_hdata_pointer(hdata_line, own_lines, "last_line");
                                while (ln && !already_displayed)
                                {
                                    struct t_gui_line_data *ld = (struct t_gui_line_data*)
                                        weechat_hdata_pointer(hdata_line, ln, "data");
                                    if (ld)
                                    {
                                        const char *tags = (const char*)
                                            weechat_hdata_string(hdata_line_data, ld, "tags");
                                        if (tags && strstr(tags, needle.c_str()))
                                            already_displayed = true;
                                    }
                                    ln = (struct t_gui_line*)
                                        weechat_hdata_move(hdata_line, ln, -1);
                                }
                            }
                        }

                        if (chk_from) xmpp_free(account.context, (void*)chk_from);
                        if (chk_to)   xmpp_free(account.context, (void*)chk_to);

                        if (already_displayed)
                            return 1;
                    }

                    message = xmpp_stanza_copy(message);
                    if (delay != nullptr)
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
                    XDEBUG("PEP event from {}: {}",
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

                            XDEBUG("omemo: [dbg] PEP devicelist from {} — omemo={}",
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
                                    
                                    XDEBUG("Avatar data received from {} ({} bytes, hash verified: {})",
                                           from_jid,
                                           actual_len,
                                           hash);
                                }
                                
                                xmpp_free(account.context, b64_data);
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

                            auto ch_it = account.channels.find(retract_id);
                            if (ch_it != account.channels.end() && ch_it->second.buffer)
                            {
                                weechat_printf(ch_it->second.buffer,
                                               "%sBookmark removed — leaving room",
                                               weechat_prefix("network"));
                                weechat_buffer_close(ch_it->second.buffer);
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
                        char *nick_text = nullptr;
                        if (nick_elem)
                            nick_text = xmpp_stanza_get_text(nick_elem);

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
                        
                        if (do_autojoin)
                        {
                            // XEP-0402 §4: join immediately on autojoin=true notification.
                            // Skip biboumi (IRC gateway) rooms.
                            bool is_biboumi = (strchr(item_id, '%') != nullptr) ||
                                            (strstr(item_id, "biboumi") != nullptr) ||
                                            (strstr(item_id, "@irc.") != nullptr);
                            
                            if (!is_biboumi)
                            {
                                char **command = weechat_string_dyn_alloc(256);
                                weechat_string_dyn_concat(command, "/enter ", -1);
                                weechat_string_dyn_concat(command, item_id, -1);
                                const char *nick_sv = account.bookmarks[item_id].nick.c_str();
                                if (nick_sv && nick_sv[0])
                                {
                                    weechat_string_dyn_concat(command, "/", -1);
                                    weechat_string_dyn_concat(command, nick_sv, -1);
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
                        else
                        {
                            // XEP-0402 §4 (v1.2.0): autojoin is false (or absent) —
                            // leave the room immediately if we are currently in it.
                            auto ch_it = account.channels.find(item_id);
                            if (ch_it != account.channels.end() && ch_it->second.buffer)
                            {
                                weechat_printf(ch_it->second.buffer,
                                               "%sBookmark autojoin disabled — leaving room",
                                               weechat_prefix("network"));
                                weechat_buffer_close(ch_it->second.buffer);
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
                std::string_view feed_service_sv = from ? std::string_view(from) : std::string_view{};
                std::string_view node_sv          = items_node ? std::string_view(items_node) : std::string_view{};
                // XEP-0472/XEP-0277: urn:xmpp:microblog:0 is the PEP microblog node — treat it
                // as a social feed even though it starts with "urn:".
                // urn:xmpp:microblog:0:comments/<uuid> are comment thread nodes — also feeds.
                bool node_is_microblog = (node_sv == "urn:xmpp:microblog:0")
                    || (node_sv.rfind("urn:xmpp:microblog:0:comments/", 0) == 0);
                bool node_is_uri = !node_sv.empty() && !node_is_microblog && (
                    node_sv.find("://") != std::string_view::npos ||
                    node_sv.substr(0, 4) == "urn:");
                bool from_self = false;
                if (!feed_service_sv.empty())
                {
                    xmpp_string_guard own_bare_g(account.context,
                        xmpp_jid_bare(account.context, account.jid().data()));
                    xmpp_string_guard from_bare_g(account.context,
                        xmpp_jid_bare(account.context, std::string(feed_service_sv).c_str()));
                    from_self = own_bare_g.ptr && from_bare_g.ptr &&
                                weechat_strcasecmp(own_bare_g.ptr, from_bare_g.ptr) == 0;
                }

                if (!node_sv.empty() && !feed_service_sv.empty() && !node_is_uri && !from_self
                    && node_sv != "urn:xmpp:omemo:2:devices"
                    && node_sv != "urn:xmpp:avatar:metadata"
                    && node_sv != "urn:xmpp:avatar:data"
                    && node_sv != "urn:xmpp:bookmarks:1"
                    && node_sv != "urn:xmpp:mds:displayed:0")
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
                    weechat::channel &feed_ch = ch_it->second;
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
                                    xmpp_stanza_t *item_req =
                                        stanza__iq_pubsub_items_item(account.context, nullptr,
                                                                     with_noop(item_id_raw));
                                    xmpp_stanza_t *items_req =
                                        stanza__iq_pubsub_items(account.context, nullptr,
                                                                node_str.c_str());
                                    xmpp_stanza_add_child(items_req, item_req);
                                    xmpp_stanza_release(item_req);
                                    std::array<xmpp_stanza_t *, 2> ch_arr = {items_req, nullptr};
                                    ch_arr[0] = stanza__iq_pubsub(account.context, nullptr,
                                                                  ch_arr.data(),
                                                                  with_noop("http://jabber.org/protocol/pubsub"));
                                    xmpp_string_guard uid_g(account.context,
                                                            xmpp_uuid_gen(account.context));
                                    const char *uid = uid_g.ptr;
                                    ch_arr[0] = stanza__iq(account.context, nullptr, ch_arr.data(),
                                                           nullptr, uid,
                                                           to ? to : account.jid().data(),
                                                           feed_service_str.c_str(), "get");
                                    if (uid)
                                        account.pubsub_fetch_ids[uid] = {feed_service_str,
                                                                          node_str, "", 0};
                                    account.connection.send(ch_arr[0]);
                                    xmpp_stanza_release(ch_arr[0]);
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
                                const std::string &title    = ae.title;
                                const std::string &pubdate  = ae.pubdate;
                                const std::string &author   = ae.author;
                                const std::string &reply_to = ae.reply_to;
                                const std::string &via_link = ae.via_link;
                                const std::string &replies_link = ae.replies_link;
                                const std::string &geoloc = ae.geoloc;
                                const std::string &body = ae.body();
                                const char *pfx = weechat_prefix("join");
                                const char *bold = weechat_color("bold");
                                const char *rst  = weechat_color("reset");
                                const char *dim  = weechat_color("darkgray");
                                const char *grn  = weechat_color("green");

                                // Use Atom <link rel="alternate"> when present; fall back to
                                // the canonical XEP-0060 item URI. Only shown when no alias.
                                std::string link = ae.link;
                                if (link.empty() && item_id_raw && *item_id_raw)
                                    link = fmt::format("xmpp:{}?;node={};item={}",
                                                       feed_service_sv, node_sv, item_id_raw);

                                // Short alias prefix shown before the title, e.g. "#3 ".
                                std::string alias_pfx;
                                if (item_alias > 0)
                                    alias_pfx = fmt::format("#{}", item_alias);

                                // Header line.  When the entry has an explicit title we
                                // display it prominently in bold.  When there is no title
                                // (microblog / social post) we emit only the alias + metadata
                                // so the body line carries the content without duplication.
                                if (!title.empty())
                                {
                                    if (!author.empty() && !pubdate.empty())
                                        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_message",
                                            "%s%s%s%s %s%s%s  [%s%s%s] — %s",
                                            pfx,
                                            alias_pfx.empty() ? "" : grn,
                                            alias_pfx.c_str(),
                                            alias_pfx.empty() ? "" : rst,
                                            bold, title.c_str(), rst,
                                            dim, author.c_str(), rst,
                                            pubdate.c_str());
                                    else if (!author.empty())
                                        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_message",
                                            "%s%s%s%s %s%s%s  [%s%s%s]",
                                            pfx,
                                            alias_pfx.empty() ? "" : grn,
                                            alias_pfx.c_str(),
                                            alias_pfx.empty() ? "" : rst,
                                            bold, title.c_str(), rst,
                                            dim, author.c_str(), rst);
                                    else if (!pubdate.empty())
                                        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_message",
                                            "%s%s%s%s %s%s%s — %s",
                                            pfx,
                                            alias_pfx.empty() ? "" : grn,
                                            alias_pfx.c_str(),
                                            alias_pfx.empty() ? "" : rst,
                                            bold, title.c_str(), rst,
                                            pubdate.c_str());
                                    else
                                        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_message",
                                            "%s%s%s%s %s%s%s",
                                            pfx,
                                            alias_pfx.empty() ? "" : grn,
                                            alias_pfx.c_str(),
                                            alias_pfx.empty() ? "" : rst,
                                            bold, title.c_str(), rst);
                                }
                                else
                                {
                                    // No title: metadata-only header line.
                                    if (!author.empty() && !pubdate.empty())
                                        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_message",
                                            "%s%s%s%s  [%s%s%s] — %s",
                                            pfx,
                                            alias_pfx.empty() ? "" : grn,
                                            alias_pfx.c_str(),
                                            alias_pfx.empty() ? "" : rst,
                                            dim, author.c_str(), rst,
                                            pubdate.c_str());
                                    else if (!author.empty())
                                        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_message",
                                            "%s%s%s%s  [%s%s%s]",
                                            pfx,
                                            alias_pfx.empty() ? "" : grn,
                                            alias_pfx.c_str(),
                                            alias_pfx.empty() ? "" : rst,
                                            dim, author.c_str(), rst);
                                    else if (!pubdate.empty())
                                        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_message",
                                            "%s%s%s%s  — %s",
                                            pfx,
                                            alias_pfx.empty() ? "" : grn,
                                            alias_pfx.c_str(),
                                            alias_pfx.empty() ? "" : rst,
                                            pubdate.c_str());
                                    // else: body line still follows below.
                                }

                                if (!reply_to.empty())
                                {
                                    // Extract item UUID from xmpp: URI (item=<uuid>) and
                                    // resolve to an alias if possible; fall back to UUID.
                                    std::string reply_label;
                                    auto item_eq = reply_to.rfind("item=");
                                    if (item_eq != std::string::npos)
                                    {
                                        std::string reply_uuid = reply_to.substr(item_eq + 5);
                                        int ralias = account.feed_alias_lookup(feed_key, reply_uuid);
                                        if (ralias > 0)
                                            reply_label = fmt::format("#{}", ralias);
                                        else
                                            reply_label = reply_uuid;
                                    }
                                    else
                                        reply_label = reply_to;
                                    weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                        "  %sIn reply to:%s %s",
                                        dim, rst, reply_label.c_str());
                                }

                                if (!via_link.empty())
                                    weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                        "  %sRepeated from:%s %s",
                                        dim, rst, via_link.c_str());

                                if (!link.empty())
                                    weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                        "  %s", link.c_str());

                                // Comments: suppress raw URI — user uses /feed comments #N or
                                // /feed comments <item-id> when no alias is assigned yet.
                                if (!replies_link.empty())
                                {
                                    const std::string &comments_ref =
                                        alias_pfx.empty()
                                            ? (item_id_raw ? std::string(item_id_raw) : std::string())
                                            : alias_pfx;
                                    if (!comments_ref.empty())
                                    {
                                        if (ae.comments_count >= 0)
                                            weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                                "  %sComments (%d):%s /feed comments %s",
                                                dim, ae.comments_count, rst, comments_ref.c_str());
                                        else
                                            weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                                "  %sComments:%s /feed comments %s",
                                                dim, rst, comments_ref.c_str());
                                    }
                                }

                                if (!ae.categories.empty())
                                {
                                    std::string tags;
                                    for (size_t i = 0; i < ae.categories.size(); ++i)
                                    {
                                        if (i) tags += ", ";
                                        tags += ae.categories[i];
                                    }
                                    weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                        "  %sTags:%s %s",
                                        dim, rst, tags.c_str());
                                }

                                for (const auto &enclosure : ae.enclosures)
                                    weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                        "  %sAttachment:%s %s",
                                        dim, rst, enclosure.c_str());

                                for (const auto &att : ae.attachments)
                                {
                                    bool is_image = !att.media_type.empty() &&
                                                    att.media_type.rfind("image/", 0) == 0;
                                    bool is_video = !att.media_type.empty() &&
                                                    att.media_type.rfind("video/", 0) == 0;
                                    std::string kind_str = (att.disposition == "attachment") ? "File"
                                                         : is_image ? "Image"
                                                         : is_video ? "Video"
                                                         : "Media";
                                    std::string size_str;
                                    if (att.size > 0)
                                    {
                                        if (att.size >= 1024*1024)
                                            size_str = fmt::format("{:.1f} MB", att.size / (1024.0*1024.0));
                                        else if (att.size >= 1024)
                                            size_str = fmt::format("{:.1f} KB", att.size / 1024.0);
                                        else
                                            size_str = fmt::format("{} B", att.size);
                                    }
                                    std::string meta;
                                    if (!att.media_type.empty()) meta += att.media_type;
                                    if (!size_str.empty()) meta += (meta.empty() ? "" : ", ") + size_str;
                                    weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                        "  %s[%s: %s%s] %s%s",
                                        dim, kind_str.c_str(), att.filename.c_str(),
                                        meta.empty() ? "" : (" (" + meta + ")").c_str(),
                                        att.url.c_str(), rst);
                                }

                                if (!geoloc.empty())
                                    weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                        "  %sLocation:%s %s",
                                        dim, rst, geoloc.c_str());

                                if (!body.empty())
                                {
                                    weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                        "  %s\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500%s",
                                        dim, rst);
                                    std::string_view bv(body);
                                    for (std::string_view::size_type pos = 0;;)
                                    {
                                        auto nl = bv.find('\n', pos);
                                        auto line = bv.substr(pos, nl == std::string_view::npos ? nl : nl - pos);
                                        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                            "  %.*s", static_cast<int>(line.size()), line.data());
                                        if (nl == std::string_view::npos) break;
                                        pos = nl + 1;
                                    }
                                    weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none", "");
                                }
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
                        std::array<xmpp_stanza_t *, 2> ch_arr = {nullptr, nullptr};
                        ch_arr[0] = stanza__iq_pubsub_items(account.context, nullptr,
                                                            node_str.c_str());
                        ch_arr[0] = stanza__iq_pubsub(account.context, nullptr,
                                                      ch_arr.data(),
                                                      with_noop("http://jabber.org/protocol/pubsub"));
                        xmpp_string_guard uid_g(account.context, xmpp_uuid_gen(account.context));
                        const char *uid = uid_g.ptr;
                        ch_arr[0] = stanza__iq(account.context, nullptr, ch_arr.data(),
                                               nullptr, uid,
                                               to ? to : account.jid().data(),
                                               feed_service_str.c_str(), "get");
                        if (uid)
                            account.pubsub_fetch_ids[uid] = {feed_service_str, node_str, "", 0};
                        account.connection.send(ch_arr[0]);
                        xmpp_stanza_release(ch_arr[0]);
                    }
                }
            }
        }

        // XEP-0184: Message Delivery Receipts — handle incoming <received> from others
        // Helper: walk the buffer backwards and update the status glyph on the line
        // tagged id_<acked_id> (strips any existing glyph suffix, appends new_glyph).
        auto update_line_glyph = [](weechat::channel *ch,
                                    const char       *acked_id,
                                    std::string_view  new_glyph)
        {
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
                    bool found = false;
                    for (int n = 0; n < tags_count && !found; n++)
                    {
                        auto str_tag = fmt::format("{}|tags_array", n);
                        const char *tag = weechat_hdata_string(hdata_line_data, line_data, str_tag.c_str());
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
                        new_msg += new_glyph;
                        struct t_hashtable *ht = weechat_hashtable_new(4,
                            WEECHAT_HASHTABLE_STRING, WEECHAT_HASHTABLE_STRING, nullptr, nullptr);
                        weechat_hashtable_set(ht, "message", new_msg.c_str());
                        weechat_hdata_update(hdata_line_data, line_data, ht);
                        weechat_hashtable_free(ht);
                        break;
                    }
                }
                last_line = weechat_hdata_pointer(hdata_line, last_line, "prev_line");
            }
        };

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
                        update_line_glyph(ch, acked_id, " ✓");
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
                        update_line_glyph(ch, acked_id, " ✓✓");
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
    if (type != nullptr && strcmp(type, "error") == 0)
        return 1;
    from = xmpp_stanza_get_from(stanza);
    if (from == nullptr)
        return 1;
    from_bare = xmpp_jid_bare(account.context, from);
    xmpp_string_guard from_bare_main_g { account.context, const_cast<char*>(from_bare) };
    to = xmpp_stanza_get_to(stanza);
    if (to == nullptr)
        to = account.jid().data();
    to_bare = to ? xmpp_jid_bare(account.context, to) : nullptr;
    xmpp_string_guard to_bare_main_g { account.context, const_cast<char*>(to_bare) };
    const bool is_self_outbound_copy = from_bare && to_bare
        && weechat_strcasecmp(from_bare, account.jid().data()) == 0
        && weechat_strcasecmp(to_bare, account.jid().data()) != 0;
    id = xmpp_stanza_get_id(stanza);
    thread = xmpp_stanza_get_attribute(stanza, "thread");
    
    // XEP-0359: Unique and Stable Stanza IDs
    xmpp_stanza_t *stanza_id_elem = xmpp_stanza_get_child_by_name_and_ns(stanza, "stanza-id", "urn:xmpp:sid:0");
    const char *stanza_id = stanza_id_elem ? xmpp_stanza_get_attribute(stanza_id_elem, "id") : nullptr;
    const char *stanza_id_by = stanza_id_elem ? xmpp_stanza_get_attribute(stanza_id_elem, "by") : nullptr;
    
    xmpp_stanza_t *origin_id_elem = xmpp_stanza_get_child_by_name_and_ns(stanza, "origin-id", "urn:xmpp:sid:0");
    const char *origin_id = origin_id_elem ? xmpp_stanza_get_attribute(origin_id_elem, "id") : nullptr;
    
    // Prefer stanza-id over origin-id over regular id for stable message identification
    const char *stable_id = stanza_id ? stanza_id : (origin_id ? origin_id : id);
    
    replace = xmpp_stanza_get_child_by_name_and_ns(stanza, "replace",
                                                   "urn:xmpp:message-correct:0");
    replace_id = replace ? xmpp_stanza_get_id(replace) : nullptr;
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

    // XEP-0333 §5 Business Rules: MUST NOT send Displayed Markers for outgoing
    // messages we sent (received back via carbons or MAM).
    if (id && (markable || request) && !is_self_outbound_copy)
    {
        weechat::channel::unread unread_val;
        unread_val.id = id;
        unread_val.thread = thread ? std::optional<std::string>(thread) : std::nullopt;
        // XEP-0490: preserve server stanza-id for correct MDS PEP publish
        unread_val.stanza_id = stanza_id ? std::optional<std::string>(stanza_id) : std::nullopt;
        unread_val.stanza_id_by = stanza_id_by ? std::optional<std::string>(stanza_id_by) : std::nullopt;
        auto unread = &unread_val;

        // XEP-0334: receipt/marker replies MUST NOT be stored.
        // Use the incoming message type so routing is correct.
        xmpp_stanza_t *message = xmpp_message_new(account.context,
                                                  type,  // "chat" or "groupchat"
                                                  channel->id.data(), nullptr);

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

        // XEP-0334: receipt/marker replies MUST NOT be stored
        {
            xmpp_stanza_t *no_store = xmpp_stanza_new(account.context);
            xmpp_stanza_set_name(no_store, "no-store");
            xmpp_stanza_set_ns(no_store, "urn:xmpp:hints");
            xmpp_stanza_add_child(message, no_store);
            xmpp_stanza_release(no_store);
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

    // XEP-0450 / XEP-0434: Automatic Trust Management — unencrypted trust messages.
    // <trust-message xmlns='urn:xmpp:tm:1' usage='urn:xmpp:atm:1' encryption='urn:xmpp:omemo:2'>
    //   <key-owner jid='...'><trust>BASE64</trust></key-owner>
    // </trust-message>
    {
        xmpp_stanza_t *trust_msg = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "trust-message", "urn:xmpp:tm:1");
        if (trust_msg
            && account.omemo
            && weechat::config::instance
            && weechat_config_boolean(weechat::config::instance->look.omemo_atm))
        {
            const char *usage = xmpp_stanza_get_attribute(trust_msg, "usage");
            const char *encryption = xmpp_stanza_get_attribute(trust_msg, "encryption");
            if (usage && std::string_view(usage) == "urn:xmpp:atm:1"
                && encryption && std::string_view(encryption) == "urn:xmpp:omemo:2")
            {
                // Walk <key-owner> children
                xmpp_stanza_t *ko = xmpp_stanza_get_children(trust_msg);
                while (ko)
                {
                    const char *ko_name = xmpp_stanza_get_name(ko);
                    const char *ko_jid = ko_name && std::string_view(ko_name) == "key-owner"
                        ? xmpp_stanza_get_attribute(ko, "jid") : nullptr;
                    if (ko_jid)
                    {
                        xmpp_stanza_t *decision = xmpp_stanza_get_children(ko);
                        while (decision)
                        {
                            const char *dname = xmpp_stanza_get_name(decision);
                            if (dname && (std::string_view(dname) == "trust"
                                          || std::string_view(dname) == "distrust"))
                            {
                                const char *fp = xmpp_stanza_get_text_ptr(decision);
                                if (fp && *fp)
                                {
                                    const std::string level =
                                        (std::string_view(dname) == "trust") ? "trusted" : "distrusted";
                                    account.omemo.store_atm_trust_pub(ko_jid, fp, level);
                                }
                            }
                            decision = xmpp_stanza_get_next(decision);
                        }
                    }
                    ko = xmpp_stanza_get_next(ko);
                }
            }
            // Trust messages must not be displayed; consume silently.
            return 1;
        }
    }

    encrypted = xmpp_stanza_get_child_by_name_and_ns(stanza, "encrypted",
                                                     "urn:xmpp:omemo:2");
    if (!encrypted)
    {
        // Narrow compatibility fallback for legacy OMEMO clients.
        encrypted = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "encrypted", "eu.siacs.conversations.axolotl");
    }

    // Record that this peer actively speaks OMEMO — but only for genuine
    // inbound encrypted messages (not self-outbound copies or plaintext).
    // This gate controls whether we will proactively fetch their devicelist
    // and establish a session; widening it caused spurious OMEMO initiation
    // toward contacts who send plaintext (the session was bootstrapped from
    // MAM traffic rather than from an actual inbound OMEMO message).
    if (account.omemo && encrypted && !is_self_outbound_copy
        && channel && channel->type == weechat::channel::chat_type::PM)
    {
        account.omemo.note_peer_traffic(account.context, channel->id);
    }

    x = xmpp_stanza_get_child_by_name_and_ns(stanza, "x", "jabber:x:encrypted");
    
    // XEP-0380: Explicit Message Encryption
    xmpp_stanza_t *eme = xmpp_stanza_get_child_by_name_and_ns(stanza, "encryption",
                                                                "urn:xmpp:eme:0");
    const char *eme_namespace = eme ? xmpp_stanza_get_attribute(eme, "namespace") : nullptr;
    const char *eme_name = eme ? xmpp_stanza_get_attribute(eme, "name") : nullptr;
    
    intext = body ? xmpp_stanza_get_text(body) : nullptr;

    if (encrypted && is_self_outbound_copy)
    {
        if (intext)
            xmpp_free(account.context, intext);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        intext = xmpp_strdup(account.context, OMEMO_ADVICE);
#pragma GCC diagnostic pop
    }

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
    if (encrypted && !is_self_outbound_copy
        && channel && channel->type == weechat::channel::chat_type::PM && !channel->omemo.enabled)
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
        cleartext_g.reset(cleartext);
        if (!cleartext)
        {
            if (is_self_outbound_copy)
                goto message_handler_after_omemo;

            xmpp_stanza_t *payload = xmpp_stanza_get_child_by_name(encrypted, "payload");
            const char *payload_text = payload ? xmpp_stanza_get_text_ptr(payload) : nullptr;
            if (!payload || !payload_text || !*payload_text)
                return 1;

            weechat_printf_date_tags(channel->buffer, 0, "notify_none", "%s%s (%s)",
                                     weechat_prefix("error"), "OMEMO Decryption Error", from);
            return 1;
        }
    }
    else
    {
        if (encrypted && !is_self_outbound_copy)
            weechat_printf(nullptr, "%sOMEMO: encrypted message but account.omemo is nullptr/false",
                           weechat_prefix("error"));
    }
message_handler_after_omemo:
    if (x)
    {
        char *ciphertext = xmpp_stanza_get_text(x);
        if (auto decrypted = account.pgp.decrypt(channel->buffer, ciphertext)) {
            cleartext = strdup(decrypted->c_str());
            cleartext_g.reset(cleartext);
        }
        xmpp_free(account.context, ciphertext);
    }
    text = cleartext ? cleartext : intext;

    // XEP-0428 / XEP-0461: Fallback Indication handling.
    // When <fallback xmlns='urn:xmpp:fallback:0'> is present the <body> may
    // embed a legacy compatibility quote prefix that must be stripped.
    xmpp_stanza_t *fallback_elem = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "fallback", "urn:xmpp:fallback:0");
    // Trimmed body storage — must outlive `text` usage below.
    std::string trimmed_body;
    if (fallback_elem && text && !replace)
    {
        xmpp_stanza_t *reactions = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "reactions", "urn:xmpp:reactions:0");
        xmpp_stanza_t *retract = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "retract", "urn:xmpp:message-retract:1");
        if (!retract)
            retract = xmpp_stanza_get_child_by_name_and_ns(
                stanza, "retract", "urn:xmpp:message-retract:0");
        xmpp_stanza_t *reply_fb = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "reply", "urn:xmpp:reply:0");
        xmpp_stanza_t *apply_to = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "apply-to", "urn:xmpp:fasten:0");

        if (reactions || retract || apply_to)
        {
            // These handlers render the full content themselves; drop body.
            text = nullptr;
        }
        else if (reply_fb)
        {
            // XEP-0461 §3.1: strip the fallback quote prefix from the body.
            // The <fallback> child <body start="0" end="N"/> gives the byte
            // range to remove.  After stripping, skip any leading whitespace
            // (blank lines that separate the quote from the reply text).
            xmpp_stanza_t *fb_body = xmpp_stanza_get_child_by_name(fallback_elem, "body");
            const char *end_attr = fb_body
                ? xmpp_stanza_get_attribute(fb_body, "end") : nullptr;
            if (end_attr)
            {
                long end = std::strtol(end_attr, nullptr, 10);
                std::string_view sv(text);
                if (end > 0 && static_cast<std::size_t>(end) < sv.size())
                {
                    sv.remove_prefix(static_cast<std::size_t>(end));
                    // Skip blank lines / leading whitespace between quote and reply
                    auto first_non_ws = sv.find_first_not_of(" \t\r\n");
                    if (first_non_ws != std::string_view::npos)
                        sv.remove_prefix(first_non_ws);
                    trimmed_body = std::string(sv);
                    text = trimmed_body.empty() ? nullptr : trimmed_body.c_str();
                }
                // If end >= body length, the whole body was the fallback quote;
                // nothing useful to display (reply_prefix provides the excerpt).
                else if (end > 0)
                    text = nullptr;
            }
            // No <body end=...> attribute — leave text as-is; better to show
            // something than nothing.
        }
    }

    // XEP-0382: Spoiler Messages — display hint before the body
    xmpp_stanza_t *spoiler_elem = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "spoiler", "urn:xmpp:spoiler:0");
    const char *spoiler_hint = spoiler_elem ? xmpp_stanza_get_text(spoiler_elem) : nullptr;

    // XEP-0466: Ephemeral Messages — detect timer value
    xmpp_stanza_t *ephemeral_elem = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "ephemeral", "urn:xmpp:ephemeral:0");
    long ephemeral_timer = 0;
    if (ephemeral_elem)
    {
        const char *timer_attr = xmpp_stanza_get_attribute(ephemeral_elem, "timer");
        if (timer_attr)
        {
            char *endp = nullptr;
            long v = std::strtol(timer_attr, &endp, 10);
            if (endp && *endp == '\0' && v > 0)
                ephemeral_timer = v;
        }
    }

    if (replace)
    {
        std::unique_ptr<char, decltype(&free)> orig_guard(nullptr, &free);
        char *orig = nullptr;
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
                    std::string str_tag;
                    for (int n_tag = 0; n_tag < tags_count; n_tag++)
                    {
                        str_tag = fmt::format("{}|tags_array", n_tag);
                        const char *tag = weechat_hdata_string(weechat_hdata_get("line_data"),
                                                               line_data, str_tag.c_str());
                        if (strlen(tag) > strlen("id_") &&
                            weechat_strcasecmp(tag+strlen("id_"), replace_id) == 0)
                        {
                            // XEP-0308 §3: Verify the correcting sender matches
                            // the original message author before applying the edit.
                            // Compute expected nick tag value (same logic as display path).
                            const char *corr_nick = from;
                            xmpp_string_guard corr_resource_g { account.context,
                                (channel && channel->type == weechat::channel::chat_type::MUC)
                                    ? xmpp_jid_resource(account.context, from)
                                    : nullptr };
                            if (corr_resource_g.ptr)
                                corr_nick = corr_resource_g.ptr;

                            bool sender_matches = false;
                            for (int chk = 0; chk < tags_count; chk++)
                            {
                                str_tag = fmt::format("{}|tags_array", chk);
                                const char *chk_tag = weechat_hdata_string(
                                    weechat_hdata_get("line_data"), line_data, str_tag.c_str());
                                if (strlen(chk_tag) > strlen("nick_") &&
                                    weechat_strcasecmp(chk_tag + strlen("nick_"), corr_nick) == 0)
                                {
                                    sender_matches = true;
                                    break;
                                }
                            }
                            if (!sender_matches)
                                break;  // Silently drop spoofed correction

                            auto arraylist_deleter = [](struct t_arraylist *al) {
                                weechat_arraylist_free(al);
                            };
                            std::unique_ptr<struct t_arraylist, decltype(arraylist_deleter)>
                                orig_lines_ptr(weechat_arraylist_new(0, 0, 0, nullptr, nullptr, nullptr, nullptr),
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
                                    line_data = nullptr;

                                msg = nullptr;
                                if (line_data)
                                {
                                    tags_count = weechat_hdata_integer(weechat_hdata_get("line_data"),
                                                                       line_data, "tags_count");
                                    for (n_tag = 0; n_tag < tags_count; n_tag++)
                                    {
                                        str_tag = fmt::format("{}|tags_array", n_tag);
                                        tag = weechat_hdata_string(weechat_hdata_get("line_data"),
                                                                   line_data, str_tag.c_str());
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
                            orig_guard.reset(orig);
                            weechat_string_dyn_free(orig_message, 0); // free_string=0: keep buffer, ownership transferred to orig_guard
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
                std::unique_ptr<void, decltype(&free)> ses_guard(result.ses, &free);
                std::unique_ptr<void, decltype(&free)> lcs_guard(result.lcs, &free);
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
                difftext = *visual;
                weechat_string_dyn_free(visual, 1);
            }
        }
    }

    // XEP-0425: Message Moderation (extends XEP-0424)
    // Look for <apply-to xmlns='urn:xmpp:fasten:0'><moderate xmlns='urn:xmpp:message-moderate:1'>
    xmpp_stanza_t *apply_to = xmpp_stanza_get_child_by_name_and_ns(stanza, "apply-to",
                                                                    "urn:xmpp:fasten:0");
    const char *moderate_id = nullptr;
    const char *moderate_reason = nullptr;
    
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
                        std::string str_tag;
                        for (int n_tag = 0; n_tag < tags_count; n_tag++)
                        {
                            str_tag = fmt::format("{}|tags_array", n_tag);
                            const char *tag = weechat_hdata_string(weechat_hdata_get("line_data"),
                                                                   line_data, str_tag.c_str());
                            if (strlen(tag) > strlen("id_") &&
                                weechat_strcasecmp(tag+strlen("id_"), moderate_id) == 0)
                            {
                                // Found the message to moderate - update it with tombstone
                                std::string tombstone = moderate_reason
                                    ? fmt::format("{}[Message moderated: {}]{}",
                                                  weechat_color("darkgray"),
                                                  moderate_reason,
                                                  weechat_color("resetcolor"))
                                    : fmt::format("{}[Message moderated by room moderator]{}",
                                                  weechat_color("darkgray"),
                                                  weechat_color("resetcolor"));
                                
                                // Update the line with tombstone
                                struct t_hashtable *hashtable = weechat_hashtable_new(8,
                                    WEECHAT_HASHTABLE_STRING,
                                    WEECHAT_HASHTABLE_STRING,
                                    nullptr, nullptr);
                                weechat_hashtable_set(hashtable, "message", tombstone.c_str());
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
    const char *retract_id = retract ? xmpp_stanza_get_attribute(retract, "id") : nullptr;
    
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
                    std::string str_tag;
                    for (int n_tag = 0; n_tag < tags_count; n_tag++)
                    {
                        str_tag = fmt::format("{}|tags_array", n_tag);
                        const char *tag = weechat_hdata_string(weechat_hdata_get("line_data"),
                                                               line_data, str_tag.c_str());
                        if (strlen(tag) > strlen("id_") &&
                            weechat_strcasecmp(tag+strlen("id_"), retract_id) == 0)
                        {
                            // Found the message to retract - update it with tombstone
                            // Create tombstone text
                            auto tombstone = fmt::format("{}[Message deleted]{}",
                                                         weechat_color("darkgray"),
                                                         weechat_color("resetcolor"));
                            
                            // Update the line with tombstone
                            struct t_hashtable *hashtable = weechat_hashtable_new(8,
                                WEECHAT_HASHTABLE_STRING,
                                WEECHAT_HASHTABLE_STRING,
                                nullptr, nullptr);
                            weechat_hashtable_set(hashtable, "message", tombstone.c_str());
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
    const char *reactions_id = reactions ? xmpp_stanza_get_attribute(reactions, "id") : nullptr;
    
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
                        std::string str_tag;
                        for (int n_tag = 0; n_tag < tags_count; n_tag++)
                        {
                            str_tag = fmt::format("{}|tags_array", n_tag);
                            const char *tag = weechat_hdata_string(weechat_hdata_get("line_data"),
                                                                   line_data, str_tag.c_str());
                            if (strlen(tag) > strlen("id_") &&
                                weechat_strcasecmp(tag+strlen("id_"), reactions_id) == 0)
                            {
                                // Found the message.
                                // XEP-0444: a new <reactions> from a sender REPLACES their
                                // previous reaction set — do not accumulate. Strip any
                                // previously-appended reaction blocks (our format:
                                // " <blue>[…]<reset>") before appending the new set.
                                const char *orig_message = weechat_hdata_string(
                                    weechat_hdata_get("line_data"), line_data, "message");

                                std::string base(orig_message ? orig_message : "");
                                // The reaction suffix we append starts with " " + weechat_color("blue") + "["
                                static const std::string rxn_prefix =
                                    std::string(" ") + weechat_color("blue") + "[";
                                auto rxn_pos = base.find(rxn_prefix);
                                if (rxn_pos != std::string::npos)
                                    base.resize(rxn_pos);

                                std::string new_message = base
                                    + " " + weechat_color("blue")
                                    + "[" + emojis + "]"
                                    + weechat_color("resetcolor");

                                // Update the line with reaction replaced
                                struct t_hashtable *hashtable = weechat_hashtable_new(8,
                                    WEECHAT_HASHTABLE_STRING,
                                    WEECHAT_HASHTABLE_STRING,
                                    nullptr, nullptr);
                                weechat_hashtable_set(hashtable, "message", new_message.c_str());
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
    std::string nick_str; // owns the nick string when extracted from a scoped guard
    if (weechat_strcasecmp(type, "groupchat") == 0)
    {
        xmpp_string_guard gc_bare_g { account.context, xmpp_jid_bare(account.context, from) };
        xmpp_string_guard gc_resource_g { account.context, xmpp_jid_resource(account.context, from) };
        // Use case-insensitive compare: JIDs are case-insensitive per RFC 6122
        if (gc_bare_g.ptr && weechat_strcasecmp(channel->name.c_str(), gc_bare_g.ptr) == 0
            && gc_resource_g.ptr && *gc_resource_g.ptr)
        {
            nick_str = gc_resource_g.ptr; // copy into surviving std::string
            nick = nick_str.c_str();
        }
        // else nick stays as `from` (the full JID)
        display_from = from;
    }
    else if (parent_channel && parent_channel->type == weechat::channel::chat_type::MUC)
    {
        xmpp_string_guard muc_resource_g { account.context, xmpp_jid_resource(account.context, from) };
        // Use case-insensitive compare: JIDs are case-insensitive per RFC 6122
        if (weechat_strcasecmp(channel->name.c_str(), from) == 0
            && muc_resource_g.ptr && *muc_resource_g.ptr)
        {
            nick_str = muc_resource_g.ptr;
            nick = nick_str.c_str();
        }
        // else nick stays as `from`
        display_from = from;
    }
    delay = xmpp_stanza_get_child_by_name_and_ns(stanza, "delay", "urn:xmpp:delay");
    timestamp = delay ? xmpp_stanza_get_attribute(delay, "stamp") : nullptr;
    const char *delay_from = delay ? xmpp_stanza_get_attribute(delay, "from") : nullptr;
    if (timestamp)
    {
        strptime(timestamp, "%FT%T", &time);
        date = timegm(&time);
    }

    // XEP-0313 dedup for MUC history: when the MUC server re-delivers old messages
    // via <delay> on rejoin, suppress any message whose stanza-id is already tagged
    // in the buffer.  This prevents duplicates on every reconnect.
    if (delay && stanza_id && channel && channel->buffer)
    {
        std::string needle = std::string("stanza_id_") + stanza_id;
        struct t_hdata *hdata_line      = weechat_hdata_get("line");
        struct t_hdata *hdata_line_data = weechat_hdata_get("line_data");
        struct t_gui_lines *own_lines   = (struct t_gui_lines*)
            weechat_hdata_pointer(weechat_hdata_get("buffer"),
                                  channel->buffer, "own_lines");
        bool already_shown = false;
        if (own_lines)
        {
            struct t_gui_line *ln = (struct t_gui_line*)
                weechat_hdata_pointer(hdata_line, own_lines, "last_line");
            while (ln && !already_shown)
            {
                struct t_gui_line_data *ld = (struct t_gui_line_data*)
                    weechat_hdata_pointer(hdata_line, ln, "data");
                if (ld)
                {
                    const char *tags = (const char*)
                        weechat_hdata_string(hdata_line_data, ld, "tags");
                    if (tags && strstr(tags, needle.c_str()))
                        already_shown = true;
                }
                ln = (struct t_gui_line*)
                    weechat_hdata_move(hdata_line, ln, -1);
            }
        }
        if (already_shown)
            return 1;
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
    // Store stanza-id metadata if present.
    // The stanza_id_ tag is written whenever the server provides a stanza-id so
    // that the MAM dedup logic (lines ~297-354) can suppress the replayed copy of
    // a message that was already shown via live delivery.  stanza_id_by_ is
    // supplementary and only written when the server also supplies the by= attribute.
    if (stanza_id)
    {
        weechat_string_dyn_concat(dyn_tags, ",stanza_id_", -1);
        weechat_string_dyn_concat(dyn_tags, stanza_id, -1);
        if (stanza_id_by)
        {
            weechat_string_dyn_concat(dyn_tags, ",stanza_id_by_", -1);
            weechat_string_dyn_concat(dyn_tags, stanza_id_by, -1);
        }
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

    // Detect self-messages so /edit, /retract, etc. can find them.
    // For MUC: the server echoes our message back — the sender nick is our own nick.
    // For PM carbons/echoes: from_bare matches our own JID.
    bool is_from_self = false;
    if (weechat_strcasecmp(type, "groupchat") == 0)
    {
        // nick was set to the resource (occupant nick) above; compare to our config nick.
        const std::string_view our_nick = account.nickname();
        if (nick && !our_nick.empty()
            && weechat_strcasecmp(nick, our_nick.data()) == 0)
            is_from_self = true;
    }
    else if (from_bare && weechat_strcasecmp(from_bare, account.jid().data()) == 0)
    {
        is_from_self = true;
    }
    if (is_from_self)
        weechat_string_dyn_concat(dyn_tags, ",self_msg", -1);

    if (weechat_string_match(text, "/me *", 0))
        weechat_string_dyn_concat(dyn_tags, ",xmpp_action", -1);
    if (replace)
    {
        weechat_string_dyn_concat(dyn_tags, ",edit", -1);
        weechat_string_dyn_concat(dyn_tags, ",replace_", -1);
        weechat_string_dyn_concat(dyn_tags, replace_id, -1);
    }

    if (date != 0 || encrypted || is_from_self)
    {
        weechat_string_dyn_concat(dyn_tags, ",notify_none", -1);
        // Self-messages in MUC are still real messages and should be logged.
        if (is_from_self && !date && !encrypted)
            weechat_string_dyn_concat(dyn_tags, ",log1", -1);
    }
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
        weechat_printf_date_tags(channel->buffer, date, nullptr, "%s%sTransport: %s",
                                 weechat_prefix("network"), weechat_color("gray"),
                                 channel::transport_name(channel->transport));
    }
    else if (!x && !encrypted && text == intext && channel->transport != weechat::channel::transport::PLAIN)
    {
        channel->transport = weechat::channel::transport::PLAIN;
        weechat_printf_date_tags(channel->buffer, date, nullptr, "%s%sTransport: %s",
                                 weechat_prefix("network"), weechat_color("gray"),
                                 channel::transport_name(channel->transport));
    }
    // For groupchat messages, display_from is the full occupant JID (room/nick).
    // Look up the user by full JID to get their avatar/color, but always format
    // the prefix using nick (the resource) to avoid showing the full JID.
    std::string display_prefix;
    if (weechat_strcasecmp(type, "groupchat") == 0 && nick && *nick)
    {
        auto *display_user = user::search(&account, display_from);
        if (display_user)
        {
            std::string pfx;
            if (!display_user->profile.avatar_rendered.empty())
                pfx = display_user->profile.avatar_rendered + " ";
            pfx += user::as_prefix_raw(nick);
            display_prefix = pfx;
        }
        else
        {
            display_prefix = user::as_prefix_raw(nick);
        }
    }
    else
    {
        display_prefix = user::as_prefix_raw(&account, display_from);
        if (display_prefix.empty())
            display_prefix = user::as_prefix_raw(nick && *nick ? nick : from_bare);
    }
    
    // XEP-0461: Message Replies - extract reply context
    xmpp_stanza_t *reply_elem = xmpp_stanza_get_child_by_name_and_ns(stanza, "reply", "urn:xmpp:reply:0");
    const char *reply_to_id = reply_elem ? xmpp_stanza_get_attribute(reply_elem, "id") : nullptr;
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
                    std::string str_tag;
                    for (int n_tag = 0; n_tag < tags_count; n_tag++)
                    {
                        str_tag = fmt::format("{}|tags_array", n_tag);
                        const char *tag = weechat_hdata_string(weechat_hdata_get("line_data"),
                                                               line_data, str_tag.c_str());
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

                                // Strip embedded color codes so we get plain text
                                char *plain_text = weechat_string_remove_color(msg_text, nullptr);
                                const char *clean_text = plain_text ? plain_text : msg_text;

                                // Skip any leading reply prefix(es) (↪ …) from a prior reply chain.
                                // UTF-8 encoding of ↪ is 3 bytes: e2 86 aa.
                                // The rendered prefix format is "↪ <excerpt> " so we find the last
                                // ↪ in the string and jump past it and its excerpt word(s).
                                // Simpler: repeatedly consume "↪ <everything up to next ↪ or end>"
                                // by scanning forward for the next ↪ occurrence.
                                {
                                    const char arrow[] = "\xE2\x86\xAA"; // ↪
                                    const char *next = strstr(clean_text, arrow);
                                    while (next)
                                    {
                                        // advance past this ↪ and look for another one
                                        const char *after = next + 3;
                                        while (*after == ' ') after++;
                                        const char *further = strstr(after, arrow);
                                        if (further)
                                        {
                                            // there is another ↪ ahead — skip to it
                                            clean_text = further;
                                            next = further;
                                        }
                                        else
                                        {
                                            // this was the last ↪; skip past its excerpt to the real body
                                            // The excerpt ends at the space before the actual message.
                                            // Our format: "↪ <excerpt> <body>" — excerpt has no trailing ↪.
                                            // We can't reliably split excerpt from body here, so just
                                            // use `after` as the start of content (skips "↪ ").
                                            clean_text = after;
                                            break;
                                        }
                                    }
                                }

                                // Decide whether to truncate to a 40-char excerpt.
                                // Truncate if: more than 5 newlines, OR any single line
                                // (segment between newlines) exceeds 200 characters.
                                bool do_truncate = false;
                                {
                                    int newline_count = 0;
                                    int line_len = 0;
                                    for (const char *p = clean_text; *p; ++p)
                                    {
                                        if (*p == '\n')
                                        {
                                            ++newline_count;
                                            line_len = 0;
                                        }
                                        else
                                        {
                                            ++line_len;
                                        }
                                        if (newline_count > 5 || line_len > 200)
                                        {
                                            do_truncate = true;
                                            break;
                                        }
                                    }
                                }

                                std::string excerpt = (do_truncate && strlen(clean_text) > 40)
                                    ? std::string(clean_text, 40) + "..."
                                    : std::string(clean_text);

                                if (plain_text) free(plain_text);
                                
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
                char *desc_text = desc_elem ? xmpp_stanza_get_text(desc_elem) : nullptr;
                
                // Format: [URL: url] or [URL: description (url)]
                if (desc_text && strlen(desc_text) > 0)
                {
                    oob_suffix = fmt::format(" {}[URL: {} ({})]{}",
                                            weechat_color("blue"),
                                            desc_text, url_text,
                                            weechat_color("resetcolor"));
                }
                else
                {
                    oob_suffix = fmt::format(" {}[URL: {}]{}",
                                            weechat_color("blue"),
                                            url_text,
                                            weechat_color("resetcolor"));
                }
                
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

    // XEP-0447: Stateless File Sharing (SFS)
    // Parse <file-sharing xmlns='urn:xmpp:sfs:0'> — preferred by Conversations ≥2.10 / Dino / Gajim.
    // Deduplicate with the SIMS block: if we already built a sims_suffix for the same URL, skip.
    for (xmpp_stanza_t *fs = xmpp_stanza_get_children(stanza);
         fs; fs = xmpp_stanza_get_next(fs))
    {
        const char *fs_name = xmpp_stanza_get_name(fs);
        const char *fs_ns   = xmpp_stanza_get_ns(fs);
        if (!fs_name || !fs_ns) continue;
        if (strcmp(fs_name, "file-sharing") != 0
            || strcmp(fs_ns, "urn:xmpp:sfs:0") != 0) continue;

        // <file xmlns='urn:xmpp:file:metadata:0'>
        xmpp_stanza_t *file_elem = xmpp_stanza_get_child_by_name_and_ns(
            fs, "file", "urn:xmpp:file:metadata:0");

        std::string sfs_name, sfs_mime, sfs_size_str;
        if (file_elem)
        {
            xmpp_stanza_t *name_e = xmpp_stanza_get_child_by_name(file_elem, "name");
            xmpp_stanza_t *mime_e = xmpp_stanza_get_child_by_name(file_elem, "media-type");
            xmpp_stanza_t *size_e = xmpp_stanza_get_child_by_name(file_elem, "size");

            if (name_e) { char *t = xmpp_stanza_get_text(name_e); if (t) { sfs_name = t; xmpp_free(account.context, t); } }
            if (mime_e) { char *t = xmpp_stanza_get_text(mime_e); if (t) { sfs_mime = t; xmpp_free(account.context, t); } }
            if (size_e) { char *t = xmpp_stanza_get_text(size_e); if (t) { sfs_size_str = t; xmpp_free(account.context, t); } }
        }

        // <sources><url-data xmlns='http://jabber.org/protocol/url-data' target='https://...'/>
        xmpp_stanza_t *sources = xmpp_stanza_get_child_by_name(fs, "sources");
        std::string sfs_url;
        bool sfs_encrypted = false; // true when source is XEP-0448 encrypted
        if (sources)
        {
            for (xmpp_stanza_t *src = xmpp_stanza_get_children(sources);
                 src; src = xmpp_stanza_get_next(src))
            {
                const char *sname = xmpp_stanza_get_name(src);
                const char *sns   = xmpp_stanza_get_ns(src);
                if (!sname) continue;

                // XEP-0448: <encrypted xmlns='urn:xmpp:esfs:0'>
                if (strcmp(sname, "encrypted") == 0 && sns
                    && strcmp(sns, "urn:xmpp:esfs:0") == 0)
                {
                    // Extract key, iv, and inner url-data target.
                    std::string esfs_key, esfs_iv, esfs_ct_url;
                    xmpp_stanza_t *key_el = xmpp_stanza_get_child_by_name(src, "key");
                    xmpp_stanza_t *iv_el  = xmpp_stanza_get_child_by_name(src, "iv");
                    if (key_el) { char *t = xmpp_stanza_get_text(key_el); if (t) { esfs_key = t; xmpp_free(account.context, t); } }
                    if (iv_el)  { char *t = xmpp_stanza_get_text(iv_el);  if (t) { esfs_iv  = t; xmpp_free(account.context, t); } }

                    xmpp_stanza_t *inner_sources = xmpp_stanza_get_child_by_name(src, "sources");
                    if (inner_sources)
                    {
                        for (xmpp_stanza_t *isrc = xmpp_stanza_get_children(inner_sources);
                             isrc && esfs_ct_url.empty(); isrc = xmpp_stanza_get_next(isrc))
                        {
                            const char *iname = xmpp_stanza_get_name(isrc);
                            if (iname && strcmp(iname, "url-data") == 0)
                            {
                                const char *target = xmpp_stanza_get_attribute(isrc, "target");
                                if (target) esfs_ct_url = target;
                            }
                        }
                    }

                    if (!esfs_key.empty() && !esfs_iv.empty() && !esfs_ct_url.empty())
                    {
                        // Kick off background download + decrypt.
                        esfs_start_download(esfs_ct_url, sfs_name, esfs_key, esfs_iv,
                                            channel ? channel->buffer : account.buffer);
                        sfs_encrypted = true;
                        sfs_url = esfs_ct_url; // use ciphertext URL for dedup check
                    }
                    break; // prefer encrypted source
                }

                if (!sfs_url.empty()) continue; // already have a plain URL

                if (strcmp(sname, "url-data") == 0)
                {
                    const char *target = xmpp_stanza_get_attribute(src, "target");
                    if (target) sfs_url = target;
                }
                else if (strcmp(sname, "reference") == 0)
                {
                    const char *uri = xmpp_stanza_get_attribute(src, "uri");
                    if (uri) sfs_url = uri;
                }
            }
        }

        // Skip if SIMS already covered this URL
        if (!sfs_url.empty() && !sims_suffix.empty()
            && sims_suffix.find(sfs_url) != std::string::npos)
            continue;
        // Also skip if the OOB suffix already covers it
        if (!sfs_url.empty() && !oob_suffix.empty()
            && oob_suffix.find(sfs_url) != std::string::npos)
        {
            oob_suffix.clear(); // show the richer SFS line instead
        }

        if (!sfs_url.empty())
        {
            if (sfs_encrypted)
            {
                // XEP-0448: show that we're downloading the encrypted file in the background.
                sims_suffix += std::string("\n") + weechat_color("cyan") + "[Encrypted file: ";
                sims_suffix += sfs_name.empty() ? "(unnamed)" : sfs_name;
                if (!sfs_size_str.empty())
                {
                    long long sz = std::stoll(sfs_size_str);
                    if (sz >= 1024 * 1024)
                        sims_suffix += fmt::format(" ({:.1f} MB)", sz / 1048576.0);
                    else if (sz >= 1024)
                        sims_suffix += fmt::format(" ({:.1f} KB)", sz / 1024.0);
                    else
                        sims_suffix += fmt::format(" ({} B)", sz);
                }
                sims_suffix += " — downloading…]" + std::string(weechat_color("resetcolor"));
            }
            else
            {
                sims_suffix += std::string("\n") + weechat_color("cyan") + "[File: ";
                if (!sfs_name.empty())
                    sims_suffix += sfs_name;
                else
                    sims_suffix += sfs_url;

                if (!sfs_mime.empty() || !sfs_size_str.empty())
                {
                    sims_suffix += " (";
                    if (!sfs_mime.empty())
                        sims_suffix += sfs_mime;
                    if (!sfs_mime.empty() && !sfs_size_str.empty())
                        sims_suffix += ", ";
                    if (!sfs_size_str.empty())
                    {
                        long long sz = std::stoll(sfs_size_str);
                        if (sz >= 1024 * 1024)
                            sims_suffix += fmt::format("{:.1f} MB", sz / 1048576.0);
                        else if (sz >= 1024)
                            sims_suffix += fmt::format("{:.1f} KB", sz / 1024.0);
                        else
                            sims_suffix += fmt::format("{} B", sz);
                    }
                    sims_suffix += ")";
                }
                sims_suffix += " " + sfs_url;
                sims_suffix += "]" + std::string(weechat_color("resetcolor"));
            }
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
    if (text && difftext.empty() && !has_unstyled)  // Don't style diffs (already styled)
    {
        // XEP-0394: Message Markup takes precedence over XEP-0393 ad-hoc styling.
        styled_text = apply_xep394_markup(stanza, text);
        if (styled_text.empty())
            styled_text = apply_xep393_styling(text);
        display_text = styled_text.c_str();
    }
    else if (!difftext.empty())
    {
        display_text = difftext.c_str();
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

    // XEP-0466: Ephemeral Messages — prepend timer indicator before the body
    if (ephemeral_timer > 0)
    {
        std::string eph_prefix = std::string(weechat_color("magenta"))
            + "[⏱ " + std::to_string(ephemeral_timer) + "s] "
            + weechat_color("resetcolor");
        if (final_text.empty())
            final_text = display_text ? display_text : "";
        final_text = eph_prefix + final_text;
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

    const char *encrypted_glyph = (encrypted || x) ? "🔒 " : "";

    if (channel_id == from_bare && to == channel->id)
        weechat_printf_date_tags(channel->buffer, date, *dyn_tags, "%s%s\t[to %s]: %s%s",
                                 edit, display_prefix.data(),
                                 to, encrypted_glyph,
                                 display_text ? display_text : "");
    else if (weechat_string_match(text, "/me *", 0))
        weechat_printf_date_tags(channel->buffer, date, *dyn_tags, "%s%s\t%s %s%s",
                                 edit, weechat_prefix("action"), display_prefix.data(),
                                 encrypted_glyph,
                                 !difftext.empty() ? difftext.c_str()+4 : display_text ? display_text+4 : "");
    else
        weechat_printf_date_tags(channel->buffer, date, *dyn_tags, "%s%s\t%s%s",
                                 edit, display_prefix.data(),
                                 encrypted_glyph,
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

    // XEP-0466: schedule tombstone timer if this was an ephemeral message.
    // Timer starts when the message is displayed (now), fires after ephemeral_timer seconds.
    if (ephemeral_timer > 0 && stable_id && *stable_id)
    {
        g_ephemeral_pending.push_back({ channel->buffer, std::string(stable_id) });
        weechat_hook_timer(ephemeral_timer * 1000, 0, 1,
                           ephemeral_tombstone_cb, &g_ephemeral_pending.back(), nullptr);
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

    std::unique_ptr<char, decltype(&free)> client_name(
            weechat_string_eval_expression(
                "weechat ${info:version}", nullptr, nullptr, nullptr),
            &free);
    char **serial = weechat_string_dyn_alloc(256);
    weechat_string_dyn_concat(serial, "client/pc//", -1);
    weechat_string_dyn_concat(serial, client_name.get(), -1);
    weechat_string_dyn_concat(serial, "<", -1);

    xmpp_stanza_t *identity = xmpp_stanza_new(account.context);
    xmpp_stanza_set_name(identity, "identity");
    xmpp_stanza_set_attribute(identity, "category", "client");
    xmpp_stanza_set_attribute(identity, "name", client_name.get());
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
        "jabber:iq:register",
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
        "urn:xmpp:markup:0",
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
        "urn:ietf:params:xml:ns:vcard-4.0",
        // XEP-0472: Pubsub Social Feed (Experimental)
        "urn:xmpp:pubsub-social-feed:1",
        // XEP-0277: receive microblog PEP push events from contacts
        "urn:xmpp:microblog:0+notify",
        // XEP-0442: Pubsub Message Archive Management
        "urn:xmpp:mam:2#pubsub",
        // XEP-0413: Order-By for MAM queries
        "urn:xmpp:order-by:1",
        // XEP-0466: Ephemeral Messages
        "urn:xmpp:ephemeral:0",
        // XEP-0448: Encrypted File Sharing
        "urn:xmpp:esfs:0",
        // XEP-0434: Trust Messages (used by XEP-0450 ATM)
        "urn:xmpp:tm:1",
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

    // Add a single-value x-data field and append it to the XEP-0115 serial
    auto add_feature1 = [&](const char *var, const char *type, const char *val) {
        xmpp_stanza_t *f = stanza_make_field(account.context, var, val, type);
        xmpp_stanza_add_child(x, f);
        xmpp_stanza_release(f);
        if (strcmp(var, "FORM_TYPE") == 0) {
            weechat_string_dyn_concat(serial, var, -1);
            weechat_string_dyn_concat(serial, "<", -1);
        }
        weechat_string_dyn_concat(serial, val, -1);
        weechat_string_dyn_concat(serial, "<", -1);
    };
    // Add a two-value x-data field and append both values to the serial
    auto add_feature2 = [&](const char *var, const char *type,
                            const char *val1, const char *val2) {
        xmpp_stanza_t *field = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(field, "field");
        xmpp_stanza_set_attribute(field, "var", var);
        xmpp_stanza_set_attribute(field, "type", type);
        for (const char *v : {val1, val2}) {
            xmpp_stanza_t *value = xmpp_stanza_new(account.context);
            xmpp_stanza_set_name(value, "value");
            xmpp_stanza_t *text = xmpp_stanza_new(account.context);
            xmpp_stanza_set_text(text, v);
            xmpp_stanza_add_child(value, text);
            xmpp_stanza_release(text);
            xmpp_stanza_add_child(field, value);
            xmpp_stanza_release(value);
        }
        xmpp_stanza_add_child(x, field);
        xmpp_stanza_release(field);
        weechat_string_dyn_concat(serial, var,  -1);
        weechat_string_dyn_concat(serial, "<",  -1);
        weechat_string_dyn_concat(serial, val1, -1);
        weechat_string_dyn_concat(serial, "<",  -1);
        weechat_string_dyn_concat(serial, val2, -1);
        weechat_string_dyn_concat(serial, "<",  -1);
    };

    add_feature1("FORM_TYPE", "hidden", "urn:xmpp:dataforms:softwareinfo");
    add_feature2("ip_version", "text-multi", "ipv4", "ipv6");
    add_feature1("os",               nullptr, osinfo.sysname);
    add_feature1("os_version",       nullptr, osinfo.release);
    add_feature1("software",         nullptr, "weechat");
    add_feature1("software_version", nullptr, weechat_info_get("version", nullptr));

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
    const char *title = title_elem ? xmpp_stanza_get_text_ptr(title_elem) : nullptr;
    xmpp_stanza_t *instr_elem = xmpp_stanza_get_child_by_name(x_form, "instructions");
    const char *instr = instr_elem ? xmpp_stanza_get_text_ptr(instr_elem) : nullptr;

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
