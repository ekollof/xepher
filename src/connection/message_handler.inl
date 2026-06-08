bool weechat::connection::message_handler(xmpp_stanza_t *stanza, bool top_level, bool is_mam_replay,
                                          std::string_view override_archive_id,
                                          std::string_view override_delay_stamp)
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

    // Helper: match a buffer-line tag against a target message ID regardless of
    // which ID namespace was used to tag the line.  Lines receive up to three
    // ID tags on live delivery:
    //   id_<stable_id>       — stable_id = stanza_id ?? origin_id ?? msg_id
    //   stanza_id_<sid>      — server-assigned archive ID (XEP-0359)
    //   origin_id_<oid>      — sender-assigned stable ID (XEP-0359)
    // On MAM replay the server strips <stanza-id> from the forwarded copy, so
    // stable_id resolves to origin_id or msg_id, and id_ will NOT equal the
    // archive ID.  XEP-0461 <reply>, XEP-0424 <retract>, XEP-0308 <replace>,
    // XEP-0425 moderation, and XEP-0444 <reactions> all carry stanza-IDs, so
    // without this broadened check they silently fail to find their target line
    // during MAM replay.
    weechat::channel *channel, *parent_channel;
    xmpp_stanza_t *x, *body, *delay, *topic, *result, *forwarded, *event, *items, *item, *encrypted;
    const char *type, *from, *nick, *from_bare, *to, *to_bare, *id, *thread, *replace_id, *timestamp;
    const char *text = nullptr;
    std::string intext_storage;
    auto intext_ptr = [&]() -> const char * {
        return intext_storage.empty() ? nullptr : intext_storage.c_str();
    };
    std::string from_bare_main_storage; // owns main from_bare string
    std::string to_bare_main_storage;   // owns main to_bare string
    // Cache own JID for use when stanza 'from' is absent (e.g. self-messages).
    // account.jid() returns a temporary; taking .data() on it is a dangling pointer.
    const std::string own_jid_str = account.jid();
     std::string omemo_cleartext_storage; // owns OMEMO-decrypted text
     std::string pgp_cleartext_storage; // owns PGP-decrypted text (avoids strdup)
     char *cleartext = nullptr;
    struct tm time = {0};
    time_t date = 0;

    auto binding = std::make_unique<xml::message>(account.context, stanza);
    body = xmpp_stanza_get_child_by_name(stanza, "body");
    xmpp_stanza_t *encrypted_without_body =
        ::xmpp::stanza_axolotl_encrypted(::xmpp::StanzaView(stanza)).raw();
    xmpp_stanza_t *pgp_without_body = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "x", "jabber:x:encrypted");

    // Payloadless OMEMO stanzas are control/key-transport messages and must
    // not create PM buffers for inactive roster contacts.
    if (body == nullptr && encrypted_without_body
        && !::xmpp::stanza_has_user_message_payload(::xmpp::StanzaView(stanza)))
        return 1;

    if (body == nullptr && !encrypted_without_body && !pgp_without_body)
    {
        topic = xmpp_stanza_get_child_by_name(stanza, "subject");
        if (topic != nullptr)
        {
            intext_storage = stanza_element_text(topic);
            type = xmpp_stanza_get_type(stanza);
        if (::xmpp::stanza_is_error_message(::xmpp::StanzaView(stanza)))
            return 1;
        from = xmpp_stanza_get_from(stanza);
        if (from == nullptr)
            return 1;
        std::string from_bare_s = ::jid(nullptr, from).bare;
            std::string from_resource_s = ::jid(nullptr, from).resource;
            from_bare = from_bare_s.c_str();
            from = from_resource_s.c_str();
            channel = nullptr;
            if (auto it = account.channels.find(from_bare); it != account.channels.end())
            {
                auto& [_, ch] = *it;
                channel = &ch;
            }
            if (!channel)
            {
                if (weechat_strcasecmp(type, "groupchat") == 0)
                {
                    auto [it_muc, _ins_muc] = account.channels.emplace(
                        std::make_pair(from_bare, weechat::channel {
                                account, weechat::channel::chat_type::MUC, from_bare, from_bare
                            }));
                    auto& [_, ch_muc] = *it_muc;
                    channel = &ch_muc;
                }
                else
                {
                    auto [it_pm, _ins_pm] = account.channels.emplace(
                        std::make_pair(from_bare, weechat::channel {
                                account, weechat::channel::chat_type::PM, from_bare, from_bare
                            }));
                    auto& [_, ch_pm] = *it_pm;
                    channel = &ch_pm;
                }
            }
            channel->update_topic(intext_storage.c_str(), from, 0);
            intext_storage.clear();
        }

        // XEP-0085: Chat State Notifications
        {
            const auto view = ::xmpp::StanzaView(stanza);
            if (::xmpp::stanza_has_chat_state(view))
            {
                if (!view.from())
                    return 1;

                auto chat_state = ::xmpp::parse_incoming_chat_state(view);
                if (!chat_state)
                    return 1;

                const std::string cs_from_bare_s = ::jid(nullptr, chat_state->from.c_str()).bare;
                const std::string cs_nick_s = ::jid(nullptr, chat_state->from.c_str()).resource;
                from_bare = cs_from_bare_s.c_str();
                nick = cs_nick_s.c_str();
                channel = nullptr;
                if (auto it = account.channels.find(cs_from_bare_s); it != account.channels.end())
                {
                    auto& [_, ch] = *it;
                    channel = &ch;
                }
                if (!channel)
                    return 1;

                const char *from_full = chat_state->from.c_str();
                auto user = user::search(&account, from_full);
                if (!user)
                {
                    auto [it_usr, _ins_usr] = account.users.emplace(
                        std::piecewise_construct,
                        std::forward_as_tuple(from_full),
                        std::forward_as_tuple(
                            &account, channel, from_full,
                            weechat_strcasecmp(from_bare, channel->id.data()) == 0 ? nick : from_full));
                    auto& [_, u] = *it_usr;
                    user = &u;
                }

                channel->mark_chat_state_supported(chat_state->from);
                channel->mark_chat_state_supported(cs_from_bare_s);

                if (::xmpp::typing_action_for_state(chat_state->state) == ::xmpp::TypingAction::show)
                    channel->add_typing(user);
                else
                    channel->remove_typing(user);
            }
        }

        // XEP-0280: Message Carbons — unwrap and recurse into inner message
        {
            const auto view = ::xmpp::StanzaView(stanza);
            if (::xmpp::stanza_is_carbon(view))
            {
                if (auto inner = ::xmpp::parse_carbon_inner_message(view, account.jid()))
                    return message_handler(*inner, false);
                return 1;
            }
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
                            const std::string mmn_text_str = stanza_element_text(mmn_body);
                            const char *mmn_text = mmn_text_str.empty()
                                ? nullptr : mmn_text_str.c_str();

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
            const auto mam_view = ::xmpp::StanzaView(stanza);

            // XEP-0442: pubsub MAM result — forwarded stanza contains a pubsub
            // <event> with the original item payload. Process it here so we can
            // build the correct feed_key from the query context (service JID from
            // our IQ) rather than from the inner message's from= attribute.
            const auto pubsub_queryid = ::xmpp::mam_pubsub_query_id(mam_view);
            if (pubsub_queryid && account.pubsub_mam_queries.contains(*pubsub_queryid))
            {
                const auto &pq = account.pubsub_mam_queries.at(*pubsub_queryid);
                std::string feed_service = pq.service;
                std::string node_name    = pq.node;
                std::string feed_key     = fmt::format("{}/{}", feed_service, node_name);

                auto [ch_it, inserted] = account.channels.try_emplace(
                    feed_key,
                    account, weechat::channel::chat_type::FEED,
                    feed_key, feed_key);
                auto& [_, feed_ch] = *ch_it;
                if (inserted)
                    account.feed_open_register(feed_key);

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
            if (auto dispatch = ::xmpp::parse_mam_forwarded_dispatch(mam_view))
            {
                xmpp_stanza_t *message = dispatch->message;
                const auto msg_view = ::xmpp::StanzaView(message);

                const char *debug_from = xmpp_stanza_get_from(message);
                const char *debug_to = xmpp_stanza_get_to(message);
                const char *debug_type = xmpp_stanza_get_type(message);

                const std::string from_bare_s = debug_from ? ::jid(nullptr, debug_from).bare : std::string{};
                const std::string to_bare_s = debug_to ? ::jid(nullptr, debug_to).bare : std::string{};
                const auto partner = ::xmpp::mam_conversation_partner_jid(
                    from_bare_s, to_bare_s, account.jid());

                ::xmpp::MamPmDiscoveryPolicy discovery;
                discovery.is_groupchat = debug_type
                    && weechat_strcasecmp(debug_type, "groupchat") == 0;
                discovery.has_partner_jid = partner.has_value();
                discovery.channel_already_exists = partner && account.channels.contains(*partner);
                discovery.deliberately_closed = partner
                    && account.mam_cache_get_last_timestamp(*partner) == static_cast<time_t>(-1);
                discovery.has_user_payload = ::xmpp::stanza_has_user_message_payload(msg_view);

                if (::xmpp::should_discover_pm_channel_from_mam(discovery))
                {
                    XDEBUG("MAM: discovered conversation with {}", *partner);
                    account.channels.emplace(
                        std::make_pair(*partner, weechat::channel {
                                account, weechat::channel::chat_type::PM,
                                *partner, *partner
                            }));
                }

                const char *msg_id = xmpp_stanza_get_id(message);
                const char *msg_from = debug_from;
                const char *msg_to = debug_to;
                const std::string msg_text_str = msg_view.child("body").text();
                const char *msg_text = msg_text_str.empty() ? nullptr : msg_text_str.c_str();
                const time_t msg_timestamp = ::xmpp::parse_forward_delay_stamp(dispatch->delay_stamp);

                if (msg_id && msg_from && msg_timestamp > 0)
                {
                    const std::string from_bare_s2 = ::jid(nullptr, msg_from).bare;
                    const std::string to_bare_s2 = msg_to ? ::jid(nullptr, msg_to).bare : std::string{};
                    const auto channel_jid_opt = ::xmpp::mam_channel_jid_for_addresses(
                        from_bare_s2, to_bare_s2, account.jid());
                    const char *channel_jid = channel_jid_opt
                        ? channel_jid_opt->c_str() : from_bare_s2.c_str();

                    std::expected<std::string, std::string> omemo_body = std::unexpected("");
                    if (!msg_text || std::string_view(msg_text) == OMEMO_ADVICE)
                        omemo_body = account.mam_cache_lookup_omemo_plaintext(channel_jid, msg_id);

                    const char *effective_body = (omemo_body && !omemo_body->empty())
                        ? omemo_body->c_str()
                        : (msg_text ? msg_text : nullptr);

                    if (effective_body)
                    {
                        const std::string cache_from = ::xmpp::mam_cache_from_label(
                            debug_type ? debug_type : "",
                            msg_from ? msg_from : "",
                            from_bare_s2);
                        account.mam_cache_message(channel_jid, msg_id, cache_from,
                                                  msg_timestamp, effective_body);
                    }
                }

                if (!dispatch->archive_id.empty() || (msg_id && *msg_id))
                {
                    const auto dedup_needles = ::xmpp::mam_dedup_needles(
                        dispatch->archive_id, msg_id ? msg_id : "");
                    const auto channel_jid_chk = ::xmpp::mam_channel_jid_for_addresses(
                        msg_from ? ::jid(nullptr, msg_from).bare : std::string_view{},
                        msg_to ? ::jid(nullptr, msg_to).bare : std::string_view{},
                        account.jid());

                    struct t_gui_buffer *chk_buf = nullptr;
                    if (channel_jid_chk)
                    {
                        if (auto it = account.channels.find(*channel_jid_chk); it != account.channels.end())
                        {
                            auto& [_, ch] = *it;
                            chk_buf = ch.buffer;
                        }
                    }

                    if (chk_buf)
                    {
                        if (!dedup_needles.stanza_id_needle.empty()
                            && !dedup_needles.message_id_needle.empty())
                        {
                            if (weechat::line_store_buffer_contains_any_tag(
                                    chk_buf,
                                    { dedup_needles.stanza_id_needle,
                                      dedup_needles.message_id_needle }))
                                return 1;
                        }
                        else if (!dedup_needles.stanza_id_needle.empty())
                        {
                            if (weechat::line_store_buffer_contains_any_tag(
                                    chk_buf, { dedup_needles.stanza_id_needle }))
                                return 1;
                        }
                        else if (!dedup_needles.message_id_needle.empty())
                        {
                            if (weechat::line_store_buffer_contains_any_tag(
                                    chk_buf, { dedup_needles.message_id_needle }))
                                return 1;
                        }
                    }
                }

                return message_handler(
                    message, false, true,
                    dispatch->archive_id.empty() ? std::string_view{} : dispatch->archive_id,
                    dispatch->delay_stamp.empty() ? std::string_view{} : dispatch->delay_stamp);
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
                           from ? from : own_jid_str.c_str(),
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
                                     const char *from_jid = from ? from : own_jid_str.c_str();

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
                                    
                                    const char *from_jid = from ? from : own_jid_str.c_str();
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
                        const char *event_from = from ? from : own_jid_str.c_str();
                        std::string own_bare_s  = ::jid(nullptr, own_jid_str.c_str()).bare;
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
                std::string_view feed_service_sv = from ? std::string_view(from) : std::string_view{};
                std::string_view node_sv          = items_node ? std::string_view(items_node) : std::string_view{};
                // XEP-0472/XEP-0277: urn:xmpp:microblog:0 is the PEP microblog node — treat it
                // as a social feed even though it starts with "urn:".
                // urn:xmpp:microblog:0:comments/<uuid> are comment thread nodes — also feeds.
                bool node_is_microblog = (node_sv == "urn:xmpp:microblog:0")
                    || (node_sv.starts_with("urn:xmpp:microblog:0:comments/"));
                // Treat reversed-domain protocol namespaces (e.g. eu.siacs.conversations.axolotl.*)
                // the same as urn: URIs — they are OMEMO/PEP protocol nodes, not user feeds.
                // This is especially important now that MUC OMEMO causes us to request
                // devicelists/bundles for room occupants, generating extra legacy OMEMO PEP traffic.
                bool node_is_protocol_ns = !node_sv.empty() && !node_is_microblog && (
                    node_sv.starts_with("eu.siacs.") ||
                    node_sv.starts_with("com.google.") ||
                    node_sv.starts_with("org.jivesoftware."));
                bool node_is_uri = !node_sv.empty() && !node_is_microblog && (
                    node_sv.contains("://") ||
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

                // Explicitly skip legacy OMEMO protocol nodes (eu.siacs.conversations.axolotl.*).
                // These arrive as PEP pushes (especially after MUC OMEMO occupant discovery)
                // and must never be turned into user-visible FEED buffers.
                bool is_legacy_omemo_node = !node_sv.empty() &&
                    (node_sv.starts_with("eu.siacs.conversations.axolotl"));

                if (is_legacy_omemo_node) {
                    XDEBUG("Dropping PEP event for legacy OMEMO node (not a user feed): {} from {}",
                           node_sv, feed_service_sv);
                }

                if (!node_sv.empty() && !feed_service_sv.empty() && !node_is_uri && !from_self
                    && !is_legacy_omemo_node
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
                                    bool first = true;
                                    std::ranges::for_each(ae.categories, [&](const auto &cat) {
                                        if (!first) tags += ", ";
                                        first = false;
                                        tags += cat;
                                    });
                                    weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                        "  %sTags:%s %s",
                                        dim, rst, tags.c_str());
                                }

                                std::ranges::for_each(ae.enclosures, [&](const auto &enclosure) {
                                    weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                        "  %sAttachment:%s %s",
                                        dim, rst, enclosure.c_str());
                                });

                                for (const auto &att : ae.attachments)
                                {
                                    bool is_image = !att.media_type.empty() &&
                                                    att.media_type.starts_with("image/");
                                    bool is_video = !att.media_type.empty() &&
                                                    att.media_type.starts_with("video/");
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

        // XEP-0184 / XEP-0333: inbound delivery receipt or displayed marker
        {
            const auto view = ::xmpp::StanzaView(stanza);
            if (::xmpp::stanza_is_receipt_ack(view))
            {
                if (auto ack = ::xmpp::parse_incoming_receipt(view))
                {
                    const std::string bare_s = ::jid(nullptr, ack->from.c_str()).bare;
                    weechat::channel *ch = nullptr;
                    if (auto ch_it = account.channels.find(bare_s); ch_it != account.channels.end())
                    {
                        auto& [_, c] = *ch_it;
                        ch = &c;
                    }
                    if (ch && ch->type != weechat::channel::chat_type::MUC)
                    {
                        (void)weechat::line_store_update_line_glyph_by_tag(
                            ch->buffer, ack->acked_id, weechat::k_glyph_delivered);
                    }
                }
                return 1;
            }
            if (::xmpp::stanza_is_displayed_ack(view))
            {
                if (auto ack = ::xmpp::parse_incoming_displayed(view))
                {
                    const std::string bare_s = ::jid(nullptr, ack->from.c_str()).bare;
                    weechat::channel *ch = nullptr;
                    if (auto ch_it = account.channels.find(bare_s); ch_it != account.channels.end())
                    {
                        auto& [_, c] = *ch_it;
                        ch = &c;
                    }
                    if (ch && ch->type != weechat::channel::chat_type::MUC)
                    {
                        (void)weechat::line_store_update_line_glyph_by_tag(
                            ch->buffer, ack->acked_id, weechat::k_glyph_seen);
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
                    weechat::channel *ch = nullptr;
                    if (auto ch_it = account.channels.find(bare_s); ch_it != account.channels.end())
                    {
                        auto& [_, c] = *ch_it;
                        ch = &c;
                    }
                    if (!ch)
                    {
                        // Create PM channel for the buzz
                        auto [it_ch, _ins] = account.channels.emplace(
                            std::make_pair(bare_s, weechat::channel {
                                    account, weechat::channel::chat_type::PM, bare_s, bare_s
                                }));
                        auto& [_, c] = *it_ch;
                        ch = &c;
                    }
                    weechat_printf_date_tags(ch->buffer, 0, "xmpp_attention,notify_highlight",
                                            "%s%s is requesting your attention! (/buzz)",
                                            weechat_prefix("network"),
                                            bare_s.c_str());
                }
                return 1;
            }
        }

        // XEP-0437: Room Activity Indicators — handle <rai> notifications
        {
            xmpp_stanza_t *rai = xmpp_stanza_get_child_by_name_and_ns(
                stanza, "rai", "urn:xmpp:rai:0");
            if (rai)
            {
                for (xmpp_stanza_t *act = xmpp_stanza_get_child_by_name(rai, "activity");
                     act; act = xmpp_stanza_get_next(act))
                {
                    const char *act_name = xmpp_stanza_get_name(act);
                    if (!act_name || weechat_strcasecmp(act_name, "activity") != 0)
                        continue;
                    const std::string jid_str = stanza_element_text(act);
                    if (jid_str.empty())
                        continue;
                    const char *jid = jid_str.c_str();

                    // Look up the room buffer; if it exists, bump hotlist and
                    // print the notification in-context.  Otherwise fall back
                    // to the account buffer.
                    weechat::channel *rai_ch = nullptr;
                    if (auto rai_it = account.channels.find(jid);
                        rai_it != account.channels.end())
                    {
                        auto& [_, rch] = *rai_it;
                        rai_ch = &rch;
                    }

                    if (rai_ch && rai_ch->buffer)
                    {
                        weechat_buffer_set(rai_ch->buffer, "hotlist",
                                           WEECHAT_HOTLIST_MESSAGE);
                        weechat_printf_date_tags(rai_ch->buffer, 0,
                                                   "xmpp_rai,notify_message",
                                                   _("%sRoom activity detected"),
                                                   weechat_prefix("network"));
                    }
                    else
                    {
                        weechat_printf(account.buffer,
                                       _("%sRoom activity: %s%s%s"),
                                       weechat_prefix("network"),
                                       weechat_color("chat_nick_self"),
                                       jid,
                                       weechat_color("reset"));
                    }
                }
                return 1;
            }
        }

        return 1;
    }
    type = xmpp_stanza_get_type(stanza);
    if (::xmpp::stanza_is_error_message(::xmpp::StanzaView(stanza)))
        return 1;
    from = xmpp_stanza_get_from(stanza);
    if (from == nullptr)
        return 1;
    from_bare_main_storage = ::jid(nullptr, from).bare;
    from_bare = from_bare_main_storage.c_str();
    to = xmpp_stanza_get_to(stanza);
    if (to == nullptr)
        to = own_jid_str.c_str();
    to_bare_main_storage = to ? ::jid(nullptr, to).bare : std::string{};
    to_bare = !to_bare_main_storage.empty() ? to_bare_main_storage.c_str() : nullptr;
    const bool is_self_outbound_copy = from_bare && to_bare
        && weechat_strcasecmp(from_bare, account.jid().data()) == 0
        && weechat_strcasecmp(to_bare, account.jid().data()) != 0;
    id = xmpp_stanza_get_id(stanza);
    bool was_omemo_cached = false;  // set when plaintext came from omemo_plaintext cache
    thread = xmpp_stanza_get_attribute(stanza, "thread");
    
    // XEP-0359: Unique and Stable Stanza IDs
    xmpp_stanza_t *stanza_id_elem = xmpp_stanza_get_child_by_name_and_ns(stanza, "stanza-id", "urn:xmpp:sid:0");
    const char *stanza_id = !override_archive_id.empty() ? override_archive_id.data()
                            : stanza_id_elem ? xmpp_stanza_get_attribute(stanza_id_elem, "id") : nullptr;
    const char *stanza_id_by = stanza_id_elem ? xmpp_stanza_get_attribute(stanza_id_elem, "by") : nullptr;
    
    xmpp_stanza_t *origin_id_elem = xmpp_stanza_get_child_by_name_and_ns(stanza, "origin-id", "urn:xmpp:sid:0");
    const char *origin_id = origin_id_elem ? xmpp_stanza_get_attribute(origin_id_elem, "id") : nullptr;
    
    // Prefer origin-id (client-assigned) over stanza-id (server-assigned)
    // over the message id attribute. The omemo_plaintext cache is keyed by
    // the client-assigned id (saved_id in the send path), so using origin-id
    // first ensures cache hits for self-sent OMEMO messages.
    const std::string stable_id_storage = ::xmpp::omemo_stable_id({
        origin_id ? std::string_view(origin_id) : std::string_view{},
        stanza_id ? std::string_view(stanza_id) : std::string_view{},
        id ? std::string_view(id) : std::string_view{}});
    const char *stable_id = stable_id_storage.empty() ? nullptr : stable_id_storage.c_str();
    
    const auto correction = ::xmpp::parse_message_correction(::xmpp::StanzaView(stanza));
    const bool has_message_correction = correction.has_value();
    std::string correction_target_storage = correction ? correction->target_id : std::string{};
    replace_id = correction_target_storage.empty()
        ? nullptr : correction_target_storage.c_str();
    const auto msg_view = ::xmpp::StanzaView(stanza);
    const bool receipt_requested = ::xmpp::stanza_requests_receipt(msg_view);
    const bool marker_markable = ::xmpp::stanza_is_marker_markable(msg_view);
    const bool is_delayed_delivery = ::xmpp::stanza_is_delayed_delivery(msg_view);

    const char *channel_id = account.jid() == from_bare ? to_bare : from_bare;
    parent_channel = nullptr;
    if (auto parent_ch_it = account.channels.find(channel_id); parent_ch_it != account.channels.end())
    {
        auto& [_, ch] = *parent_ch_it;
        parent_channel = &ch;
    }
    const char *pm_id = account.jid() == from_bare ? to : from;
    channel = parent_channel;
    if (!channel)
    {
        auto [it_ch, _ins_ch] = account.channels.emplace(
            std::make_pair(channel_id, weechat::channel {
                    account,
                    weechat_strcasecmp(type, "groupchat") == 0
                        ? weechat::channel::chat_type::MUC : weechat::channel::chat_type::PM,
                    channel_id, channel_id
                }));
        auto& [_, ch_new] = *it_ch;
        channel = &ch_new;
    }
    if (channel && channel->type == weechat::channel::chat_type::MUC
        && weechat_strcasecmp(type, "chat") == 0)
    {
        auto [it_pm, _ins_pm] = account.channels.emplace(
            std::make_pair(pm_id, weechat::channel {
                    account, weechat::channel::chat_type::PM,
                    pm_id, pm_id
                }));
        auto& [_, ch_pm] = *it_pm;
        channel = &ch_pm;
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
    if (id)
    {
        ::xmpp::AckReplyInput ack_input;
        ack_input.message_id = id;
        ack_input.reply_to = from;
        ack_input.message_type = type ? type : "chat";
        ack_input.receipt_requested = receipt_requested;
        ack_input.marker_markable = marker_markable;
        if (thread)
            ack_input.thread = thread;
        if (stanza_id)
            ack_input.stanza_id = stanza_id;
        if (stanza_id_by)
            ack_input.stanza_id_by = stanza_id_by;

        ::xmpp::AckReplySuppress suppress;
        suppress.self_outbound_copy = is_self_outbound_copy;
        suppress.muc_channel = is_muc_channel;
        suppress.mam_replay = is_mam_replay;
        suppress.delayed_delivery = is_delayed_delivery;

        if (auto ack_reply = ::xmpp::build_ack_reply(ack_input, suppress))
        {
            account.connection.send(ack_reply->reply.build(account.context).get());

            weechat::channel::unread unread;
            unread.id = ack_reply->unread.id;
            unread.thread = ack_reply->unread.thread;
            unread.stanza_id = ack_reply->unread.stanza_id;
            unread.stanza_id_by = ack_reply->unread.stanza_id_by;
            channel->unreads.push_back(unread);
        }
    }

    // XEP-0249: Direct MUC Invitations
    if (auto invite = ::xmpp::parse_direct_muc_invite(::xmpp::StanzaView(stanza)))
    {
        weechat_printf(account.buffer,
                      _("%s%s invited you to %s%s%s"),
                      weechat_prefix("network"),
                      invite->inviter_bare.c_str(),
                      invite->room_jid.c_str(),
                      invite->reason ? " (" : "",
                      invite->reason ? invite->reason->c_str() : "");
        if (invite->reason)
            weechat_printf(account.buffer, "%s)", "");
        weechat_printf(account.buffer,
                      _("%sTo join: /join %s%s%s"),
                      weechat_prefix("network"),
                      invite->room_jid.c_str(),
                      invite->password ? " " : "",
                      invite->password ? invite->password->c_str() : "");
        return 1;
    }

    encrypted = ::xmpp::stanza_axolotl_encrypted(::xmpp::StanzaView(stanza)).raw();

    // Record that this peer actively speaks OMEMO — but only for genuine
    // inbound encrypted messages (not self-outbound copies or plaintext).
    // This gate controls whether we will proactively fetch their devicelist
    // and establish a session; widening it caused spurious OMEMO initiation
    // toward contacts who send plaintext (the session was bootstrapped from
    // MAM traffic rather than from an actual inbound OMEMO message).
    if (account.omemo
        && ::xmpp::should_note_omemo_peer_traffic(
            encrypted != nullptr,
            is_self_outbound_copy,
            channel && channel->type == weechat::channel::chat_type::PM))
    {
        account.omemo.note_peer_traffic(account.context, channel->id);
    }

    x = xmpp_stanza_get_child_by_name_and_ns(stanza, "x", "jabber:x:encrypted");
    
    // XEP-0380: Explicit Message Encryption
    xmpp_stanza_t *eme = xmpp_stanza_get_child_by_name_and_ns(stanza, "encryption",
                                                                "urn:xmpp:eme:0");
    const char *eme_namespace = eme ? xmpp_stanza_get_attribute(eme, "namespace") : nullptr;
    const char *eme_name = eme ? xmpp_stanza_get_attribute(eme, "name") : nullptr;
    
    intext_storage = body ? stanza_element_text(body) : std::string {};

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
    const bool is_own_device_self_copy = encrypted && is_self_outbound_copy && account.omemo
        ? ::xmpp::is_own_device_omemo_self_copy(
            ::xmpp::StanzaView(encrypted), account.omemo.device_id)
        : false;
    if (const auto self_copy_advice = ::xmpp::evaluate_omemo_self_copy_advice(
            encrypted != nullptr,
            is_self_outbound_copy,
            is_mam_replay,
            is_own_device_self_copy);
        self_copy_advice.apply_advice)
    {
        intext_storage = OMEMO_ADVICE;
        // For MAM replays of messages sent from this device: the self-copy
        // can never be decrypted (Signal does not support self-decryption of
        // outbound messages — the inbound session at {own_jid, own_device_id}
        // is never established). Clear |encrypted| so the decode() block is
        // skipped and the message is displayed using the OMEMO_ADVICE
        // placeholder (showing the user their sent history exists without
        // corrupting the live Signal session state).
        // For live carbon copies (!is_mam_replay), leave |encrypted| set so
        // the existing silently-discard path still applies
        // (live copies are already shown as sent messages; no replay needed).
        if (self_copy_advice.clear_encrypted_on_mam)
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
                    intext_storage = xhtml_fallback;
                }
            }
        }
    }
    
    // Auto-enable OMEMO when receiving encrypted messages (PM or MUC).
    // MUC auto-enable is now safe because we have real-JID tracking + bundle readiness checks.
    if (channel
        && ::xmpp::should_auto_enable_channel_omemo(
            encrypted != nullptr, is_self_outbound_copy, channel->omemo.enabled))
    {
        weechat_printf(channel->buffer, "%s", fmt::format("{}Auto-enabling OMEMO (received encrypted message)",
                       weechat_prefix("network")).c_str());
        channel->omemo.enabled = 1;
        channel->set_transport(weechat::channel::transport::OMEMO, 0);
    }

    
    // Auto-enable PGP for PM channels when receiving PGP encrypted messages
    if (x && channel && channel->type == weechat::channel::chat_type::PM && !channel->pgp.enabled)
    {
        weechat_printf(channel->buffer, "%s", fmt::format("{}Auto-enabling PGP (received encrypted message)",
                       weechat_prefix("network")).c_str());
        channel->pgp.enabled = 1;
        channel->set_transport(weechat::channel::transport::PGP, 0);
    }
    
    // On MAM replay: check if we cached the plaintext from a prior
    // live delivery or from the send path (self-sent messages stored
    // at channel.cpp:1206/1372). This runs OUTSIDE the encrypted guard
    // so self-device own copies — where encrypted is nullified at line
    // 1934 — still reach the cache.
    if (is_mam_replay && stable_id && channel_id && account.omemo)
    {
        auto cached = account.mam_cache_lookup_omemo_plaintext(
            std::string(channel_id), std::string(stable_id));
        if (cached)
        {
            omemo_cleartext_storage = std::move(*cached);
            cleartext = omemo_cleartext_storage.data();
            was_omemo_cached = true;
            goto message_handler_after_omemo;
        }
    }

    if (encrypted && account.omemo)
    {
        bool omemo_is_duplicate = false;
        // For MUC OMEMO (plan §4.1): the stanza 'from' is room@service/nick.
        // We must pass the *sender's real bare JID* (from the member table) to
        // decode() so that Signal sessions and trust are looked up correctly.
        // If we don't have the real JID yet (semi-anonymous or race), fall back
        // to the room bare JID — decode will fail gracefully and we show a
        // placeholder (as recommended by the plan).
        //
        // Note on MAM replay safety (plan §7): the early cache check above
        // (using channel_id + stable_id) should already have supplied the
        // cleartext for historical MUC OMEMO messages, avoiding any ratchet
        // touch. This decode_jid mapping is a defensive fallback.
        std::optional<std::string_view> muc_sender_real_jid;
        std::string muc_sender_real_storage;
        if (channel && channel->type == weechat::channel::chat_type::MUC)
        {
            const std::string sender_nick = ::jid(nullptr, from).resource;
            if (!sender_nick.empty())
            {
                if (auto member_opt = channel->member_search(sender_nick.c_str()))
                {
                    if ((*member_opt)->real_jid && !(*member_opt)->real_jid->empty())
                    {
                        muc_sender_real_storage = *(*member_opt)->real_jid;
                        muc_sender_real_jid = muc_sender_real_storage;
                    }
                }
            }
        }
        const std::string decode_jid_storage =
            ::xmpp::resolve_omemo_decode_jid(from_bare, muc_sender_real_jid);
        const char *decode_jid = decode_jid_storage.c_str();

        // Always attempt decryption — MAM messages can be decrypted if the
        // Signal session is still valid.  quiet=false so errors are logged.
        // out_is_duplicate distinguishes "already seen live" (SG_ERR_DUPLICATE_MESSAGE)
        // from a genuine session failure, matching Gajim's DuplicateMessage→NodeProcessed.
        auto omemo_result = account.omemo.decode(&account, channel->buffer, decode_jid, encrypted,
                                                 /*quiet=*/false, &omemo_is_duplicate,
                                                 /*suppress_peer_traffic=*/is_mam_replay);
        if (omemo_result)
        {
            omemo_cleartext_storage = std::move(*omemo_result);
            cleartext = omemo_cleartext_storage.data();
            // Cache decrypted plaintext for MAM replay in future sessions.
            // Store on both live inbound delivery and MAM replays where decryption
            // succeeds (ratchet not yet advanced).  This ensures the plaintext is
            // available on the next WeeChat restart even when the first successful
            // decryption happened during a MAM catchup rather than live delivery.
            // Cache decrypted plaintext for ALL messages including self
            // outbound copies. Own messages sent from other devices on the
            // account need their cleartext cached so future MAM replays can
            // retrieve it even after ratchet state has advanced.
            if (channel_id)
            {
                // Store under all three IDs so the MAM replay lookup always hits.
                // On live delivery the server injects <stanza-id> (server-assigned).
                // On MAM replay the server strips <stanza-id> from the forwarded copy,
                // so stable_id on replay resolves to origin-id or the message id=
                // attribute instead.  Storing all three guarantees a cache hit
                // regardless of which ID the next MAM replay uses as its stable_id.
                const std::string ch(channel_id);
                for (const std::string &cache_id : ::xmpp::omemo_plaintext_cache_ids({
                         stanza_id ? std::string_view(stanza_id) : std::string_view{},
                         origin_id ? std::string_view(origin_id) : std::string_view{},
                         id ? std::string_view(id) : std::string_view{}}))
                {
                    account.mam_cache_store_omemo_plaintext(ch, cache_id, omemo_cleartext_storage);
                }
            }
        }
        if (!cleartext)
        {
            const auto failure = ::xmpp::disposition_for_omemo_decrypt_failure({
                is_self_outbound_copy,
                is_mam_replay,
                ::xmpp::axolotl_payload_is_empty(::xmpp::StanzaView(encrypted))});

            switch (failure)
            {
            case ::xmpp::OmemoDecryptFailureDisposition::ContinueAfterOmemo:
                goto message_handler_after_omemo;
            case ::xmpp::OmemoDecryptFailureDisposition::ShowUndecryptablePlaceholder:
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
                intext_storage = std::string(::xmpp::k_omemo_undecryptable_placeholder);
                encrypted = nullptr;   // prevent the after-omemo null-text guard from skipping display
                goto message_handler_after_omemo;
            case ::xmpp::OmemoDecryptFailureDisposition::AbortSilent:
                return 1;
            case ::xmpp::OmemoDecryptFailureDisposition::ShowDecryptionError:
                weechat_printf_date_tags(channel->buffer, 0, "notify_none", "%s%s (%s)",
                                         weechat_prefix("error"), "OMEMO Decryption Error", from);
                return 1;
            }
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
    if (::xmpp::should_skip_display_after_omemo(
            encrypted != nullptr, cleartext != nullptr, is_self_outbound_copy, is_mam_replay))
        return 1;

    if (x)
    {
        const std::string ciphertext = stanza_element_text(x);
        if (!ciphertext.empty())
        {
            if (auto decrypted = account.pgp.decrypt(channel->buffer, ciphertext.c_str()))
            {
                pgp_cleartext_storage = std::move(*decrypted);
                cleartext = pgp_cleartext_storage.data();
            }
        }
    }
    text = cleartext ? cleartext : intext_ptr();

    // XEP-0428 / XEP-0461: Fallback Indication handling.
    // When <fallback xmlns='urn:xmpp:fallback:0'> is present the <body> may
    // embed a legacy compatibility quote prefix that must be stripped.
    // Trimmed body storage — must outlive `text` usage below.
    std::string trimmed_body;
    if (text)
    {
        const auto fallback_body = ::xmpp::apply_fallback_body_trim(
            ::xmpp::StanzaView(stanza), text, has_message_correction);
        switch (fallback_body.disposition)
        {
        case ::xmpp::FallbackBodyDisposition::Cleared:
            text = nullptr;
            break;
        case ::xmpp::FallbackBodyDisposition::Trimmed:
            trimmed_body = std::move(fallback_body.trimmed);
            text = trimmed_body.empty() ? nullptr : trimmed_body.c_str();
            break;
        case ::xmpp::FallbackBodyDisposition::Unchanged:
            break;
        }
    }

    // XEP-0382: Spoiler Messages — display hint before the body
    const auto spoiler_hint = ::xmpp::parse_spoiler_hint(::xmpp::StanzaView(stanza));

    // XEP-0466: Ephemeral Messages — detect timer value
    const long ephemeral_timer =
        ::xmpp::parse_ephemeral_timer(::xmpp::StanzaView(stanza)).value_or(0);

    // XEP-0308: Message correction — replace the original line in-place.
    if (has_message_correction)
    {
        const bool is_muc_for_correction = channel
            && channel->type == weechat::channel::chat_type::MUC;
        const std::string sender_key = ::xmpp::message_correction_sender_key(
            from, from_bare, is_muc_for_correction);

        if (text
            && weechat::line_store_find_message_line_for_sender(
                channel->buffer, correction_target_storage, sender_key)
                == weechat::LineStoreLookupResult::Found)
        {
            [[maybe_unused]] const bool correction_updated =
                weechat::line_store_update_message_by_id(
                    channel->buffer,
                    correction_target_storage,
                    ::xmpp::format_message_correction_text(text));
        }
        return 1;
    }

    // XEP-0425 v0.3: Message Moderation (extends XEP-0424)
    if (auto moderation = ::xmpp::parse_moderated_retraction(::xmpp::StanzaView(stanza)))
    {
        if (!channel
            || !::xmpp::should_accept_moderation_from_sender(from_bare, channel->id))
            return 1;

        const char *moderation_channel_id = account.jid() == from_bare ? to_bare : from_bare;
        account.mam_cache_retract_message(moderation_channel_id, moderation->target_id.c_str());

        std::optional<std::string_view> moderate_reason_view;
        if (moderation->reason)
            moderate_reason_view = *moderation->reason;
        const std::string tombstone =
            ::xmpp::format_moderation_tombstone(moderate_reason_view);

        const auto moderation_lookup = weechat::line_store_tombstone_message_by_id(
            channel->buffer,
            moderation->target_id,
            tombstone,
            "xmpp_retracted,xmpp_moderated,notify_none");

        if (moderation_lookup == weechat::LineStoreLookupResult::Found)
        {
            if (moderation->reason)
            {
                weechat_printf_date_tags(channel->buffer, 0, "notify_none",
                    "%s%s moderated a message: %s",
                    weechat_prefix("network"),
                    from_bare, moderation->reason->c_str());
            }
            else
            {
                weechat_printf_date_tags(channel->buffer, 0, "notify_none",
                    "%s%s moderated a message",
                    weechat_prefix("network"),
                    from_bare);
            }
        }
        else if (moderation->reason)
        {
            weechat_printf_date_tags(channel->buffer, 0, "notify_none",
                "%s%s moderated a message (not found in buffer): %s",
                weechat_prefix("network"),
                from_bare, moderation->reason->c_str());
        }
        else
        {
            weechat_printf_date_tags(channel->buffer, 0, "notify_none",
                "%s%s moderated a message (not found in buffer)",
                weechat_prefix("network"),
                from_bare);
        }

        return 1;
    }

    // XEP-0424: Message Retraction
    if (auto retraction = ::xmpp::parse_message_retraction(::xmpp::StanzaView(stanza)))
    {
        const bool is_muc_for_retraction = channel
            && channel->type == weechat::channel::chat_type::MUC;
        const std::string retraction_sender = ::xmpp::retraction_sender_key(
            from, from_bare, is_muc_for_retraction);

        std::string retraction_occupant_id;
        bool prefer_occupant_id = false;
        const bool room_has_occupant_ids = channel && from_bare
            && account.peer_supports_feature(from_bare, "urn:xmpp:occupant-id:0");
        if (room_has_occupant_ids)
        {
            if (auto occupant_id = ::xmpp::parse_retraction_occupant_id(::xmpp::StanzaView(stanza)))
            {
                retraction_occupant_id = std::move(*occupant_id);
                prefer_occupant_id = true;
            }
        }

        const char *retraction_channel_id = account.jid() == from_bare ? to_bare : from_bare;
        account.mam_cache_retract_message(retraction_channel_id, retraction->target_id.c_str());

        const auto retraction_lookup = weechat::line_store_tombstone_retraction_by_id(
            channel->buffer,
            retraction->target_id,
            ::xmpp::format_retraction_tombstone(),
            "xmpp_retracted,notify_none",
            retraction_sender,
            retraction_occupant_id,
            prefer_occupant_id);

        if (retraction_lookup == weechat::LineStoreLookupResult::Found)
        {
            weechat_printf_date_tags(channel->buffer, 0, "notify_none",
                "%s%s retracted a message",
                weechat_prefix("network"),
                from_bare);
        }
        else if (retraction_lookup != weechat::LineStoreLookupResult::SenderRejected)
        {
            weechat_printf_date_tags(channel->buffer, 0, "notify_none",
                "%s%s retracted a message (not found in buffer)",
                weechat_prefix("network"),
                from_bare);
        }

        return 1;
    }


    // XEP-0444: Message Reactions
    if (auto reactions = ::xmpp::parse_message_reactions(::xmpp::StanzaView(stanza)))
    {
        if (channel)
        {
            [[maybe_unused]] const bool reactions_applied =
                weechat::line_store_apply_reactions_by_id(
                    channel->buffer, reactions->target_id, reactions->emojis);
        }
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
    timestamp = !override_delay_stamp.empty() ? override_delay_stamp.data()
                : delay ? xmpp_stanza_get_attribute(delay, "stamp") : nullptr;
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
                                  channel->buffer, "lines");
        bool already_shown = false;
        if (own_lines)
        {
            struct t_gui_line *ln = (struct t_gui_line*)
                weechat_hdata_pointer(hdata_lines, own_lines, "last_line");
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
    const bool from_self = from_bare
        && weechat_strcasecmp(from_bare, account.jid().data()) == 0;
    if (::xmpp::should_clear_typing_on_message(delay != nullptr, from_self))
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

    if (is_mam_replay)
        weechat_string_dyn_concat(dyn_tags, ",no_trigger", -1);

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
    if (has_message_correction)
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
        if (auto bm_it = account.bookmarks.find(channel->id); bm_it != account.bookmarks.end())
        {
            auto& [_, bm] = *bm_it;
            channel_notify = bm.notify_setting;
        }

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

    const char *edit = has_message_correction ? "📝 " : "";
    if (x && text == cleartext && channel->transport != weechat::channel::transport::PGP)
        channel->transport = weechat::channel::transport::PGP;
    else if (!x && !encrypted && text && !cleartext
             && text == intext_storage.c_str()
             && channel->transport != weechat::channel::transport::PLAIN)
        channel->transport = weechat::channel::transport::PLAIN;
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
    
    // XEP-0461: Message Replies — extract reply context for quote line.
    std::string reply_prefix;
    std::string reply_quote_nick;
    if (auto reply = ::xmpp::parse_message_reply(::xmpp::StanzaView(stanza)))
    {
        if (channel)
        {
            if (auto quote = weechat::line_store_lookup_reply_quote(
                    channel->buffer, reply->target_id))
            {
                reply_prefix = std::move(quote->excerpt);
                reply_quote_nick = std::move(quote->quote_nick);
            }
        }
        if (reply_prefix.empty())
            reply_prefix = std::string(::xmpp::default_reply_excerpt());
    }
    
    // Inline image display candidates (weechat-icat / Kitty graphics protocol)
    std::string incoming_image_url;
    std::string incoming_image_mime;
    size_t incoming_image_width = 0, incoming_image_height = 0;

    // XEP-0066: Out of Band Data - extract URL from <x xmlns='jabber:x:oob'>
    xmpp_stanza_t *oob_x = xmpp_stanza_get_child_by_name_and_ns(stanza, "x", "jabber:x:oob");
    std::string oob_suffix;
    if (oob_x)
    {
        xmpp_stanza_t *url_elem = xmpp_stanza_get_child_by_name(oob_x, "url");
        if (url_elem)
        {
            const std::string url_text = stanza_element_text(url_elem);
            if (!url_text.empty())
            {
                // Candidate for weechat-icat (no MIME in OOB — check extension)
                std::string_view url_sv(url_text);
                if (url_sv.ends_with(".jpg") || url_sv.ends_with(".jpeg")
                    || url_sv.ends_with(".png") || url_sv.ends_with(".gif")
                    || url_sv.ends_with(".webp"))
                {
                    incoming_image_url = url_text;
                }

                const std::string desc_text = stanza_element_text(
                    xmpp_stanza_get_child_by_name(oob_x, "desc"));

                // Format: [URL: url] or [URL: description (url)]
                if (!desc_text.empty())
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

    // XEP-0385 / XEP-0447 / XEP-0448: SIMS and SFS display suffixes
    std::string sims_suffix;
    const auto media_view = ::xmpp::StanzaView(stanza);
    for (const auto &share : ::xmpp::collect_sims_shares(media_view))
    {
        if (is_image_mime_type(share.meta.mime))
        {
            incoming_image_url = share.url;
            incoming_image_mime = share.meta.mime;
            incoming_image_width = share.meta.width;
            incoming_image_height = share.meta.height;
        }

        sims_suffix += ::xmpp::format_file_share_suffix(
            share.meta.name, share.meta.mime, share.meta.size_raw, share.url);

        if (!oob_suffix.empty() && oob_suffix.contains(share.url))
            oob_suffix.clear();
    }

    for (const auto &sfs : ::xmpp::collect_sfs_shares(media_view))
    {
        if (sfs.encrypted)
        {
            const auto &enc = *sfs.encrypted;
            const std::string esfs_stable_id = stable_id ? std::string(stable_id) : std::string();
            const std::string esfs_channel_jid = channel_id ? std::string(channel_id) : std::string();

            bool already_downloaded = false;
            if (!esfs_stable_id.empty() && !esfs_channel_jid.empty())
            {
                if (auto prev = account.mam_cache_lookup_esfs_download(
                        esfs_channel_jid, esfs_stable_id))
                {
                    sims_suffix += ::xmpp::format_encrypted_file_saved_suffix(
                        enc.meta.name, *prev);
                    already_downloaded = true;

                    if (weechat::config::instance &&
                        weechat_config_boolean(weechat::config::instance->look.icat) &&
                        is_image_mime_type(enc.meta.mime))
                    {
                        auto [w, h] = read_image_dimensions(prev->c_str());
                        const std::string dim_args = icat_dimension_args(w, h);
                        const std::string icat_cmd = fmt::format(
                            "/icat -print_immediately{} {}", dim_args, *prev);
                        weechat_command(channel->buffer, icat_cmd.c_str());
                    }
                }
            }

            if (!already_downloaded)
            {
                esfs_start_download(enc.ciphertext_url, enc.meta.name, enc.key_b64, enc.iv_b64,
                                    channel ? channel->buffer : account.buffer,
                                    &account, esfs_channel_jid, esfs_stable_id);
                sims_suffix += ::xmpp::format_encrypted_file_suffix(
                    enc.meta.name, enc.meta.size_raw);
            }
            continue;
        }

        if (!sfs.plain_url)
            continue;

        const std::string &sfs_url = *sfs.plain_url;
        if (!sims_suffix.empty() && sims_suffix.contains(sfs_url))
            continue;
        if (!oob_suffix.empty() && oob_suffix.contains(sfs_url))
            oob_suffix.clear();

        if (is_image_mime_type(sfs.meta.mime))
        {
            incoming_image_url = sfs_url;
            incoming_image_mime = sfs.meta.mime;
            incoming_image_width = sfs.meta.width;
            incoming_image_height = sfs.meta.height;
        }

        sims_suffix += ::xmpp::format_file_share_suffix(
            sfs.meta.name, sfs.meta.mime, sfs.meta.size_raw, sfs_url);
    }

    // XEP-0511: Link Metadata — parse <rdf:Description> containing OpenGraph metadata.
    // Previews are collected here and printed as separate buffer lines after the main message.
    // Live messages populate the LMDB og_previews cache; MAM replays look up the cache.
    std::vector<account::og_preview> og_previews_to_show;

    xmpp_stanza_t *rdf_desc = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "Description", "http://www.w3.org/1999/02/22-rdf-syntax-ns#");
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

            account::og_preview p;
            for (xmpp_stanza_t *prop = xmpp_stanza_get_children(desc);
                 prop; prop = xmpp_stanza_get_next(prop))
            {
                const char *prop_name = xmpp_stanza_get_name(prop);
                const char *prop_ns   = xmpp_stanza_get_ns(prop);
                if (!prop_name || !prop_ns) continue;
                if (std::string_view(prop_ns) != "https://ogp.me/ns#") continue;

                const std::string val = stanza_element_text(prop);
                if (val.empty()) continue;

                if (std::string_view(prop_name) == "title" && p.title.empty())
                    p.title = val;
                else if (std::string_view(prop_name) == "description" && p.description.empty())
                    p.description = val;
                else if (std::string_view(prop_name) == "url" && p.url.empty())
                    p.url = val;
                else if (std::string_view(prop_name) == "image" && p.image.empty())
                {
                    // Only store HTTP(S) image URLs; skip cid:, ni:, data: URIs
                    if (std::string_view(val).starts_with("http"))
                        p.image = val;
                }
            }

            if (!p.title.empty() || !p.description.empty() || !p.url.empty())
            {
                // Persist to LMDB cache so MAM replay can redisplay without network fetch
                if (!p.url.empty())
                    account.og_cache_store(p.url, p);
                og_previews_to_show.push_back(std::move(p));
            }
        }
    }
    else if (is_mam_replay && text)
    {
        // MAM replay: no <rdf:Description> in stanza — look up any URLs in the body from cache.
        // Simple scanner: find "http://" or "https://" and scan to first whitespace or end.
        std::string_view body_sv(text);
        size_t pos = 0;
        while (pos < body_sv.size())
        {
            size_t found = body_sv.find("http", pos);
            if (found == std::string_view::npos) break;
            // Confirm it's http:// or https://
            std::string_view rest = body_sv.substr(found);
            if (!rest.starts_with("http://") && !rest.starts_with("https://"))
            {
                pos = found + 4;
                continue;
            }
            // Scan URL to first whitespace or end
            size_t end = found;
            while (end < body_sv.size() && !std::isspace((unsigned char)body_sv[end]))
                ++end;
            std::string url(body_sv.substr(found, end - found));
            strip_url_trailing_punct(url);
            if (url.empty()) { pos = end; continue; }
            if (auto cached = account.og_cache_lookup(url))
                og_previews_to_show.push_back(std::move(*cached));
            pos = end;
        }
    }

    // XEP-0393 / XEP-0394: message body styling
    std::string styled_text;
    const char *display_text = text;
    if (text)
    {
        styled_text = ::xmpp::format_inbound_message_body(stanza, text);
        display_text = styled_text.c_str();
    }

    // Replace text emoticons with Unicode emoji (configurable).
    std::string emoticon_text;
    if (display_text
        && weechat::config::instance
        && weechat::config::instance->look.emoticons.boolean())
    {
        emoticon_text = replace_emoticons(display_text);
        display_text = emoticon_text.c_str();
    }

    std::string final_text; // used by spoiler / ephemeral / oob suffix blocks below

    // XEP-0461: emit reply context as a separate quote line above the message.
    // Nick column shows the replying user (same as the message line below it).
    // Body shows  │ quotedNick: excerpt  in dim/cyan so it reads as a quote block.
    // notify_none,no_log: no highlight, no duplicate log entry.
    if (!reply_prefix.empty())
    {
        const std::string quote_line =
            ::xmpp::format_reply_quote_body(reply_quote_nick, reply_prefix);
        std::string msg = fmt::format("{}\t{}", display_prefix, quote_line);
        weechat_printf_date_tags(channel->buffer, date,
            "notify_none,no_log,xmpp_reply_quote", "%s", msg.c_str());
    }

    // XEP-0382: Spoiler Messages — prepend spoiler warning before the body
    if (spoiler_hint)
    {
        std::optional<std::string_view> hint_view;
        if (!spoiler_hint->empty())
            hint_view = *spoiler_hint;
        const std::string spoiler_prefix =
            ::xmpp::format_spoiler_display_prefix(hint_view);
        if (final_text.empty())
            final_text = display_text ? display_text : "";
        final_text = spoiler_prefix + final_text;
        display_text = final_text.c_str();
    }

    // XEP-0466: Ephemeral Messages — prepend timer indicator before the body
    if (ephemeral_timer > 0)
    {
        const std::string eph_prefix =
            ::xmpp::format_ephemeral_display_prefix(ephemeral_timer);
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

    const char *encrypted_glyph = (encrypted || x || was_omemo_cached) ? "🔒 " : "";

    if (channel_id == from_bare && to == channel->id)
    {
        std::string msg = fmt::format("{}\t{}[to {}]: {}{}",
                                      display_prefix,
                                      edit, to, encrypted_glyph,
                                      display_text ? display_text : "");
        weechat_printf_date_tags(channel->buffer, date, *dyn_tags, "%s", msg.c_str());
    }
    else if (weechat_string_match(text, "/me *", 0))
    {
        std::string msg = fmt::format("{}\t{}{}{} {}",
                                      weechat_prefix("action"),
                                      edit, display_prefix,
                                      encrypted_glyph,
                                      display_text ? display_text+4 : "");
        weechat_printf_date_tags(channel->buffer, date, *dyn_tags, "%s", msg.c_str());
    }
    else
    {
        std::string msg = fmt::format("{}\t{}{}{}",
                                      display_prefix,
                                      edit, encrypted_glyph,
                                      display_text ? display_text : "");
        weechat_printf_date_tags(channel->buffer, date, *dyn_tags, "%s", msg.c_str());
    }

    // weechat-icat: display inline image for incoming SFS/SIMS/OOB image URLs.
    // -print_immediately prints placeholder lines synchronously so the preview
    // appears directly under the message, before any later messages arrive.
    if (weechat::config::instance &&
        weechat_config_boolean(weechat::config::instance->look.icat) &&
        !incoming_image_url.empty())
    {
        std::string dim_args = icat_dimension_args(incoming_image_width, incoming_image_height);
        std::string icat_cmd = fmt::format("/icat -print_immediately{} {}",
                                           dim_args, incoming_image_url);
        weechat_command(channel->buffer, icat_cmd.c_str());
    }

    // XEP-0511: print each collected OG preview as a separate buffer line.
    // Using notify_none,no_log,xmpp_og_preview so these lines are never logged
    // or highlighted, and can be identified/skipped by the reply-excerpt scanner.
    if (!og_previews_to_show.empty()
        && weechat::config::instance
        && weechat::config::instance->look.incoming_link_preview.boolean())
    {
        for (const auto& p : og_previews_to_show)
        {
            std::string line = format_og_preview_card(
                p.title, p.description, p.url, p.image, "");
             std::string msg = fmt::format("{}\t{}", display_prefix, line);
             weechat_printf_date_tags(channel->buffer, date,
                 "notify_none,no_log,xmpp_og_preview", "%s", msg.c_str());
        }
    }

    // For any http(s):// URL in the message body that wasn't already shown
    // (from the rdf:Description stanza or the LMDB cache), kick off an async
    // fetch to get OG/title metadata.
    //
    // Only do this for LIVE messages (not MAM replay).  WeeChat always appends
    // new lines at the end of the buffer regardless of the date parameter, so
    // async fetch results for MAM-replayed messages would appear below "History
    // loaded" rather than inline with the original message.  For MAM, the LMDB
    // cache lookup (in the is_mam_replay branch above) already handles URLs
    // that were fetched during a previous live session.  First-time-seen URLs
    // in MAM will be fetched when the same URL appears in a live message later.
    if (text && !is_mam_replay
        && weechat::config::instance
        && weechat::config::instance->look.incoming_link_preview.boolean())
    {
        // Collect URLs that were already shown so we don't double-print.
        std::unordered_set<std::string> already_shown;
        for (const auto& p : og_previews_to_show)
            if (!p.url.empty()) already_shown.insert(p.url);

        std::string_view body_sv(text);
        size_t pos = 0;
        while (pos < body_sv.size())
        {
            size_t found = body_sv.find("http", pos);
            if (found == std::string_view::npos) break;
            std::string_view rest = body_sv.substr(found);
            if (!rest.starts_with("http://") && !rest.starts_with("https://"))
            {
                pos = found + 4;
                continue;
            }
            size_t end = found;
            while (end < body_sv.size() && !std::isspace((unsigned char)body_sv[end]))
                ++end;
            std::string url(body_sv.substr(found, end - found));
            strip_url_trailing_punct(url);
            if (url.empty()) { pos = end; continue; }
            pos = end;

            if (already_shown.contains(url)) continue;
            // For self-sent messages, use silent mode — the XEP-0511 metadata
            // from the <rdf:Description> carbon copy already displayed the
            // preview, and we sent the OG metadata ourselves.
            const bool is_self = from_bare
                && weechat_strcasecmp(from_bare, account.jid().data()) == 0;
            og_start_fetch(url, channel->buffer, &account,
                           std::string(display_prefix), date, is_self);
        }
    }

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
    if (::xmpp::should_schedule_ephemeral_tombstone(
            ephemeral_timer, stable_id ? std::string_view(stable_id) : std::string_view{}))
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
         "urn:xmpp:rai:0",
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
    std::ranges::for_each(advertised_features, [&](const auto &f) { sorted_features.emplace_back(f); });
    std::ranges::sort(sorted_features);

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

    // Add a single-value x-data field and append it to the XEP-0115 serial.
    // XEP-0115 §5.1: the S string for a form is:
    //   form_type_value< (FORM_TYPE value only, NOT "FORM_TYPE<value")
    //   then for each non-FORM_TYPE field sorted by var: var<val<
    auto add_feature1 = [&](const char *var, const char *type, const char *val) {
        xmpp_stanza_t *f = stanza_make_field(account.context, var, val, type);
        xmpp_stanza_add_child(x, f);
        xmpp_stanza_release(f);
        if (std::string_view(var) != "FORM_TYPE") {
            // non-FORM_TYPE fields: var<val<
            weechat_string_dyn_concat(serial, var, -1);
            weechat_string_dyn_concat(serial, "<", -1);
        }
        // FORM_TYPE field: just the value (the form type URI); non-FORM_TYPE: the value
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

    const std::string title = stanza_element_text(
        xmpp_stanza_get_child_by_name(x_form, "title"));
    const std::string instr = stanza_element_text(
        xmpp_stanza_get_child_by_name(x_form, "instructions"));

    weechat_printf_date_tags(buf, 0, "xmpp_adhoc,notify_none",
                             "%s%s── Ad-Hoc Form%s%s%s ──%s",
                             weechat_prefix("network"),
                             weechat_color("bold"),
                             title.empty() ? "" : ": ",
                             title.empty() ? "" : title.c_str(),
                             title.empty() ? "" : "",
                             weechat_color("-bold"));
    if (!instr.empty())
        weechat_printf_date_tags(buf, 0, "xmpp_adhoc,notify_none",
                                 "%s  %s%s%s",
                                 weechat_prefix("network"),
                                 weechat_color("italic"),
                                 instr.c_str(),
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
            values.push_back(stanza_element_text(v));
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
            bool first = true;
            std::ranges::for_each(values, [&](const auto &v) {
                if (!first)
                    val_display += std::string(weechat_color("darkgray"))
                                   + " | " + weechat_color("resetcolor");
                first = false;
                val_display += v;
            });
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

                const std::string oval = stanza_element_text(
                    xmpp_stanza_get_child_by_name(opt, "value"));
                const char *olabel = xmpp_stanza_get_attribute(opt, "label");

                if (!options_str.empty()) options_str += "  ";

                // Is this option currently selected?
                bool selected = false;
                for (auto &v : values)
                    if (!oval.empty() && v == oval) { selected = true; break; }

                if (selected)
                    options_str += weechat_color("green");
                else
                    options_str += weechat_color("darkgray");

                options_str += oval.empty() ? "?" : oval;
                if (olabel && !oval.empty() && std::string_view(olabel) != oval)
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
