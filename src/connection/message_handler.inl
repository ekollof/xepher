bool weechat::connection::message_handler(xmpp_stanza_t *stanza, bool top_level, bool is_mam_replay)
{
    // SM counter incremented in libstrophe wrapper, not here
    // top_level parameter kept for nested/recursive calls

    (void) top_level;
    append_raw_xml_trace(account, "RECV", stanza);

    // Cache hdata handles — stable for the process lifetime, resolved once.
    static struct t_hdata *hdata_line      = weechat_hdata_get("line");
    static struct t_hdata *hdata_line_data = weechat_hdata_get("line_data");
    static struct t_hdata *hdata_lines     = weechat_hdata_get("lines");
    static struct t_hdata *hdata_buffer    = weechat_hdata_get("buffer");

    weechat::channel *channel, *parent_channel;
    xmpp_stanza_t *x, *body, *delay, *topic, *replace, *request, *markable, *composing, *sent, *received, *result, *forwarded, *event, *items, *item, *encrypted;
    const char *type, *from, *nick, *from_bare, *to, *to_bare, *id, *thread, *replace_id, *timestamp;
    const char *text = nullptr;
    xmpp_string_guard intext_g { account.context, nullptr };
    char *&intext = intext_g.ptr;
    std::string from_bare_main_storage; // owns main from_bare string
    std::string to_bare_main_storage;   // owns main to_bare string
     std::string omemo_cleartext_storage; // owns OMEMO-decrypted text
     std::string pgp_cleartext_storage; // owns PGP-decrypted text (avoids strdup)
     char *cleartext = nullptr;
    struct tm time = {0};
    time_t date = 0;

    auto binding = xml::message(account.context, stanza);
    body = xmpp_stanza_get_child_by_name(stanza, "body");
    xmpp_stanza_t *encrypted_without_body = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "encrypted", "eu.siacs.conversations.axolotl");
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
            msg, "encrypted", "eu.siacs.conversations.axolotl");
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
        if (type != nullptr && std::string_view(type) == "error")
            return 1;
        from = xmpp_stanza_get_from(stanza);
        if (from == nullptr)
            return 1;
        std::string from_bare_s = ::jid(nullptr, from).bare;
            std::string from_resource_s = ::jid(nullptr, from).resource;
            from_bare = from_bare_s.c_str();
            from = from_resource_s.c_str();
            { auto it = account.channels.find(from_bare); channel = it != account.channels.end() ? &it->second : nullptr; }
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
            std::string cs_from_bare_s = ::jid(nullptr, from).bare;
            std::string cs_nick_s = ::jid(nullptr, from).resource;
            from_bare = cs_from_bare_s.c_str();
            nick = cs_nick_s.c_str();
            { auto it = account.channels.find(from_bare); channel = it != account.channels.end() ? &it->second : nullptr; }
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
            std::string carbon_from_bare_s = ::jid(nullptr, carbon_from).bare;
            if (carbon_from_bare_s.empty() ||
                weechat_strcasecmp(carbon_from_bare_s.c_str(), account.jid().data()) != 0)
                return 1;

            xmpp_stanza_t *message = xmpp_stanza_get_child_by_name(forwarded, "message");
            if (message)
                return message_handler(message, false);  // Don't double-count nested stanza
            return 1;
        }

        // XEP-0452: MUC Mention Notifications (v0.2.x wire format)
        // The server sends a <message from='room@muc.example.com'> containing:
        //   <mentions xmlns='urn:xmpp:mmn:0'>
        //     <forwarded xmlns='urn:xmpp:forward:0'>...the original groupchat message...</forwarded>
        //   </mentions>
        // Security (§4 MUST): outer from= must match the from= of the forwarded message.
        {
            xmpp_stanza_t *mmn_elem = xmpp_stanza_get_child_by_name_and_ns(
                stanza, "mentions", "urn:xmpp:mmn:0");
            if (mmn_elem)
            {
                // The outer message from= is the MUC JID that sent us the notification.
                const char *outer_from = xmpp_stanza_get_from(stanza);
                std::string muc_jid = outer_from ? ::jid(nullptr, outer_from).bare : std::string{};

                if (!muc_jid.empty())
                {
                    // <forwarded> is a direct child of <mentions>, not of <message>
                    xmpp_stanza_t *mmn_fwd = xmpp_stanza_get_child_by_name_and_ns(
                        mmn_elem, "forwarded", "urn:xmpp:forward:0");
                    if (mmn_fwd)
                    {
                        xmpp_stanza_t *mmn_msg = xmpp_stanza_get_child_by_name(mmn_fwd, "message");
                        xmpp_stanza_t *mmn_delay = xmpp_stanza_get_child_by_name_and_ns(
                            mmn_fwd, "delay", "urn:xmpp:delay");
                        if (mmn_msg)
                        {
                            // §4 MUST: forwarded message from= bare JID must match muc_jid
                            const char *inner_from_raw = xmpp_stanza_get_from(mmn_msg);
                            if (!inner_from_raw)
                                return 1;
                            std::string inner_muc = ::jid(nullptr, inner_from_raw).bare;
                            if (weechat_strcasecmp(inner_muc.c_str(), muc_jid.c_str()) != 0)
                                return 1;

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
                            std::string mmn_nick_s = mmn_from ? ::jid(nullptr, mmn_from).resource : std::string{};
                            const char *mmn_nick = !mmn_nick_s.empty()
                                ? mmn_nick_s.c_str() : "(unknown)";

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
                    // Only create a channel if the archived stanza contains actual user content
                    // (a plaintext body, an OMEMO encrypted payload, or a PGP payload).
                    // This prevents pure control stanzas — OMEMO key-transport messages,
                    // chat-state notifications, read receipts — from spuriously opening PM
                    // buffers for contacts who haven't sent a real message in >7 days.
                    if (!debug_type || weechat_strcasecmp(debug_type, "groupchat") != 0)
                    {
                        const std::string from_bare_s = debug_from ? ::jid(nullptr, debug_from).bare : std::string {};
                        const std::string to_bare_s   = debug_to   ? ::jid(nullptr, debug_to).bare   : std::string {};
                        const char *from_bare_cs = from_bare_s.empty() ? nullptr : from_bare_s.c_str();
                        const char *to_bare_cs   = to_bare_s.empty()   ? nullptr : to_bare_s.c_str();
                        
                        // Determine the conversation partner JID
                        const char *partner_jid = nullptr;
                        if (from_bare_cs && weechat_strcasecmp(from_bare_cs, account.jid().data()) != 0)
                            partner_jid = from_bare_cs;  // Message FROM someone else
                        else if (to_bare_cs && weechat_strcasecmp(to_bare_cs, account.jid().data()) != 0)
                            partner_jid = to_bare_cs;  // Message TO someone else (sent by us)
                        
                        // Create PM channel only if the stanza carries real user content AND
                        // the user hasn't deliberately closed it (last_mam_fetch == -1 in LMDB).
                        if (partner_jid && !account.channels.contains(partner_jid)
                            && account.mam_cache_get_last_timestamp(partner_jid) != static_cast<time_t>(-1)
                            && stanza_has_user_message_payload(message))
                        {
                            XDEBUG("MAM: discovered conversation with {}", partner_jid);

                            account.channels.emplace(
                                std::make_pair(partner_jid, weechat::channel {
                                        account, weechat::channel::chat_type::PM,
                                        partner_jid, partner_jid
                                    }));
                        }
                    }
                    
                    // Extract message details for caching
                    const char *msg_id = xmpp_stanza_get_id(message);
                    const char *msg_from = xmpp_stanza_get_from(message);
                    const char *msg_to = xmpp_stanza_get_to(message);
                    xmpp_stanza_t *msg_body = xmpp_stanza_get_child_by_name(message, "body");
                    xmpp_string_guard msg_text_g(account.context,
                        msg_body ? xmpp_stanza_get_text(msg_body) : nullptr);
                    const char *msg_text = msg_text_g.ptr;
                    
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
                    
                    // Cache the message if we have all required fields.
                    // For OMEMO messages there is no plaintext <body> in the stanza, so
                    // fall back to the omemo_plaintext LMDB table (populated at live
                    // delivery) to get the decrypted body.  This lets mam_cache_load_messages
                    // replay OMEMO conversations on the next restart without needing to
                    // re-decrypt from the server archive (which would fail due to ratchet
                    // state having advanced).
                    if (msg_id && msg_from && msg_timestamp > 0)
                    {
                        std::string from_bare_s2 = ::jid(nullptr, msg_from).bare;
                        std::string to_bare_s2 = msg_to ? ::jid(nullptr, msg_to).bare : std::string{};
                        
                        // Determine channel JID (from_bare for received, to_bare for sent)
                        const char *channel_jid = from_bare_s2.c_str();
                        if (!to_bare_s2.empty() && weechat_strcasecmp(to_bare_s2.c_str(), account.jid().data()) != 0)
                            channel_jid = to_bare_s2.c_str();

                        // Use plaintext body if available; otherwise check omemo_plaintext cache.
                        std::optional<std::string> omemo_body;
                        if (!msg_text)
                            omemo_body = account.mam_cache_lookup_omemo_plaintext(channel_jid, msg_id);

                        const char *effective_body = msg_text
                            ? msg_text
                            : (omemo_body ? omemo_body->c_str() : nullptr);

                        if (effective_body)
                        {
                            // For MUC messages, store the occupant nick (resource part of the
                            // full JID, e.g. "Nick" from "room@service/Nick") so that
                            // mam_cache_load_messages displays the sender correctly.
                            // For PM messages, the bare JID is the right display string.
                            std::string cache_from;
                            if (debug_type && weechat_strcasecmp(debug_type, "groupchat") == 0)
                            {
                                cache_from = ::jid(nullptr, msg_from).resource;
                                if (cache_from.empty())
                                    cache_from = from_bare_s2; // fallback if resource missing
                            }
                            else
                            {
                                cache_from = from_bare_s2;
                            }
                            account.mam_cache_message(channel_jid, msg_id, cache_from,
                                                      msg_timestamp, effective_body);
                        }
                    }
                    
                    // XEP-0313: Dedup MAM results against already-displayed live messages.
                    // Two tag strategies are used:
                    //   1. stanza_id_<archive-id>  – written when the server injects a
                    //      <stanza-id> element on live delivery (XEP-0359).
                    //   2. id_<msg-id>              – written from the message's own id=
                    //      attribute (always present); used as fallback for servers that
                    //      do not inject <stanza-id> on direct delivery.
                    // If either tag is found in the channel buffer, the message was
                    // already displayed and the MAM copy should be silently discarded.
                    const char *archive_id = xmpp_stanza_get_attribute(result, "id");
                    if ((archive_id && *archive_id) || (msg_id && *msg_id))
                    {
                        // Resolve channel JID from the forwarded message addresses
                        std::string chk_from_s = msg_from ? ::jid(nullptr, msg_from).bare : std::string{};
                        std::string chk_to_s   = msg_to   ? ::jid(nullptr, msg_to).bare   : std::string{};

                        const char *channel_jid_chk = !chk_from_s.empty() ? chk_from_s.c_str() : nullptr;
                        if (!chk_to_s.empty() && weechat_strcasecmp(chk_to_s.c_str(), account.jid().data()) != 0)
                            channel_jid_chk = chk_to_s.c_str();

                        struct t_gui_buffer *chk_buf = nullptr;
                        if (channel_jid_chk) {
                            auto it = account.channels.find(channel_jid_chk);
                            if (it != account.channels.end())
                                chk_buf = it->second.buffer;
                        }

                        bool already_displayed = false;
                        if (chk_buf)
                        {
                            // Primary needle: stanza_id set by server on live delivery (XEP-0359)
                            std::string needle1 = (archive_id && *archive_id)
                                ? std::string("stanza_id_") + archive_id : std::string{};
                            // Fallback needle: message's own id= tag, always written on live delivery
                            std::string needle2 = (msg_id && *msg_id)
                                ? std::string("id_") + msg_id : std::string{};

                            struct t_gui_lines *own_lines = (struct t_gui_lines*)
                                weechat_hdata_pointer(hdata_buffer,
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
                                        if (tags)
                                        {
                                            std::string_view tv(tags);
                                            if ((!needle1.empty() && tv.contains(needle1)) ||
                                                (!needle2.empty() && tv.contains(needle2)))
                                                already_displayed = true;
                                        }
                                    }
                                    ln = (struct t_gui_line*)
                                        weechat_hdata_move(hdata_line, ln, -1);
                                }
                            }
                        }

                        if (already_displayed)
                            return 1;
                    }

                    message = xmpp_stanza_copy(message);
                    if (delay != nullptr)
                        xmpp_stanza_add_child_ex(message, xmpp_stanza_copy(delay), 0);
                    int ret = message_handler(message, false, true);  // MAM replay: suppress outgoing receipts/markers
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
                        xmpp_string_guard b64_data_g(account.context, xmpp_stanza_get_text(data_elem));
                                char *b64_data = b64_data_g.ptr;
                                if (b64_data)
                                {
                                    // Decode base64 avatar data
                                    BIO *bio, *b64;
                                    size_t decode_len = std::string_view(b64_data).size();
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
                        xmpp_string_guard nick_text_g(account.context,
                            nick_elem ? xmpp_stanza_get_text(nick_elem) : nullptr);
                        const char *nick_text = nick_text_g.ptr;

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
                            auto existing_ch = account.channels.find(item_id);
                            bool already_joined = existing_ch != account.channels.end()
                                && existing_ch->second.buffer
                                && !existing_ch->second.joining;

                            if (!is_biboumi && !already_joined)
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
                            auto ch_it = account.channels.find(item_id);
                            if (ch_it != account.channels.end() && ch_it->second.buffer
                                && !ch_it->second.joining)
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
                        std::string own_bare_s  = ::jid(nullptr, account.jid().data()).bare;
                        std::string event_bare_s = ::jid(nullptr, event_from).bare;
                        bool is_own = !own_bare_s.empty() && !event_bare_s.empty() &&
                            (weechat_strcasecmp(own_bare_s.c_str(), event_bare_s.c_str()) == 0);

                        if (!is_own)
                            continue;

                        // Find the channel for this peer and clear its unreads
                        // up to and including last_id (or all if id unknown)
                        weechat::channel *ch = nullptr;
                        { auto it = account.channels.find(peer_jid); ch = it != account.channels.end() ? &it->second : nullptr; }
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
                // Treat reversed-domain protocol namespaces (e.g. eu.siacs.conversations.axolotl.*)
                // the same as urn: URIs — they are OMEMO/PEP protocol nodes, not user feeds.
                bool node_is_protocol_ns = !node_sv.empty() && !node_is_microblog && (
                    node_sv.rfind("eu.siacs.", 0) == 0 ||
                    node_sv.rfind("com.google.", 0) == 0 ||
                    node_sv.rfind("org.jivesoftware.", 0) == 0);
                bool node_is_uri = !node_sv.empty() && !node_is_microblog && (
                    node_sv.find("://") != std::string_view::npos ||
                    node_sv.substr(0, 4) == "urn:" ||
                    node_is_protocol_ns);
                bool from_self = false;
                if (!feed_service_sv.empty())
                {
                    std::string own_bare_fs  = ::jid(nullptr, account.jid().data()).bare;
                    std::string from_bare_fs = ::jid(nullptr, std::string(feed_service_sv).c_str()).bare;
                    from_self = !own_bare_fs.empty() && !from_bare_fs.empty() &&
                                weechat_strcasecmp(own_bare_fs.c_str(), from_bare_fs.c_str()) == 0;
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

        // XEP-0184: Message Delivery Receipts — handle incoming <received> from others
        // Helper: walk the buffer backwards and update the status glyph on the line
        // tagged id_<acked_id> (strips any existing glyph suffix, appends new_glyph).
        auto update_line_glyph = [](weechat::channel *ch,
                                    const char       *acked_id,
                                    std::string_view  new_glyph)
        {
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
                    std::string bare_s = ::jid(nullptr, receipt_from).bare;
                    auto ch_it = account.channels.find(bare_s);
                    weechat::channel *ch = ch_it != account.channels.end() ? &ch_it->second : nullptr;
                    if (ch && ch->type != weechat::channel::chat_type::MUC)
                    {
                        // Find the sent message line tagged id_<acked_id> and update glyph ⌛→✓
                        update_line_glyph(ch, acked_id, " ✓");
                    }
                }
                return 1;
            }
        }

        // XEP-0333: Chat Markers — handle <displayed> from others (v1.0+; <read> removed)
        {
            xmpp_stanza_t *marker_displayed = xmpp_stanza_get_child_by_name_and_ns(
                stanza, "displayed", "urn:xmpp:chat-markers:0");
            xmpp_stanza_t *marker = marker_displayed;
            if (marker)
            {
                const char *marker_from = xmpp_stanza_get_from(stanza);
                const char *acked_id = xmpp_stanza_get_id(marker);
                if (marker_from && acked_id)
                {
                    std::string bare_s = ::jid(nullptr, marker_from).bare;
                    auto ch_it = account.channels.find(bare_s);
                    weechat::channel *ch = ch_it != account.channels.end() ? &ch_it->second : nullptr;
                    if (ch && ch->type != weechat::channel::chat_type::MUC)
                    {
                        // Find the sent message line tagged id_<acked_id> and update glyph ⌛/✓→✓✓
                        update_line_glyph(ch, acked_id, " ✓✓");
                    }
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
                    std::string bare_s = ::jid(nullptr, attn_from).bare;
                    auto ch_it = account.channels.find(bare_s);
                    weechat::channel *ch = ch_it != account.channels.end() ? &ch_it->second : nullptr;
                    if (!ch)
                    {
                        // Create PM channel for the buzz
                        ch = &account.channels.emplace(
                            std::make_pair(bare_s, weechat::channel {
                                    account, weechat::channel::chat_type::PM, bare_s, bare_s
                                })).first->second;
                    }
                    weechat_printf_date_tags(ch->buffer, 0, "xmpp_attention,notify_highlight",
                                            "%s%s is requesting your attention! (/buzz)",
                                            weechat_prefix("network"),
                                            bare_s.c_str());
                }
                return 1;
            }
        }

        return 1;
    }
    type = xmpp_stanza_get_type(stanza);
    if (type != nullptr && std::string_view(type) == "error")
        return 1;
    from = xmpp_stanza_get_from(stanza);
    if (from == nullptr)
        return 1;
    from_bare_main_storage = ::jid(nullptr, from).bare;
    from_bare = from_bare_main_storage.c_str();
    to = xmpp_stanza_get_to(stanza);
    if (to == nullptr)
        to = account.jid().data();
    to_bare_main_storage = to ? ::jid(nullptr, to).bare : std::string{};
    to_bare = !to_bare_main_storage.empty() ? to_bare_main_storage.c_str() : nullptr;
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

    // XEP-0203: check for delayed delivery early so we can suppress
    // receipts/markers for offline-stored messages (see receipt block below).
    const bool is_delayed_delivery = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "delay", "urn:xmpp:delay") != nullptr;

    const char *channel_id = account.jid() == from_bare ? to_bare : from_bare;
    auto parent_ch_it = account.channels.find(channel_id);
    parent_channel = parent_ch_it != account.channels.end() ? &parent_ch_it->second : nullptr;
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
    // XEP-0333 §4.1: SHOULD NOT send Chat Markers to a MUC room; they reveal
    // presence to all participants and serve no useful purpose in group chat.
    // MAM-replayed messages MUST NOT generate any outgoing receipts or markers:
    // the archive is a historical record; replaying it on reconnect must be silent.
    // XEP-0203: offline/delayed deliveries (messages with <delay>) must also be
    // suppressed — the sending client has already moved on and firing a receipt
    // for a stale offline message creates a spurious error→receipt loop when the
    // remote user has no active session on their server.
    const bool is_muc_channel = channel && channel->type == weechat::channel::chat_type::MUC;
    if (id && (markable || request) && !is_self_outbound_copy && !is_muc_channel && !is_mam_replay && !is_delayed_delivery)
    {
        weechat::channel::unread unread_val;
        unread_val.id = id;
        unread_val.thread = thread ? std::optional<std::string>(thread) : std::nullopt;
        // XEP-0490: preserve server stanza-id for correct MDS PEP publish
        unread_val.stanza_id = stanza_id ? std::optional<std::string>(stanza_id) : std::nullopt;
        unread_val.stanza_id_by = stanza_id_by ? std::optional<std::string>(stanza_id_by) : std::nullopt;
        auto unread = &unread_val;

        // XEP-0184 / XEP-0333: reply MUST go to the full JID (from= of the
        // incoming stanza, which includes the resource).  Sending to the bare
        // JID causes the server to fan-out to all active resources, so every
        // other device of the contact sees the receipt/marker.
        // XEP-0334: receipt/marker replies MUST NOT be stored.
        // Use the incoming message type so routing is correct.
        stanza::message msg;
        msg.to(from).type(type ? type : "chat");

        if (request)
            msg.receipt_received(unread->id);

        if (markable)
            msg.chat_marker_displayed(unread->id);

        if (unread->thread.has_value())
            msg.thread(*unread->thread);

        // XEP-0334: receipt/marker replies MUST NOT be stored
        msg.no_store();

        account.connection.send(msg.build(account.context).get());

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
            std::string invite_from_bare_s = from ? ::jid(nullptr, from).bare : std::string{};
            from_bare = !invite_from_bare_s.empty() ? invite_from_bare_s.c_str() : "unknown";
            
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

    encrypted = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "encrypted", "eu.siacs.conversations.axolotl");

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

    // For live carbon copies of self-sent OMEMO messages we don't have the
    // session state to decrypt them on the spot, so show the advisory text.
    //
    // For MAM replays of self-sent messages: if the message was sent from
    // THIS device (sender_sid == our device_id), Signal cannot decrypt the
    // self-copy — the key was encrypted using our own outbound session state,
    // but the inbound session at {own_jid, own_device_id} has never been
    // established (we never received a PreKeySignalMessage from ourselves in
    // the Signal protocol sense). Attempting decryption produces "Bad MAC"
    // on all archived session states. Show OMEMO_ADVICE instead.
    //
    // For MAM replays from OTHER devices on our account (sender_sid != our
    // device_id), fall through to decode() — they may have encrypted a key
    // for our device and we can attempt decryption normally.
    const bool is_own_device_self_copy = [&]() -> bool {
        if (!encrypted || !is_self_outbound_copy || !account.omemo)
            return false;
        xmpp_stanza_t *enc_header = xmpp_stanza_get_child_by_name(encrypted, "header");
        if (!enc_header)
            return false;
        const char *sid_str = xmpp_stanza_get_attribute(enc_header, "sid");
        if (!sid_str || !*sid_str)
            return false;
        char *end = nullptr;
        unsigned long sid_val = std::strtoul(sid_str, &end, 10);
        if (end == sid_str || *end != '\0')
            return false;
        return static_cast<std::uint32_t>(sid_val) == account.omemo.device_id;
    }();
    if (encrypted && is_self_outbound_copy && (!is_mam_replay || is_own_device_self_copy))
    {
        if (intext)
            xmpp_free(account.context, intext);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        intext = xmpp_strdup(account.context, OMEMO_ADVICE);
#pragma GCC diagnostic pop
        // For MAM replays of messages sent from this device: the self-copy
        // can never be decrypted (Signal does not support self-decryption of
        // outbound messages — the inbound session at {own_jid, own_device_id}
        // is never established). Clear |encrypted| so the decode() block is
        // skipped and the message is displayed using the OMEMO_ADVICE
        // placeholder (showing the user their sent history exists without
        // corrupting the live Signal session state).
        // For live carbon copies (!is_mam_replay), leave |encrypted| set so
        // the existing silently-discard path at line 2026 still applies
        // (live copies are already shown as sent messages; no replay needed).
        if (is_mam_replay)
            encrypted = nullptr;
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
        bool omemo_is_duplicate = false;
        // On MAM replay: check if we cached the plaintext from the live delivery.
        // If found, use it directly and skip Signal decryption entirely — the ratchet
        // key was consumed in the original session and decryption would fail with
        // SG_ERR_DUPLICATE_MESSAGE.  This mirrors Gajim's SQLite archive approach.
        if (is_mam_replay && stable_id && channel_id)
        {
            auto cached = account.mam_cache_lookup_omemo_plaintext(
                std::string(channel_id), std::string(stable_id));
            if (cached.has_value())
            {
                omemo_cleartext_storage = std::move(*cached);
                cleartext = omemo_cleartext_storage.data();
                encrypted = nullptr;  // bypass after-omemo null-text guard
                goto message_handler_after_omemo;
            }
        }
        // Always attempt decryption — MAM messages can be decrypted if the
        // Signal session is still valid.  quiet=false so errors are logged.
        // out_is_duplicate distinguishes "already seen live" (SG_ERR_DUPLICATE_MESSAGE)
        // from a genuine session failure, matching Gajim's DuplicateMessage→NodeProcessed.
        auto omemo_result = account.omemo.decode(&account, channel->buffer, from_bare, encrypted,
                                                 /*quiet=*/false, &omemo_is_duplicate,
                                                 /*suppress_peer_traffic=*/is_mam_replay);
        if (omemo_result)
        {
            omemo_cleartext_storage = std::move(*omemo_result);
            cleartext = omemo_cleartext_storage.data();
            // Cache decrypted plaintext for MAM replay in future sessions.
            // Only store on live inbound delivery — not for MAM replays themselves
            // (the cache hit path below handles those) and not for self outbound
            // copies (carbons of our own sent messages).
            if (!is_mam_replay && !is_self_outbound_copy && channel_id)
            {
                // Store under all three IDs so the MAM replay lookup always hits.
                // On live delivery the server injects <stanza-id> (server-assigned);
                // on MAM replay the server strips <stanza-id> from the forwarded copy,
                // so stable_id on replay resolves to origin-id or the message id=
                // attribute instead — a different value.  Storing all three guarantees
                // a cache hit regardless of which one the replay uses as its stable_id.
                std::string ch(channel_id);
                std::string sid   = stanza_id  ? std::string(stanza_id)  : "";
                std::string oid   = origin_id  ? std::string(origin_id)  : "";
                std::string mid   = id         ? std::string(id)         : "";
                if (!sid.empty())
                    account.mam_cache_store_omemo_plaintext(ch, sid, omemo_cleartext_storage);
                if (!oid.empty() && oid != sid)
                    account.mam_cache_store_omemo_plaintext(ch, oid, omemo_cleartext_storage);
                if (!mid.empty() && mid != sid && mid != oid)
                    account.mam_cache_store_omemo_plaintext(ch, mid, omemo_cleartext_storage);
            }
        }
        if (!cleartext)
        {
            if (is_self_outbound_copy)
                goto message_handler_after_omemo;

            if (is_mam_replay)
            {
                // Show a placeholder for all MAM decryption failures, including
                // SG_ERR_DUPLICATE_MESSAGE (omemo_is_duplicate).  The within-session
                // case (live delivery then MAM replay in the same WeeChat run) is
                // already handled by stanza-id dedup above, which fires first and
                // returns early.  Reaching here with omemo_is_duplicate means the
                // Signal ratchet was consumed in a prior WeeChat session — the buffer
                // is empty (fresh start) and the message was never displayed in this
                // session.  The plaintext is irrecoverable either way, so show a
                // placeholder so the user knows a message exists (matches what
                // Conversations / Gajim show for undecryptable archived messages).
                if (intext)
                    xmpp_free(account.context, intext);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
                intext = xmpp_strdup(account.context, "[undecryptable OMEMO message]");
#pragma GCC diagnostic pop
                encrypted = nullptr;   // prevent the after-omemo null-text guard from skipping display
                goto message_handler_after_omemo;
            }

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
    // If OMEMO decryption produced no cleartext and this is a self-outbound copy
    // (MAM replay of a message we sent from another device, or a live carbon copy),
    // there is nothing to display.  Key-transport messages (null payload, all-zero IV)
    // are the common case here — they establish the session but carry no user-visible
    // content.  Falling through to the display path with text==nullptr would print a
    // blank line (or crash on weechat_string_match(nullptr)).
    // Note: MAM replays of messages sent from THIS device have |encrypted| cleared
    // above so they bypass this check and flow to the display path with
    // intext=OMEMO_ADVICE (showing the user their sent history).
    // Note: MAM replays with decryption failure (including cross-session duplicates)
    // set intext=[undecryptable] and clear |encrypted|, so they bypass this check
    // and display the placeholder.
    if (encrypted && !cleartext && (is_self_outbound_copy || is_mam_replay))
        return 1;

    if (x)
    {
        xmpp_string_guard ciphertext_g(account.context, xmpp_stanza_get_text(x));
        const char *ciphertext = ciphertext_g.ptr;
        if (auto decrypted = account.pgp.decrypt(channel->buffer, ciphertext)) {
            pgp_cleartext_storage = std::move(*decrypted);
            cleartext = pgp_cleartext_storage.data();
        }
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
            // The <fallback> child <body start="N" end="M"/> gives the byte
            // range [start,end) to remove (XEP-0428 §2.2).  Default start=0.
            // After stripping, skip any leading whitespace (blank lines
            // that separate the quote from the reply text).
            xmpp_stanza_t *fb_body = xmpp_stanza_get_child_by_name(fallback_elem, "body");
            const char *end_attr   = fb_body
                ? xmpp_stanza_get_attribute(fb_body, "end")   : nullptr;
            const char *start_attr = fb_body
                ? xmpp_stanza_get_attribute(fb_body, "start") : nullptr;
            if (end_attr)
            {
                long end   = std::strtol(end_attr,   nullptr, 10);
                long start = start_attr ? std::strtol(start_attr, nullptr, 10) : 0L;
                if (start < 0) start = 0;
                std::string_view sv(text);
                if (end > 0 && static_cast<std::size_t>(end) < sv.size())
                {
                    // Reconstruct: prefix (before start) + suffix (after end)
                    std::string rebuilt;
                    if (start > 0)
                        rebuilt = std::string(sv.substr(0, static_cast<std::size_t>(start)));
                    std::string_view suffix = sv.substr(static_cast<std::size_t>(end));
                    // Skip blank lines / leading whitespace between quote and reply
                    auto first_non_ws = suffix.find_first_not_of(" \t\r\n");
                    if (first_non_ws != std::string_view::npos)
                        suffix.remove_prefix(first_non_ws);
                    rebuilt += suffix;
                    trimmed_body = std::move(rebuilt);
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
    xmpp_string_guard spoiler_hint_g(account.context,
        spoiler_elem ? xmpp_stanza_get_text(spoiler_elem) : nullptr);
    const char *spoiler_hint = spoiler_hint_g.ptr;

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
        void *edit_line_data = nullptr; // line_data of the original message for in-place update
        void *lines = weechat_hdata_pointer(hdata_buffer,
                                            channel->buffer, "lines");
        if (lines)
        {
            void *last_line = weechat_hdata_pointer(hdata_lines,
                                                    lines, "last_line");
            while (last_line && !orig)
            {
                void *line_data = weechat_hdata_pointer(hdata_line,
                                                        last_line, "data");
                if (line_data)
                {
                    int tags_count = weechat_hdata_integer(hdata_line_data,
                                                           line_data, "tags_count");
                    std::string str_tag;
                    for (int n_tag = 0; n_tag < tags_count; n_tag++)
                    {
                        str_tag = fmt::format("{}|tags_array", n_tag);
                        const char *tag = weechat_hdata_string(hdata_line_data,
                                                               line_data, str_tag.c_str());
                         if (tag && std::string_view(tag).starts_with("id_") &&
                             weechat_strcasecmp(tag + 3, replace_id) == 0)
                         {
                             // XEP-0308 §3: Verify the correcting sender matches
                              // the original message author before applying the edit.
                              // For MUC: compare MUC nick (resource part of full JID).
                              // For PM:  compare bare JID — the nick_ tag may store a full
                              //          JID with a different resource than the correction,
                              //          so bare-JID matching is the only robust approach.
                              const char *corr_nick = from_bare; // default: bare JID for PM
                              std::string corr_resource_s;
                              if (channel && channel->type == weechat::channel::chat_type::MUC)
                              {
                                  corr_resource_s = ::jid(nullptr, from).resource;
                                  if (!corr_resource_s.empty())
                                      corr_nick = corr_resource_s.c_str();
                              }

                            bool sender_matches = false;
                            for (int chk = 0; chk < tags_count; chk++)
                            {
                                str_tag = fmt::format("{}|tags_array", chk);
                                const char *chk_tag = weechat_hdata_string(
                                    hdata_line_data, line_data, str_tag.c_str());
                                 if (chk_tag && std::string_view(chk_tag).starts_with("nick_") &&
                                    weechat_strcasecmp(chk_tag + 5, corr_nick) == 0)
                                {
                                    sender_matches = true;
                                    break;
                                }
                            }
                            if (!sender_matches)
                                break;  // Silently drop spoofed correction

                            // Save this line_data for the in-place update below.
                            edit_line_data = line_data;

                            auto arraylist_deleter = [](struct t_arraylist *al) {
                                weechat_arraylist_free(al);
                            };
                            std::unique_ptr<struct t_arraylist, decltype(arraylist_deleter)>
                                orig_lines_ptr(weechat_arraylist_new(0, 0, 0, nullptr, nullptr, nullptr, nullptr),
                                               arraylist_deleter);
                            struct t_arraylist *orig_lines = orig_lines_ptr.get();
                            char *msg = (char*)weechat_hdata_string(hdata_line_data,
                                                                    line_data, "message");
                            weechat_arraylist_insert(orig_lines, 0, msg);

                            while (msg)
                            {
                                last_line = weechat_hdata_pointer(hdata_line,
                                                                  last_line, "prev_line");
                                if (last_line)
                                    line_data = weechat_hdata_pointer(hdata_line,
                                                                      last_line, "data");
                                else
                                    line_data = nullptr;

                                msg = nullptr;
                                if (line_data)
                                {
                                    tags_count = weechat_hdata_integer(hdata_line_data,
                                                                       line_data, "tags_count");
                                    for (n_tag = 0; n_tag < tags_count; n_tag++)
                                    {
                                        str_tag = fmt::format("{}|tags_array", n_tag);
                                        tag = weechat_hdata_string(hdata_line_data,
                                                                   line_data, str_tag.c_str());
                                         if (tag && std::string_view(tag).starts_with("id_") &&
                                            weechat_strcasecmp(tag + 3, replace_id) == 0)
                                        {
                                            msg = (char*)weechat_hdata_string(hdata_line_data,
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

                last_line = weechat_hdata_pointer(hdata_line,
                                                  last_line, "prev_line");
            }
        }

        (void)orig; // diff display removed; corrected text shown as-is

        // XEP-0308: Replace the original line in-place rather than appending a
        // new "* " line.  Use the inline diff if available, otherwise plain text.
        if (edit_line_data)
        {
            const std::string new_msg = std::string("✏️ ") + text;
            struct t_hashtable *ht = weechat_hashtable_new(4,
                WEECHAT_HASHTABLE_STRING, WEECHAT_HASHTABLE_STRING,
                nullptr, nullptr);
            weechat_hashtable_set(ht, "message", new_msg.c_str());
            weechat_hdata_update(hdata_line_data, edit_line_data, ht);
            weechat_hashtable_free(ht);
            return 1;
        }
        // Original message not in buffer (e.g. scrolled out or MAM replay) —
        // drop the correction silently rather than printing a dangling ✏️ line.
        return 1;
    }

    // XEP-0425 v0.3: Message Moderation (extends XEP-0424)
    // Wire format: <retract xmlns='urn:xmpp:message-retract:1' id='...'> with a
    // <moderated xmlns='urn:xmpp:message-moderate:1'> child element.
    // (The obsolete pre-v0.3 <apply-to xmlns='urn:xmpp:fasten:0'> wrapper is gone.)
    //
    // §5 MUST: accept moderation only from the MUC service itself (from_bare == channel JID).
    xmpp_stanza_t *mod_retract = xmpp_stanza_get_child_by_name_and_ns(stanza, "retract",
                                                                       "urn:xmpp:message-retract:1");
    xmpp_stanza_t *moderated_elem = mod_retract
        ? xmpp_stanza_get_child_by_name_and_ns(mod_retract, "moderated",
                                               "urn:xmpp:message-moderate:1")
        : nullptr;
    const char *moderate_id = mod_retract ? xmpp_stanza_get_attribute(mod_retract, "id") : nullptr;
    const char *moderate_reason = nullptr;
    xmpp_string_guard moderate_reason_g { account.context, nullptr };

    if (moderated_elem && moderate_id)
    {
        // §5 MUST: moderation stanza MUST originate from the MUC room itself.
        // Reject if the sender is not the MUC service (i.e. from_bare != channel JID).
        if (!channel || !from_bare ||
            weechat_strcasecmp(from_bare, channel->id.data()) != 0)
            return 1;

        // Extract optional reason
        xmpp_stanza_t *reason_elem = xmpp_stanza_get_child_by_name(moderated_elem, "reason");
        if (reason_elem)
        {
            moderate_reason_g = xmpp_string_guard(account.context, xmpp_stanza_get_text(reason_elem));
            moderate_reason = moderate_reason_g.ptr;
        }

        // Save moderation to MAM cache
        const char *channel_id = account.jid() == from_bare ? to_bare : from_bare;
        account.mam_cache_retract_message(channel_id, moderate_id);

        // Find and tombstone the moderated message in buffer
        void *lines = weechat_hdata_pointer(hdata_buffer,
                                            channel->buffer, "lines");
        if (lines)
        {
            void *last_line = weechat_hdata_pointer(hdata_lines,
                                                    lines, "last_line");
            while (last_line)
            {
                void *line_data = weechat_hdata_pointer(hdata_line,
                                                        last_line, "data");
                if (line_data)
                {
                    int tags_count = weechat_hdata_integer(hdata_line_data,
                                                           line_data, "tags_count");
                    std::string str_tag;
                    for (int n_tag = 0; n_tag < tags_count; n_tag++)
                    {
                        str_tag = fmt::format("{}|tags_array", n_tag);
                        const char *tag = weechat_hdata_string(hdata_line_data,
                                                               line_data, str_tag.c_str());
                        if (tag && std::string_view(tag).starts_with("id_") &&
                            weechat_strcasecmp(tag + 3, moderate_id) == 0)
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
                            weechat_hdata_update(hdata_line_data, line_data, hashtable);
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

                            return 1;
                        }
                    }
                }

                last_line = weechat_hdata_pointer(hdata_line,
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

        return 1;
    }

    // XEP-0424: Message Retraction
    xmpp_stanza_t *retract = xmpp_stanza_get_child_by_name_and_ns(stanza, "retract",
                                                                   "urn:xmpp:message-retract:1");
    const char *retract_id = retract ? xmpp_stanza_get_attribute(retract, "id") : nullptr;

    if (retract_id)
    {
        // XEP-0424 §5 MUST: verify the retractor is the original sender.
        // For MUC: compare MUC nick (resource part).
        // For PM:  compare bare JID.
        // Additionally, if the room advertises XEP-0421 (occupant-id), use
        // the occupant-id from the retraction stanza to verify identity even
        // through nick changes in semi-anonymous rooms.
        const char *retract_nick = from_bare; // default for PM
        std::string retract_resource_s;
        if (channel && channel->type == weechat::channel::chat_type::MUC)
        {
            retract_resource_s = ::jid(nullptr, from).resource;
            if (!retract_resource_s.empty())
                retract_nick = retract_resource_s.c_str();
        }

        // Extract occupant-id from the retraction stanza (XEP-0421 §4).
        // Only trusted if the room advertises urn:xmpp:occupant-id:0.
        const char *retract_occ_id = nullptr;
        const bool room_has_occupant_ids = channel && from_bare &&
            account.peer_supports_feature(from_bare, "urn:xmpp:occupant-id:0");
        if (room_has_occupant_ids)
        {
            xmpp_stanza_t *occ_elem = xmpp_stanza_get_child_by_name_and_ns(
                stanza, "occupant-id", "urn:xmpp:occupant-id:0");
            if (occ_elem)
                retract_occ_id = xmpp_stanza_get_attribute(occ_elem, "id");
        }

        // Save retraction to MAM cache
        const char *channel_id = account.jid() == from_bare ? to_bare : from_bare;
        account.mam_cache_retract_message(channel_id, retract_id);

        // Find and tombstone the retracted message in buffer
        void *lines = weechat_hdata_pointer(hdata_buffer,
                                            channel->buffer, "lines");
        if (lines)
        {
            void *last_line = weechat_hdata_pointer(hdata_lines,
                                                    lines, "last_line");
            while (last_line)
            {
                void *line_data = weechat_hdata_pointer(hdata_line,
                                                        last_line, "data");
                if (line_data)
                {
                    int tags_count = weechat_hdata_integer(hdata_line_data,
                                                           line_data, "tags_count");
                    std::string str_tag;
                    bool id_matched = false;
                    for (int n_tag = 0; n_tag < tags_count; n_tag++)
                    {
                        str_tag = fmt::format("{}|tags_array", n_tag);
                        const char *tag = weechat_hdata_string(hdata_line_data,
                                                               line_data, str_tag.c_str());
                        if (tag && std::string_view(tag).starts_with("id_") &&
                            weechat_strcasecmp(tag + 3, retract_id) == 0)
                        {
                            id_matched = true;
                            break;
                        }
                    }

                    if (id_matched)
                    {
                        // XEP-0424 §5 MUST: sender verification.
                        // Primary method: nick_ tag match.
                        // Secondary (MUC semi-anon): occupant_id_ tag match.
                        bool sender_ok = false;

                        if (retract_occ_id)
                        {
                            // Occupant-id available and room is trusted — use it.
                            // It is robust against nick changes.
                            std::string occ_needle =
                                std::string("occupant_id_") + retract_occ_id;
                            for (int chk = 0; chk < tags_count && !sender_ok; chk++)
                            {
                                str_tag = fmt::format("{}|tags_array", chk);
                                const char *t = weechat_hdata_string(hdata_line_data,
                                                                     line_data, str_tag.c_str());
                                if (t && weechat_strcasecmp(t, occ_needle.c_str()) == 0)
                                    sender_ok = true;
                            }
                        }
                        else
                        {
                            // Fall back to nick_ tag comparison.
                            for (int chk = 0; chk < tags_count && !sender_ok; chk++)
                            {
                                str_tag = fmt::format("{}|tags_array", chk);
                                const char *t = weechat_hdata_string(hdata_line_data,
                                                                     line_data, str_tag.c_str());
                                if (t && std::string_view(t).starts_with("nick_") &&
                                    weechat_strcasecmp(t + 5, retract_nick) == 0)
                                    sender_ok = true;
                            }
                        }

                        if (!sender_ok)
                        {
                            // Sender mismatch — silently drop the spoofed retraction.
                            break;
                        }

                        // Found the message to retract - update it with tombstone
                        auto tombstone = fmt::format("{}[Message deleted]{}",
                                                     weechat_color("darkgray"),
                                                     weechat_color("resetcolor"));

                        struct t_hashtable *hashtable = weechat_hashtable_new(8,
                            WEECHAT_HASHTABLE_STRING,
                            WEECHAT_HASHTABLE_STRING,
                            nullptr, nullptr);
                        weechat_hashtable_set(hashtable, "message", tombstone.c_str());
                        weechat_hashtable_set(hashtable, "tags", "xmpp_retracted,notify_none");
                        weechat_hdata_update(hdata_line_data, line_data, hashtable);
                        weechat_hashtable_free(hashtable);

                        // Print notification
                        weechat_printf_date_tags(channel->buffer, 0, "notify_none",
                            "%s%s retracted a message",
                            weechat_prefix("network"),
                            from_bare);

                        return 1;
                    }
                }

                last_line = weechat_hdata_pointer(hdata_line,
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
                xmpp_string_guard emoji_g(account.context, xmpp_stanza_get_text(reaction_elem));
                const char *emoji = emoji_g.ptr;
                if (emoji)
                {
                    if (!first) weechat_string_dyn_concat(dyn_emojis, " ", -1);
                    weechat_string_dyn_concat(dyn_emojis, emoji, -1);
                    first = false;
                }
            }
            reaction_elem = xmpp_stanza_get_next(reaction_elem);
        }
        
        const char *emojis = *dyn_emojis;
        // Always walk the buffer to update the target line — even when emojis
        // is empty (XEP-0444 §3.2: empty <reactions> means remove all reactions).
        {
            // Find the message being reacted to and append reaction
            void *lines = weechat_hdata_pointer(hdata_buffer,
                                                channel->buffer, "lines");
            if (lines)
            {
                void *last_line = weechat_hdata_pointer(hdata_lines,
                                                        lines, "last_line");
                while (last_line)
                {
                    void *line_data = weechat_hdata_pointer(hdata_line,
                                                            last_line, "data");
                    if (line_data)
                    {
                        int tags_count = weechat_hdata_integer(hdata_line_data,
                                                               line_data, "tags_count");
                        std::string str_tag;
                        for (int n_tag = 0; n_tag < tags_count; n_tag++)
                        {
                            str_tag = fmt::format("{}|tags_array", n_tag);
                            const char *tag = weechat_hdata_string(hdata_line_data,
                                                                   line_data, str_tag.c_str());
                            if (tag && std::string_view(tag).starts_with("id_") &&
                                weechat_strcasecmp(tag + 3, reactions_id) == 0)
                            {
                                // Found the message.
                                // XEP-0444: a new <reactions> from a sender REPLACES their
                                // previous reaction set — do not accumulate. Strip any
                                // previously-appended reaction blocks (our format:
                                // " <blue>[…]<reset>") before appending the new set.
                                const char *orig_message = weechat_hdata_string(
                                    hdata_line_data, line_data, "message");

                                std::string base(orig_message ? orig_message : "");
                                // The reaction suffix we append starts with " " + weechat_color("blue") + "["
                                static const std::string rxn_prefix =
                                    std::string(" ") + weechat_color("blue") + "[";
                                auto rxn_pos = base.find(rxn_prefix);
                                if (rxn_pos != std::string::npos)
                                    base.resize(rxn_pos);

                                std::string new_message = emojis && *emojis
                                    ? base
                                      + " " + weechat_color("blue")
                                      + "[" + emojis + "]"
                                      + weechat_color("resetcolor")
                                    : base; // empty reactions = remove suffix

                                // Update the line with reaction replaced
                                struct t_hashtable *hashtable = weechat_hashtable_new(8,
                                    WEECHAT_HASHTABLE_STRING,
                                    WEECHAT_HASHTABLE_STRING,
                                    nullptr, nullptr);
                                weechat_hashtable_set(hashtable, "message", new_message.c_str());
                                weechat_hdata_update(hdata_line_data, line_data, hashtable);
                                weechat_hashtable_free(hashtable);

                                weechat_string_dyn_free(dyn_emojis, 1);
                                return 1;
                            }
                        }
                    }

                    last_line = weechat_hdata_pointer(hdata_line,
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
        std::string gc_bare_s   = ::jid(nullptr, from).bare;
        std::string gc_resource_s = ::jid(nullptr, from).resource;
        // Use case-insensitive compare: JIDs are case-insensitive per RFC 6122
        if (!gc_bare_s.empty() && weechat_strcasecmp(channel->name.c_str(), gc_bare_s.c_str()) == 0
            && !gc_resource_s.empty())
        {
            nick_str = gc_resource_s; // copy into surviving std::string
            nick = nick_str.c_str();
        }
        // else nick stays as `from` (the full JID)
        display_from = from;
    }
    else if (parent_channel && parent_channel->type == weechat::channel::chat_type::MUC)
    {
        std::string muc_resource_s = ::jid(nullptr, from).resource;
        // Use case-insensitive compare: JIDs are case-insensitive per RFC 6122
        if (weechat_strcasecmp(channel->name.c_str(), from) == 0
            && !muc_resource_s.empty())
        {
            nick_str = muc_resource_s;
            nick = nick_str.c_str();
        }
        // else nick stays as `from`
        display_from = from;
    }
    else
    {
        // PM channel: use bare JID as nick so that XEP-0308 corrections from a
        // different resource of the same contact still match (resource may vary
        // between the original message and its correction).
        nick = from_bare;
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
        struct t_gui_lines *own_lines   = (struct t_gui_lines*)
            weechat_hdata_pointer(hdata_buffer,
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
                    if (tags && std::string_view(tags).contains(needle))
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
    // XEP-0421: Occupant Identifiers — store occupant_id_ tag for MUC messages.
    // Per §4 MUST: only trust the occupant-id if the room advertises
    // urn:xmpp:occupant-id:0 via disco#info (otherwise a malicious client
    // could forge the element before it reaches the MUC).
    if (is_muc_channel && from_bare &&
        account.peer_supports_feature(from_bare, "urn:xmpp:occupant-id:0"))
    {
        xmpp_stanza_t *occ_id_elem = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "occupant-id", "urn:xmpp:occupant-id:0");
        if (occ_id_elem)
        {
            const char *occ_id = xmpp_stanza_get_attribute(occ_id_elem, "id");
            if (occ_id && *occ_id)
            {
                weechat_string_dyn_concat(dyn_tags, ",occupant_id_", -1);
                weechat_string_dyn_concat(dyn_tags, occ_id, -1);
            }
        }
    }
    // XEP-0380: Store encryption method if advertised
    if (eme_namespace)
    {
        weechat_string_dyn_concat(dyn_tags, ",eme_", -1);
        if (eme_name)
            weechat_string_dyn_concat(dyn_tags, eme_name, -1);
        else if (std::string_view(eme_namespace).contains("omemo"))
            weechat_string_dyn_concat(dyn_tags, "OMEMO", -1);
        else if (std::string_view(eme_namespace).contains("openpgp"))
            weechat_string_dyn_concat(dyn_tags, "OpenPGP", -1);
        else if (std::string_view(eme_namespace).contains("encryption"))
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
            if (std::string_view(ref_name) != "reference"
                || std::string_view(ref_ns) != "urn:xmpp:reference:0")
                continue;
            const char *ref_type = xmpp_stanza_get_attribute(ref, "type");
            if (!ref_type || std::string_view(ref_type) != "mention") continue;
            const char *ref_uri = xmpp_stanza_get_attribute(ref, "uri");
            if (!ref_uri) continue;
            // URI is "xmpp:user@server" or "xmpp:user@server/resource"
            std::string_view ref_uri_sv(ref_uri);
            auto colon_pos = ref_uri_sv.find(':');
            if (colon_pos == std::string_view::npos) continue;
            std::string mentioned_jid(ref_uri_sv.substr(colon_pos + 1));
            // Strip resource if present
            auto slash = mentioned_jid.find('/');
            if (slash != std::string::npos)
                mentioned_jid = mentioned_jid.substr(0, slash);
            std::string local_bare_s = ::jid(nullptr, account.jid().data()).bare;
            if (!local_bare_s.empty() && weechat_strcasecmp(mentioned_jid.c_str(), local_bare_s.c_str()) == 0)
                xep0372_mentioned = true;
        }
        if (xep0372_mentioned)
            weechat_string_dyn_concat(dyn_tags, ",notify_highlight", -1);
    }

    // XEP-0492: Chat Notification Settings — override the WeeChat notify tag
    // based on the per-bookmark setting for this channel.
    // "never"      → suppress all notifications (notify_none)
    // "always"     → always highlight (notify_highlight)
    // "on-mention" → only highlight when @mentioned (default MUC behaviour)
    // (empty / default) → leave the tag already appended unchanged
    if (!date && !is_from_self && channel)
    {
        std::string_view channel_notify;
        auto bm_it = account.bookmarks.find(channel->id);
        if (bm_it != account.bookmarks.end())
            channel_notify = bm_it->second.notify_setting;

        if (channel_notify == "never")
        {
            // Strip any existing notify_* tag and replace with notify_none.
            std::string tags_so_far(*dyn_tags ? *dyn_tags : "");
            // Remove all ",notify_*" substrings
            while (true) {
                auto p = tags_so_far.find(",notify_");
                if (p == std::string::npos) break;
                auto end = tags_so_far.find(',', p + 1);
                if (end == std::string::npos)
                    tags_so_far.erase(p);
                else
                    tags_so_far.erase(p, end - p);
            }
            weechat_string_dyn_free(dyn_tags, 1);
            dyn_tags = weechat_string_dyn_alloc(static_cast<int>(tags_so_far.size() + 16));
            weechat_string_dyn_concat(dyn_tags, tags_so_far.c_str(), -1);
            weechat_string_dyn_concat(dyn_tags, ",notify_none", -1);
        }
        else if (channel_notify == "always")
        {
            // Append notify_highlight unconditionally (WeeChat merges duplicates).
            weechat_string_dyn_concat(dyn_tags, ",notify_highlight", -1);
        }
        // "on-mention": no override — WeeChat highlight rules handle @nick detection
    }

    const char *edit = replace ? "✏️ " : "";
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
        // PM channel: prefer a known-user prefix (avatar + colour), but fall
        // back to a plain coloured bare-JID string so the nick column is never
        // empty.
        //
        // The label shown in the nick column is always the bare JID (or nick,
        // which for PM is also bare JID). We never display a full JID with
        // resource — chatstate handlers may create user objects keyed on the
        // full JID, so looking up by full JID for avatar/colour is fine, but
        // we must NOT use such a user's display_name (which may be the full JID)
        // as the rendered label.
        const std::string_view pm_label = (nick && *nick)  ? std::string_view(nick)
                                        : from_bare         ? std::string_view(from_bare)
                                                            : std::string_view("(unknown)");

        // Try bare-JID user lookup first (roster contacts are stored by bare JID).
        auto *pm_user = user::search(&account, display_from);
        // If not found, try full-JID (chatstate-created users), but still
        // render with pm_label (bare JID) not the user's stored display_name.
        if (!pm_user && from && *from)
            pm_user = user::search(&account, from);

        if (pm_user)
        {
            std::string pfx;
            if (!pm_user->profile.avatar_rendered.empty())
                pfx = pm_user->profile.avatar_rendered + " ";
            pfx += user::as_prefix_raw(pm_label);
            display_prefix = pfx;
        }
        else
        {
            display_prefix = user::as_prefix_raw(pm_label);
        }
    }
    
    // XEP-0461: Message Replies - extract reply context
    xmpp_stanza_t *reply_elem = xmpp_stanza_get_child_by_name_and_ns(stanza, "reply", "urn:xmpp:reply:0");
    const char *reply_to_id = reply_elem ? xmpp_stanza_get_attribute(reply_elem, "id") : nullptr;
    std::string reply_prefix;      // the excerpt text for the quote line
    std::string reply_quote_nick;  // nick of the message being replied to
    
    if (reply_to_id)
    {
        // Find the original message being replied to
        void *lines = weechat_hdata_pointer(hdata_buffer,
                                            channel->buffer, "lines");
        if (lines)
        {
            void *last_line = weechat_hdata_pointer(hdata_lines,
                                                    lines, "last_line");
            while (last_line)
            {
                void *line_data = weechat_hdata_pointer(hdata_line,
                                                        last_line, "data");
                if (line_data)
                {
                    int tags_count = weechat_hdata_integer(hdata_line_data,
                                                           line_data, "tags_count");
                    std::string str_tag;
                    for (int n_tag = 0; n_tag < tags_count; n_tag++)
                    {
                        str_tag = fmt::format("{}|tags_array", n_tag);
                        const char *tag = weechat_hdata_string(hdata_line_data,
                                                               line_data, str_tag.c_str());
                        if (tag && std::string_view(tag).starts_with("id_") &&
                            weechat_strcasecmp(tag + 3, reply_to_id) == 0)
                        {
                            // Found the original message - get excerpt
                            const char *orig_message = weechat_hdata_string(
                                hdata_line_data, line_data, "message");
                            
                            if (orig_message)
                            {
                                // Extract just the text part (after tab if present)
                                std::string_view msg_sv(orig_message);
                                auto tab_pos = msg_sv.find('\t');
                                if (tab_pos != std::string_view::npos)
                                    msg_sv = msg_sv.substr(tab_pos + 1);

                                // Strip embedded color codes so we get plain text
                                std::string msg_owned(msg_sv);
                                char *plain_text = weechat_string_remove_color(msg_owned.c_str(), nullptr);
                                std::string_view clean_text = plain_text ? plain_text : msg_sv;

                                // Skip any leading reply prefix(es) (↪ …) from a prior reply chain.
                                // UTF-8 encoding of ↪ is 3 bytes: e2 86 aa.
                                // The rendered prefix format is "↪ <excerpt> " so we find the last
                                // ↪ in the string and jump past it and its excerpt word(s).
                                // Simpler: repeatedly consume "↪ <everything up to next ↪ or end>"
                                // by scanning forward for the next ↪ occurrence.
                                {
                                    constexpr std::string_view arrow = "\xE2\x86\xAA"; // ↪
                                    auto pos = clean_text.find(arrow);
                                    while (pos != std::string_view::npos)
                                    {
                                        // advance past this ↪ and any trailing spaces
                                        std::string_view after = clean_text.substr(pos + arrow.size());
                                        after = after.substr(after.find_first_not_of(' '));
                                        auto further = after.find(arrow);
                                        if (further != std::string_view::npos)
                                        {
                                            // there is another ↪ ahead — skip to it
                                            clean_text = after.substr(further);
                                            pos = 0;
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
                                    for (char c : clean_text)
                                    {
                                        if (c == '\n')
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

                                std::string excerpt = (do_truncate && clean_text.size() > 40)
                                    ? std::string(clean_text.substr(0, 40)) + "..."
                                    : std::string(clean_text);

                                if (plain_text) free(plain_text);

                                // Extract the original sender's nick from the line tags.
                                for (int nn = 0; nn < tags_count; nn++)
                                {
                                    str_tag = fmt::format("{}|tags_array", nn);
                                    const char *ntag = weechat_hdata_string(
                                        hdata_line_data, line_data, str_tag.c_str());
                                    if (ntag && std::string_view(ntag).starts_with("nick_"))
                                    {
                                        reply_quote_nick = ntag + 5;
                                        break;
                                    }
                                }

                                reply_prefix = excerpt;
                            }
                            break;
                        }
                    }
                }

                last_line = weechat_hdata_pointer(hdata_line,
                                                  last_line, "prev_line");
            }
        }
        
        // If we didn't find the original, just show reply indicator
        if (reply_prefix.empty())
        {
            reply_prefix = "[reply]";
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
            xmpp_string_guard url_text_g(account.context, xmpp_stanza_get_text(url_elem));
            const char *url_text = url_text_g.ptr;
            if (url_text)
            {
                // Optionally get description
                xmpp_stanza_t *desc_elem = xmpp_stanza_get_child_by_name(oob_x, "desc");
                xmpp_string_guard desc_text_g(account.context,
                    desc_elem ? xmpp_stanza_get_text(desc_elem) : nullptr);
                const char *desc_text = desc_text_g.ptr;
                
                // Format: [URL: url] or [URL: description (url)]
                if (desc_text && !std::string_view(desc_text).empty())
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
        if (std::string_view(ref_name) != "reference"
            || std::string_view(ref_ns) != "urn:xmpp:reference:0") continue;
        const char *ref_type = xmpp_stanza_get_attribute(ref, "type");
        if (!ref_type || std::string_view(ref_type) != "data") continue;

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

            if (name_e) { xmpp_string_guard t_g(account.context, xmpp_stanza_get_text(name_e)); if (t_g.ptr) sims_name = t_g.ptr; }
            if (mime_e) { xmpp_string_guard t_g(account.context, xmpp_stanza_get_text(mime_e)); if (t_g.ptr) sims_mime = t_g.ptr; }
            if (size_e) { xmpp_string_guard t_g(account.context, xmpp_stanza_get_text(size_e)); if (t_g.ptr) sims_size_str = t_g.ptr; }
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
                if (std::string_view(sname) == "reference"
                    && std::string_view(sns) == "urn:xmpp:reference:0")
                {
                    // XEP-0385: <reference type='data' uri='https://...'/>
                    const char *uri = xmpp_stanza_get_attribute(src, "uri");
                    if (uri) sims_url = uri;
                }
                else if (std::string_view(sname) == "url-data")
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
        if (std::string_view(fs_name) != "file-sharing"
            || std::string_view(fs_ns) != "urn:xmpp:sfs:0") continue;

        // <file xmlns='urn:xmpp:file:metadata:0'>
        xmpp_stanza_t *file_elem = xmpp_stanza_get_child_by_name_and_ns(
            fs, "file", "urn:xmpp:file:metadata:0");

        std::string sfs_name, sfs_mime, sfs_size_str;
        if (file_elem)
        {
            xmpp_stanza_t *name_e = xmpp_stanza_get_child_by_name(file_elem, "name");
            xmpp_stanza_t *mime_e = xmpp_stanza_get_child_by_name(file_elem, "media-type");
            xmpp_stanza_t *size_e = xmpp_stanza_get_child_by_name(file_elem, "size");

            if (name_e) { xmpp_string_guard t_g(account.context, xmpp_stanza_get_text(name_e)); if (t_g.ptr) sfs_name = t_g.ptr; }
            if (mime_e) { xmpp_string_guard t_g(account.context, xmpp_stanza_get_text(mime_e)); if (t_g.ptr) sfs_mime = t_g.ptr; }
            if (size_e) { xmpp_string_guard t_g(account.context, xmpp_stanza_get_text(size_e)); if (t_g.ptr) sfs_size_str = t_g.ptr; }
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
                if (std::string_view(sname) == "encrypted" && sns
                    && std::string_view(sns) == "urn:xmpp:esfs:0")
                {
                    // XEP-0448 §4: cipher attribute identifies the algorithm.
                    // We only support AES-256-GCM/NoPadding; skip other ciphers.
                    const char *cipher_attr = xmpp_stanza_get_attribute(src, "cipher");
                    if (!cipher_attr || std::string_view(cipher_attr)
                            != "urn:xmpp:ciphers:aes-256-gcm-nopadding:0")
                    {
                        XDEBUG("XEP-0448: unsupported cipher '{}', skipping encrypted source",
                               cipher_attr ? cipher_attr : "(none)");
                        break; // still prefer encrypted source, even if unsupported
                    }

                    // Extract key, iv, and inner url-data target.
                    std::string esfs_key, esfs_iv, esfs_ct_url;
                    xmpp_stanza_t *key_el = xmpp_stanza_get_child_by_name(src, "key");
                    xmpp_stanza_t *iv_el  = xmpp_stanza_get_child_by_name(src, "iv");
                    if (key_el) { xmpp_string_guard t_g(account.context, xmpp_stanza_get_text(key_el)); if (t_g.ptr) esfs_key = t_g.ptr; }
                    if (iv_el)  { xmpp_string_guard t_g(account.context, xmpp_stanza_get_text(iv_el));  if (t_g.ptr) esfs_iv  = t_g.ptr; }

                    xmpp_stanza_t *inner_sources = xmpp_stanza_get_child_by_name(src, "sources");
                    if (inner_sources)
                    {
                        for (xmpp_stanza_t *isrc = xmpp_stanza_get_children(inner_sources);
                             isrc && esfs_ct_url.empty(); isrc = xmpp_stanza_get_next(isrc))
                        {
                            const char *iname = xmpp_stanza_get_name(isrc);
                            if (iname && std::string_view(iname) == "url-data")
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

                if (std::string_view(sname) == "url-data")
                {
                    const char *target = xmpp_stanza_get_attribute(src, "target");
                    if (target) sfs_url = target;
                }
                else if (std::string_view(sname) == "reference")
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
            if (std::string_view(desc_name) != "Description"
                || std::string_view(desc_ns) != "http://www.w3.org/1999/02/22-rdf-syntax-ns#")
                continue;

            std::string og_title, og_desc, og_url, og_image;
            for (xmpp_stanza_t *prop = xmpp_stanza_get_children(desc);
                 prop; prop = xmpp_stanza_get_next(prop))
            {
                const char *prop_name = xmpp_stanza_get_name(prop);
                const char *prop_ns   = xmpp_stanza_get_ns(prop);
                if (!prop_name || !prop_ns) continue;
                if (std::string_view(prop_ns) != "https://ogp.me/ns#") continue;

                xmpp_string_guard val_g(account.context, xmpp_stanza_get_text(prop));
                const char *val = val_g.ptr;
                if (!val) continue;

                if (std::string_view(prop_name) == "title" && og_title.empty())
                    og_title = val;
                else if (std::string_view(prop_name) == "description" && og_desc.empty())
                    og_desc = val;
                else if (std::string_view(prop_name) == "url" && og_url.empty())
                    og_url = val;
                else if (std::string_view(prop_name) == "image" && og_image.empty())
                {
                    // Only store HTTP(S) image URLs; skip cid:, ni:, data: URIs
                    if (std::string_view(val).starts_with("http"))
                        og_image = val;
                }
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
    if (text && !has_unstyled)  // Apply styling
    {
        // XEP-0394: Message Markup takes precedence over XEP-0393 ad-hoc styling.
        styled_text = apply_xep394_markup(stanza, text);
        if (styled_text.empty())
            styled_text = apply_xep393_styling(text);
        display_text = styled_text.c_str();
    }

    std::string final_text; // used by spoiler / ephemeral / oob suffix blocks below

    // XEP-0461: emit reply context as a separate quote line above the message.
    // Nick column shows the replying user (same as the message line below it).
    // Body shows  │ quotedNick: excerpt  in dim/cyan so it reads as a quote block.
    // notify_none,no_log: no highlight, no duplicate log entry.
    if (!reply_prefix.empty())
    {
        std::string quote_line = std::string(weechat_color("darkgray"))
            + "│ "
            + weechat_color("cyan");
        if (!reply_quote_nick.empty())
            quote_line += reply_quote_nick + weechat_color("darkgray") + ": ";
        quote_line += weechat_color("darkgray") + reply_prefix + weechat_color("resetcolor");
        weechat_printf_date_tags(channel->buffer, date,
            "notify_none,no_log,xmpp_reply_quote",
            "%s\t%s", display_prefix.data(), quote_line.c_str());
    }

    // XEP-0382: Spoiler Messages — prepend spoiler warning before the body
    if (spoiler_hint)
    {
        std::string spoiler_prefix = std::string(weechat_color("yellow"))
            + "[Spoiler"
            + (spoiler_hint && !std::string_view(spoiler_hint).empty()
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
        weechat_printf_date_tags(channel->buffer, date, *dyn_tags, "%s\t%s[to %s]: %s%s",
                                 display_prefix.data(),
                                 edit, to, encrypted_glyph,
                                 display_text ? display_text : "");
    else if (weechat_string_match(text, "/me *", 0))
        weechat_printf_date_tags(channel->buffer, date, *dyn_tags, "%s\t%s%s %s%s",
                                 weechat_prefix("action"),
                                 edit, display_prefix.data(),
                                 encrypted_glyph,
                                 display_text ? display_text+4 : "");
    else
        weechat_printf_date_tags(channel->buffer, date, *dyn_tags, "%s\t%s%s%s",
                                 display_prefix.data(),
                                 edit, encrypted_glyph,
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

xmpp_stanza_t *weechat::connection::get_caps(xmpp_stanza_t *reply, std::optional<std::string> *hash, const char *node)
{
    // Build <query xmlns='http://jabber.org/protocol/disco#info'> via spec builder.
    struct query_spec : stanza::spec {
        query_spec(const char *n) : spec("query") {
            attr("xmlns", "http://jabber.org/protocol/disco#info");
            if (n && *n) attr("node", n);
        }
    } qs(node);
    auto query_sp = qs.build(account.context);
    xmpp_stanza_t *query = query_sp.get();

    std::unique_ptr<char, decltype(&free)> client_name(
            weechat_string_eval_expression(
                "weechat ${info:version}", nullptr, nullptr, nullptr),
            &free);
    char **serial = weechat_string_dyn_alloc(256);
    weechat_string_dyn_concat(serial, "client/pc//", -1);
    weechat_string_dyn_concat(serial, client_name.get(), -1);
    weechat_string_dyn_concat(serial, "<", -1);

    // Build <identity category='client' name='...' type='pc'/> via spec builder.
    struct identity_spec : stanza::spec {
        identity_spec(const char *name) : spec("identity") {
            attr("category", "client");
            attr("name", name ? name : "");
            attr("type", "pc");
        }
    } ids(client_name.get());
    auto id_sp = ids.build(account.context);
    xmpp_stanza_add_child(query, id_sp.get());

    const std::vector<std::string_view> advertised_features {
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
         "urn:xmpp:occupant-id:0",
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
        // XEP-0413: Order-By — bare feature plus scoped variants (§6)
        "urn:xmpp:order-by:1",
        "urn:xmpp:order-by:1@urn:xmpp:mam:2",
        "urn:xmpp:order-by:1@http://jabber.org/protocol/pubsub",
        // XEP-0466: Ephemeral Messages
        "urn:xmpp:ephemeral:0",
        // XEP-0448: Encrypted File Sharing
        "urn:xmpp:esfs:0",
        // Legacy OMEMO (eu.siacs.conversations.axolotl) PEP push subscription.
        // Advertising this tells the server to push devicelist updates to us,
        // and signals to Conversations/Gajim that we speak axolotl-namespace OMEMO.
        "eu.siacs.conversations.axolotl.devicelist+notify",
    };

    std::vector<std::string> sorted_features;
    sorted_features.reserve(advertised_features.size());
    for (const auto feature : advertised_features)
        sorted_features.emplace_back(feature);
    std::sort(sorted_features.begin(), sorted_features.end());

    for (const auto &ns : sorted_features)
    {
        struct feature_spec : stanza::spec {
            feature_spec(const char *v) : spec("feature") { attr("var", v); }
        } fs(ns.c_str());
        auto fsp = fs.build(account.context);
        xmpp_stanza_add_child(query, fsp.get());

        // XEP-0115 hash input must use sorted features.
        weechat_string_dyn_concat(serial, ns.c_str(), -1);
        weechat_string_dyn_concat(serial, "<", -1);
    }

    // Build <x xmlns='jabber:x:data' type='result'> via spec builder.
    struct xdata_spec : stanza::spec {
        xdata_spec() : spec("x") {
            attr("xmlns", "jabber:x:data");
            attr("type", "result");
        }
    } xs;
    auto x_sp = xs.build(account.context);
    xmpp_stanza_t *x = x_sp.get();

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
        if (std::string_view(var) == "FORM_TYPE") {
            weechat_string_dyn_concat(serial, var, -1);
            weechat_string_dyn_concat(serial, "<", -1);
        }
        weechat_string_dyn_concat(serial, val, -1);
        weechat_string_dyn_concat(serial, "<", -1);
    };
    // Add a two-value x-data field via builder and append both values to the serial.
    auto add_feature2 = [&](const char *var, const char *type,
                            const char *val1, const char *val2) {
        // Build <field var='...' type='...'><value>val1</value><value>val2</value></field>
        struct value_spec2 : stanza::spec {
            explicit value_spec2(const char *v) : spec("value") { text(v); }
        };
        struct field_spec2 : stanza::spec {
            field_spec2(const char *var_, const char *type_,
                        const char *v1, const char *v2)
                : spec("field")
            {
                attr("var", var_);
                if (type_) attr("type", type_);
                value_spec2 vs1(v1);
                value_spec2 vs2(v2);
                child(vs1);
                child(vs2);
            }
        } fs2(var, type, val1, val2);
        auto fsp2 = fs2.build(account.context);
        xmpp_stanza_add_child(x, fsp2.get());
        weechat_string_dyn_concat(serial, var,  -1);
        weechat_string_dyn_concat(serial, "<",  -1);
        weechat_string_dyn_concat(serial, val1, -1);
        weechat_string_dyn_concat(serial, "<",  -1);
        weechat_string_dyn_concat(serial, val2, -1);
        weechat_string_dyn_concat(serial, "<",  -1);
    };

    add_feature1("FORM_TYPE", "hidden", "urn:xmpp:dataforms:softwareinfo");
    add_feature2("ip_version", "text-multi", "ipv4", "ipv6");
    // XEP-0092 §5 MUST NOT: an application MUST provide a way to disable
    // sharing OS information.  Gate on look.share_os_info (default on).
    if (weechat_config_boolean(weechat::config::instance->look.share_os_info))
    {
        add_feature1("os",         nullptr, osinfo.sysname);
        add_feature1("os_version", nullptr, osinfo.release);
    }
    add_feature1("software",         nullptr, "weechat");
    add_feature1("software_version", nullptr, weechat_info_get("version", nullptr));

    xmpp_stanza_add_child(query, x);

    xmpp_stanza_set_type(reply, "result");
    xmpp_stanza_add_child(reply, query);

    // XEP-0115 requires SHA-1; use EVP_Digest (present in both OpenSSL and
    // LibreSSL) instead of the libstrophe-specific xmpp_sha1_* API.
    unsigned char digest[20];
    unsigned int digest_len = sizeof(digest);
    std::string_view serial_sv(*serial);
    EVP_Digest(serial_sv.data(), serial_sv.size(), digest, &digest_len,
               EVP_sha1(), nullptr);
    weechat_string_dyn_free(serial, 1);

    if (hash)
    {
        const int encoded_size = 4 * static_cast<int>((digest_len + 2) / 3) + 1;
        std::string encoded(static_cast<std::size_t>(encoded_size), '\0');
        const int written = weechat_string_base_encode(
            "64", reinterpret_cast<const char *>(digest),
            static_cast<int>(digest_len), encoded.data());
        if (written > 0) {
            encoded.resize(static_cast<std::size_t>(written));
            *hash = std::move(encoded);
        }
    }

    return reply;
}

// XEP-0004: Data Forms — render a <x xmlns='jabber:x:data'> form to a buffer
void render_data_form(struct t_gui_buffer *buf, xmpp_stanza_t *x_form,
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
        if (!fname || std::string_view(fname) != "field") continue;
        const char *var   = xmpp_stanza_get_attribute(field, "var");
        const char *label = xmpp_stanza_get_attribute(field, "label");
        const char *ftype = xmpp_stanza_get_attribute(field, "type");

        // Skip hidden fields — not user-visible
        if (ftype && std::string_view(ftype) == "hidden") continue;

        field_index++;

        // Collect all <value> children (text-multi and list-multi can have several)
        std::vector<std::string> values;
        for (xmpp_stanza_t *v = xmpp_stanza_get_children(field);
             v; v = xmpp_stanza_get_next(v))
        {
            const char *vname = xmpp_stanza_get_name(v);
            if (!vname || std::string_view(vname) != "value") continue;
            const char *vtext = xmpp_stanza_get_text_ptr(v);
            if (vtext) values.push_back(vtext);
        }

        // Check if field is required
        bool required = (xmpp_stanza_get_child_by_name(field, "required") != nullptr);

        // Determine display value string
        bool is_password = ftype && std::string_view(ftype) == "text-private";
        bool is_boolean  = ftype && std::string_view(ftype) == "boolean";
        bool is_fixed    = ftype && std::string_view(ftype) == "fixed";
        bool is_list     = ftype && (std::string_view(ftype) == "list-single"
                                  || std::string_view(ftype) == "list-multi");

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
                if (!oname || std::string_view(oname) != "option") continue;

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
                if (olabel && oval && std::string_view(olabel) != std::string_view(oval))
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
            if (std::string_view(ftype) == "text-single" || std::string_view(ftype) == "text-multi")
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
