bool weechat::connection::message_handler(xmpp_stanza_t *stanza, bool top_level, bool is_mam_replay,
                                          std::string_view override_archive_id,
                                          std::string_view override_delay_stamp,
                                          bool is_carbon_copy)
{
    // SM counter incremented in libstrophe wrapper, not here
    // top_level parameter kept for nested/recursive calls

    (void) top_level;
    append_raw_xml_trace(account, "RECV", stanza);

    // XEP-0280: unwrap carbons before any payloadless early-return paths.
    {
        const auto view = ::xmpp::StanzaView(stanza);
        if (::xmpp::stanza_is_carbon(view))
        {
            if (auto inner = ::xmpp::parse_carbon_inner_message(view, account.jid()))
                return message_handler(*inner, false, false, {}, {}, true);
            return 1;
        }
    }

    const auto inbound = ::xmpp::StanzaView(stanza);

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
    xmpp_stanza_t *x, *body, *delay, *topic, *result, *encrypted;
    const char *type, *from, *nick, *from_bare, *to, *to_bare, *id, *thread, *replace_id, *timestamp;
    const char *text = nullptr;
    std::string intext_storage;
    auto intext_ptr = [&]() -> const char * {
        return intext_storage.empty() ? nullptr : intext_storage.c_str();
    };
    std::string from_bare_main_storage; // owns main from_bare string
    std::string to_bare_main_storage;   // owns main to_bare string
    std::string inbound_type_storage;
    std::string inbound_from_storage;
    std::string inbound_to_storage;
    std::string inbound_id_storage;
    std::string inbound_thread_storage;
    std::string stanza_id_attr_storage;
    std::string stanza_id_by_attr_storage;
    std::string origin_id_attr_storage;
    std::string timestamp_attr_storage;
    std::string delay_from_attr_storage;
    std::string eme_namespace_storage;
    std::string eme_name_storage;
    // Cache own JID for use when stanza 'from' is absent (e.g. self-messages).
    // account.jid() returns a temporary; taking .data() on it is a dangling pointer.
    const std::string own_jid_str = account.jid();

    const auto find_channel_for_partner = [&](std::string_view partner_bare)
        -> weechat::channel *
    {
        const std::string key = account.resolve_channel_key(partner_bare);
        if (key.empty())
            return nullptr;
        if (auto it = account.channels.find(key); it != account.channels.end())
            return &it->second;
        return nullptr;
    };

    const auto partner_channel_from_message = [&](const ::xmpp::StanzaView &msg)
        -> weechat::channel *
    {
        const auto partner = ::xmpp::conversation_channel_jid_from_message(
            msg.from().value_or(""),
            msg.to().value_or(""),
            own_jid_str);
        if (!partner)
            return nullptr;
        return find_channel_for_partner(*partner);
    };

     std::string omemo_cleartext_storage; // owns OMEMO-decrypted text
     std::string pgp_cleartext_storage; // owns PGP-decrypted text (avoids strdup)
     char *cleartext = nullptr;
    struct tm time = {0};
    time_t date = 0;

    auto binding = std::make_unique<xml::message>(account.context, stanza);
    body = inbound.child("body").raw();
    xmpp_stanza_t *encrypted_without_body =
        ::xmpp::stanza_axolotl_encrypted(inbound).raw();
    xmpp_stanza_t *pgp_without_body = inbound.child("x", "jabber:x:encrypted").raw();

    // Payloadless OMEMO stanzas are control/key-transport messages and must
    // not create PM buffers for inactive roster contacts.
    if (body == nullptr && encrypted_without_body
        && !::xmpp::stanza_has_user_message_payload(::xmpp::StanzaView(stanza)))
        return 1;

    if (body == nullptr && !encrypted_without_body && !pgp_without_body)
    {
        topic = inbound.child("subject").raw();
        if (topic != nullptr)
        {
            intext_storage = stanza_element_text(topic);
            inbound_type_storage = inbound.attr_string("type");
            type = inbound_type_storage.empty() ? nullptr : inbound_type_storage.c_str();
        if (::xmpp::stanza_is_error_message(inbound))
            return 1;
        inbound_from_storage = inbound.attr_string("from");
        from = inbound_from_storage.empty() ? nullptr : inbound_from_storage.c_str();
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
                channel = partner_channel_from_message(inbound);
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

        // XEP-0452: MUC Mention Notifications (v0.2.x wire format)
        // The server sends a <message from='room@muc.example.com'> containing:
        //   <mentions xmlns='urn:xmpp:mmn:0'>
        //     <forwarded xmlns='urn:xmpp:forward:0'>...the original groupchat message...</forwarded>
        //   </mentions>
        // Security (§4 MUST): outer from= must match the from= of the forwarded message.
        {
            const auto mmn_view = inbound.child("mentions", "urn:xmpp:mmn:0");
            if (mmn_view.valid())
            {
                // The outer message from= is the MUC JID that sent us the notification.
                const std::string outer_from_s = inbound.attr_string("from");
                const char *outer_from = outer_from_s.empty() ? nullptr : outer_from_s.c_str();
                std::string muc_jid = outer_from ? ::jid(nullptr, outer_from).bare : std::string{};

                if (!muc_jid.empty())
                {
                    // <forwarded> is a direct child of <mentions>, not of <message>
                    const auto mmn_fwd = mmn_view.child("forwarded", "urn:xmpp:forward:0");
                    if (mmn_fwd.valid())
                    {
                        const auto mmn_msg = mmn_fwd.child("message");
                        const auto mmn_delay = mmn_fwd.child("delay", "urn:xmpp:delay");
                        if (mmn_msg.valid())
                        {
                            // §4 MUST: forwarded message from= bare JID must match muc_jid
                            const std::string inner_from_raw_s = mmn_msg.attr_string("from");
                            const char *inner_from_raw = inner_from_raw_s.empty()
                                ? nullptr : inner_from_raw_s.c_str();
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

                            const std::string mmn_text_str = mmn_msg.child("body").text();
                            const char *mmn_text = mmn_text_str.empty()
                                ? nullptr : mmn_text_str.c_str();

                            const std::string mmn_from_s = mmn_msg.attr_string("from");
                            const char *mmn_from = mmn_from_s.empty() ? nullptr : mmn_from_s.c_str();
                            std::string mmn_nick_s = mmn_from ? ::jid(nullptr, mmn_from).resource : std::string{};
                            const char *mmn_nick = !mmn_nick_s.empty()
                                ? mmn_nick_s.c_str() : "(unknown)";

                            // Parse timestamp from <delay> if present
                            time_t mmn_ts = 0;
                            if (mmn_delay.valid())
                            {
                                const std::string stamp = mmn_delay.attr_string("stamp");
                                if (!stamp.empty())
                                {
                                    struct tm mmn_tm = {};
                                    strptime(stamp.c_str(), "%FT%T", &mmn_tm);
                                    mmn_ts = timegm(&mmn_tm);
                                }
                            }

                            if (mmn_text && *mmn_text)
                            {
                                weechat::UiPort::for_buffer(muc_buf)->printf_date_tags(
                                    mmn_ts, "xmpp_message,notify_highlight,log1",
                                    fmt::format("{}{}\t{}",
                                        weechat::RuntimePort::default_runtime().prefix("action"),
                                        mmn_nick, mmn_text));
                            }
                        }
                    }
                    return 1;
                }
            }
        }

        result = inbound.child("result", "urn:xmpp:mam:2").raw();
        if (result)
        {
            const auto mam_view = inbound;

            // XEP-0442: pubsub MAM result — forwarded stanza contains a pubsub
            // <event> with the original item payload. Process it here so we can
            // build the correct feed_key from the query context (service JID from
            // our IQ) rather than from the inner message's from= attribute.
            const auto pubsub_queryid = ::xmpp::mam_pubsub_query_id(mam_view);
            if (pubsub_queryid && account.pubsub_mam_queries.contains(*pubsub_queryid))
            {
                if (!weechat::xmpp_feeds_enabled())
                    return 1;

                const auto &pq = account.pubsub_mam_queries.at(*pubsub_queryid);
                std::string feed_service = pq.service;
                std::string node_name    = pq.node;
                std::string feed_key     = fmt::format("{}/{}", feed_service, node_name);

                if (!account.feed_is_open(feed_key))
                    return 1;

                auto [ch_it, inserted] = account.channels.try_emplace(
                    feed_key,
                    account, weechat::channel::chat_type::FEED,
                    feed_key, feed_key);
                auto& [_, feed_ch] = *ch_it;
                if (inserted)
                    account.feed_open_register(feed_key);

                const auto result_view = ::xmpp::StanzaView(result);
                const auto forwarded_view = result_view.child("forwarded", "urn:xmpp:forward:0");
                if (forwarded_view.valid())
                {
                    const auto fwd_msg = forwarded_view.child("message");
                    if (fwd_msg.valid())
                    {
                        // Find the <event> or direct <item> child
                        const auto fwd_event = fwd_msg.child(
                            "event", "http://jabber.org/protocol/pubsub#event");
                        const auto fwd_items = fwd_event.valid()
                            ? fwd_event.child("items") : ::xmpp::StanzaView(nullptr);

                        if (fwd_items.valid())
                        {
                            // Extract the publisher (from= of the inner message or publisher= on item)
                            const std::string publisher_jid_s = fwd_msg.attr_string("from");
                            const char *publisher_jid = publisher_jid_s.empty()
                                ? nullptr : publisher_jid_s.c_str();

                            for (const auto &fwd_item : fwd_items)
                            {
                                if (fwd_item.name() != "item")
                                    continue;

                                const std::string item_id_raw_s = fwd_item.attr_string("id");
                                const char *item_id_raw = item_id_raw_s.empty()
                                    ? nullptr : item_id_raw_s.c_str();
                                if (item_id_raw && account.feed_item_seen(feed_key, item_id_raw))
                                    continue;

                                auto entry_view = fwd_item.child(
                                    "entry", "http://www.w3.org/2005/Atom");
                                if (!entry_view.valid())
                                    entry_view = fwd_item.child("entry");
                                if (!entry_view.valid()) continue;

                                const std::string pub_s = fwd_item.attr_string("publisher");
                                const std::string_view pub_sv =
                                    pub_s.empty() ? std::string_view(publisher_jid) : std::string_view(pub_s);

                                atom_entry ae = parse_atom_entry(entry_view, pub_sv);
                                if (item_id_raw && !ae.item_id.empty())
                                    account.feed_atom_id_set(feed_key, item_id_raw, ae.item_id);
                                if (item_id_raw && !ae.replies_link.empty())
                                    account.feed_replies_link_set(feed_key, item_id_raw, ae.replies_link);

                                int item_alias = -1;
                                if (item_id_raw && *item_id_raw)
                                    item_alias = account.feed_alias_assign(feed_key, item_id_raw);

                                if (ae.empty()) continue;

                                ::xmpp::render_atom_entry_to_feed(
                                    feed_ch,
                                    account,
                                    feed_key,
                                    feed_service,
                                    node_name,
                                    item_id_raw ? std::string_view(item_id_raw) : std::string_view{},
                                    item_alias,
                                    ae);

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

                const std::string debug_from_s = msg_view.attr_string("from");
                const std::string debug_to_s = msg_view.attr_string("to");
                const std::string debug_type_s = msg_view.attr_string("type");
                const char *debug_from = debug_from_s.empty() ? nullptr : debug_from_s.c_str();
                const char *debug_to = debug_to_s.empty() ? nullptr : debug_to_s.c_str();
                const char *debug_type = debug_type_s.empty() ? nullptr : debug_type_s.c_str();

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

                const std::string msg_id_s = msg_view.attr_string("id");
                const char *msg_id = msg_id_s.empty() ? nullptr : msg_id_s.c_str();
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
                                                  msg_timestamp, effective_body,
                                                  /*batched=*/true);
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

        handle_pubsub_pep_event(stanza, own_jid_str);


        // XEP-0184 / XEP-0333: inbound delivery receipt or displayed marker
        {
            const auto view = ::xmpp::StanzaView(stanza);
            if (::xmpp::stanza_is_receipt_ack(view))
            {
                if (auto ack = ::xmpp::parse_incoming_receipt(view))
                {
                    if (weechat::channel *ch = partner_channel_from_message(inbound))
                    {
                        const bool muc_channel =
                            ch->type == weechat::channel::chat_type::MUC;
                        const auto event = weechat::build_incoming_receipt_render_event(
                            ack->acked_id, muc_channel);
                        (void)weechat::apply_render_event(ch->buffer, event);
                    }
                }
                return 1;
            }
            if (::xmpp::stanza_is_displayed_ack(view))
            {
                if (auto ack = ::xmpp::parse_incoming_displayed(view))
                {
                    if (weechat::channel *ch = partner_channel_from_message(inbound))
                    {
                        const bool muc_channel =
                            ch->type == weechat::channel::chat_type::MUC;
                        const auto event = weechat::build_incoming_displayed_render_event(
                            ack->acked_id, muc_channel);
                        (void)weechat::apply_render_event(ch->buffer, event);
                    }
                }
                return 1;
            }
        }

        // XEP-0224: Attention — handle incoming <attention> from others
        {
            const auto attention_view = inbound.child("attention", "urn:xmpp:attention:0");
            if (attention_view.valid())
            {
                const std::string attn_from_s = inbound.attr_string("from");
                const char *attn_from = attn_from_s.empty() ? nullptr : attn_from_s.c_str();
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
                    weechat::UiPort::for_buffer(ch->buffer)->printf_date_tags_network(
                        0, "xmpp_attention,notify_highlight",
                        fmt::format("{} is requesting your attention! (/buzz)", bare_s));
                }
                return 1;
            }
        }

        // XEP-0437: Room Activity Indicators — handle <rai> notifications
        {
            const auto rai_view = inbound.child("rai", "urn:xmpp:rai:0");
            if (rai_view.valid())
            {
                for (const auto &act : rai_view)
                {
                    if (act.name() != "activity")
                        continue;
                    const std::string jid_str = act.text();
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
                        weechat::UiPort::for_buffer(rai_ch->buffer)->printf_date_tags(
                            0, "xmpp_rai,notify_message",
                            _("Room activity detected"));
                    }
                    else
                    {
                        weechat::UiPort::for_buffer(account.buffer)->printf_network(fmt::format(
                            "{}{}{}{}",
                            _("Room activity: "),
                            weechat::RuntimePort::default_runtime().color("chat_nick_self"),
                            jid,
                            weechat::RuntimePort::default_runtime().color("reset")));
                    }
                }
                return 1;
            }
        }

        // XEP-0444 / XEP-0424: payloadless messages (e.g. carbon-copied reactions)
        {
            const auto payloadless_view = ::xmpp::StanzaView(stanza);
            if (auto reactions = ::xmpp::parse_message_reactions(payloadless_view))
            {
                if (weechat::channel *rx_ch = partner_channel_from_message(inbound))
                {
                    if (rx_ch->buffer)
                    {
                        (void)weechat::apply_render_event(
                            rx_ch->buffer,
                            weechat::build_reactions_render_event(
                                reactions->target_id, reactions->emojis));
                    }
                }
                return 1;
            }
            if (auto retraction = ::xmpp::parse_message_retraction(payloadless_view))
            {
                const std::string rx_from_s = inbound.attr_string("from");
                const std::string rx_to_s = inbound.attr_string("to");
                const char *rx_from = rx_from_s.empty() ? nullptr : rx_from_s.c_str();
                const char *rx_to = rx_to_s.empty() ? nullptr : rx_to_s.c_str();
                if (!rx_from && rx_to)
                {
                    const std::string rx_to_bare = ::jid(nullptr, rx_to).bare;
                    if (!rx_to_bare.empty()
                        && !::xmpp::bare_jid_iequals(rx_to_bare, own_jid_str))
                        rx_from = own_jid_str.c_str();
                }
                if (weechat::channel *rx_ch = partner_channel_from_message(inbound))
                {
                    const auto partner_jid = ::xmpp::conversation_channel_jid_from_message(
                        rx_from_s, rx_to_s, own_jid_str);
                    if (partner_jid)
                    {
                        account.mam_cache_retract_message(
                            partner_jid->c_str(), retraction->target_id.c_str());
                    }

                    const bool is_muc_for_retraction =
                        rx_ch->type == weechat::channel::chat_type::MUC;
                    const std::string rx_from_bare = rx_from
                        ? ::jid(nullptr, rx_from).bare : std::string{};
                    const std::string retraction_sender = ::xmpp::retraction_sender_key(
                        rx_from, rx_from_bare.c_str(), is_muc_for_retraction);

                    const auto retraction_lookup = weechat::apply_render_event(
                        rx_ch->buffer,
                        weechat::build_retraction_tombstone_render_event(
                            retraction->target_id,
                            ::xmpp::format_retraction_tombstone(),
                            "xmpp_retracted,notify_none",
                            retraction_sender,
                            {},
                            false));

                    auto rx_ui = weechat::UiPort::for_buffer(rx_ch->buffer);
                    const std::string rx_from = rx_from_bare.empty()
                        ? own_jid_str : rx_from_bare;
                    if (retraction_lookup
                        && *retraction_lookup == weechat::LineStoreLookupResult::Found)
                    {
                        rx_ui->printf_network(fmt::format("{} retracted a message", rx_from));
                    }
                    else if (!retraction_lookup
                             || *retraction_lookup
                                    != weechat::LineStoreLookupResult::SenderRejected)
                    {
                        rx_ui->printf_network(
                            fmt::format("{} retracted a message (not found in buffer)", rx_from));
                    }
                }
                return 1;
            }
        }

        return 1;
    }
    inbound_type_storage = inbound.attr_string("type");
    type = inbound_type_storage.empty() ? nullptr : inbound_type_storage.c_str();
    if (::xmpp::stanza_is_error_message(inbound))
        return 1;
    inbound_from_storage = inbound.attr_string("from");
    from = inbound_from_storage.empty() ? nullptr : inbound_from_storage.c_str();
    inbound_to_storage = inbound.attr_string("to");
    to = inbound_to_storage.empty() ? nullptr : inbound_to_storage.c_str();
    if (from == nullptr)
    {
        // XEP-0280 sent carbons: inner copy may omit from=; peer is in to=.
        if (is_carbon_copy && to)
        {
            const std::string inferred_to_bare = ::jid(nullptr, to).bare;
            if (!inferred_to_bare.empty()
                && !::xmpp::bare_jid_iequals(inferred_to_bare, own_jid_str))
                from = own_jid_str.c_str();
        }
        if (from == nullptr)
            return 1;
    }
    from_bare_main_storage = ::jid(nullptr, from).bare;
    from_bare = from_bare_main_storage.c_str();
    if (to == nullptr)
        to = own_jid_str.c_str();
    to_bare_main_storage = to ? ::jid(nullptr, to).bare : std::string{};
    to_bare = !to_bare_main_storage.empty() ? to_bare_main_storage.c_str() : nullptr;
    const bool from_is_account = from_bare
        && ::xmpp::bare_jid_iequals(from_bare, own_jid_str);
    const bool is_self_outbound_copy = from_is_account && to_bare
        && !::xmpp::bare_jid_iequals(to_bare, own_jid_str);
    inbound_id_storage = inbound.attr_string("id");
    id = inbound_id_storage.empty() ? nullptr : inbound_id_storage.c_str();
    bool was_omemo_cached = false;  // set when plaintext came from omemo_plaintext cache
    inbound_thread_storage = inbound.attr_string("thread");
    thread = inbound_thread_storage.empty() ? nullptr : inbound_thread_storage.c_str();
    
    // XEP-0359: Unique and Stable Stanza IDs
    const auto stanza_id_view = inbound.child("stanza-id", "urn:xmpp:sid:0");
    stanza_id_attr_storage = stanza_id_view.attr_string("id");
    stanza_id_by_attr_storage = stanza_id_view.attr_string("by");
    const char *stanza_id = !override_archive_id.empty() ? override_archive_id.data()
                            : stanza_id_attr_storage.empty() ? nullptr : stanza_id_attr_storage.c_str();
    const char *stanza_id_by = stanza_id_by_attr_storage.empty() ? nullptr : stanza_id_by_attr_storage.c_str();
    
    const auto origin_id_view = inbound.child("origin-id", "urn:xmpp:sid:0");
    origin_id_attr_storage = origin_id_view.attr_string("id");
    const char *origin_id = origin_id_attr_storage.empty() ? nullptr : origin_id_attr_storage.c_str();
    
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
    const auto msg_view = inbound;
    const bool receipt_requested = ::xmpp::stanza_requests_receipt(msg_view);
    const bool marker_markable = ::xmpp::stanza_is_marker_markable(msg_view);
    const bool is_delayed_delivery = ::xmpp::stanza_is_delayed_delivery(msg_view);

    std::string channel_id_storage;
    if (auto channel_jid = ::xmpp::conversation_channel_jid(
            from_bare ? std::string_view(from_bare) : std::string_view{},
            to_bare ? std::string_view(to_bare) : std::string_view{},
            own_jid_str))
        channel_id_storage = account.resolve_channel_key(*channel_jid);
    else if (from_bare)
        channel_id_storage = account.resolve_channel_key(from_bare);
    else if (to_bare)
        channel_id_storage = account.resolve_channel_key(to_bare);
    const char *channel_id = channel_id_storage.empty() ? nullptr : channel_id_storage.c_str();
    parent_channel = channel_id ? find_channel_for_partner(channel_id) : nullptr;
    const char *pm_id = from_is_account ? to : from;
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
        auto ui = weechat::UiPort::for_buffer(account.buffer);
        for (const auto& line :
             ::xmpp::render_direct_muc_invite_notification(*invite).network_lines)
            ui->printf_network(line);
        return 1;
    }

    // XEP-0045 §7.8.2: Mediated MUC invitations
    if (auto mediated = ::xmpp::parse_mediated_muc_invite(::xmpp::StanzaView(stanza)))
    {
        const std::string inviter = mediated->inviter_bare
            ? *mediated->inviter_bare
            : mediated->room_jid;
        account.track_pending_mediated_invite(
            mediated->room_jid,
            inviter,
            mediated->password
                ? std::optional<std::string_view>{*mediated->password}
                : std::nullopt);

        auto ui = weechat::UiPort::for_buffer(account.buffer);
        for (const auto& line :
             ::xmpp::render_mediated_muc_invite_notification(*mediated).network_lines)
            ui->printf_network(line);
        return 1;
    }

    // XEP-0045 §7.8.2: Mediated invitation declined (room notifies inviter)
    if (auto declined = ::xmpp::parse_mediated_muc_decline(::xmpp::StanzaView(stanza)))
    {
        auto ui = weechat::UiPort::for_buffer(account.buffer);
        ui->printf_network(
            ::xmpp::render_mediated_muc_decline_notification(*declined));
        return 1;
    }

    encrypted = ::xmpp::stanza_axolotl_encrypted(inbound).raw();

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
            channel && channel->type == weechat::channel::chat_type::PM,
            is_mam_replay))
    {
        account.omemo.note_peer_traffic(account.context, channel->id);
    }

    x = inbound.child("x", "jabber:x:encrypted").raw();
    
    // XEP-0380: Explicit Message Encryption
    const auto eme_view = inbound.child("encryption", "urn:xmpp:eme:0");
    eme_namespace_storage = eme_view.attr_string("namespace");
    eme_name_storage = eme_view.attr_string("name");
    const char *eme_namespace = eme_namespace_storage.empty() ? nullptr : eme_namespace_storage.c_str();
    const char *eme_name = eme_name_storage.empty() ? nullptr : eme_name_storage.c_str();
    
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
    // Outbound copies sent from this device cannot be Signal-decrypted (the inbound
    // session at {own_jid, own_device_id} is never established). Live duplicates
    // (MAM page echo, SM replay) must skip decode() entirely — attempting it on
    // multi-key legacy headers has caused multi-second hangs and connection loss.
    const bool skip_own_device_omemo_decode = is_self_outbound_copy && is_own_device_self_copy;
    if (const auto self_copy_advice = ::xmpp::evaluate_omemo_self_copy_advice(
            encrypted != nullptr,
            is_self_outbound_copy,
            is_mam_replay,
            is_own_device_self_copy,
            is_carbon_copy);
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
        // For live copies from this device, leave |encrypted| set so
        // should_skip_display_after_omemo() silently discards the duplicate;
        // decode() is gated separately (skip_own_device_omemo_decode).
        if (self_copy_advice.clear_encrypted_on_mam)
            encrypted = nullptr;
    }

    // XEP-0071: XHTML-IM — prefer <html><body> rich rendering over plain <body>.
    // We apply it whenever XHTML is present AND the message is not encrypted
    // (encrypted messages have no usable XHTML anyway).
    // If plain <body> is also absent we use the XHTML as the sole text source.
    // XEP-0231: Movim stickers use <img src='cid:…@bob.xmpp.org'/> without inline
    // BoB data — skip XHTML text when icat will fetch/display the image.
    const bool icat_enabled_early = weechat::config::instance
        && weechat::config::instance->look.icat.boolean();
    const bool skip_xhtml_for_bob = icat_enabled_early
        && ::xmpp::message_has_xhtml_bob_images(::xmpp::StanzaView(stanza));
    std::string xhtml_fallback;
    if (!encrypted && !x && !skip_xhtml_for_bob)
    {
        const auto html_view = inbound.child("html", "http://jabber.org/protocol/xhtml-im");
        if (html_view.valid())
        {
            const auto html_body = html_view.child("body");
            if (html_body.valid())
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
    if (skip_xhtml_for_bob)
        intext_storage.clear();

    // Auto-enable OMEMO when receiving encrypted messages (PM or MUC).
    // MUC auto-enable is now safe because we have real-JID tracking + bundle readiness checks.
    if (channel
        && ::xmpp::should_auto_enable_channel_omemo(
            encrypted != nullptr, is_self_outbound_copy, channel->omemo.enabled,
            is_mam_replay))
    {
        weechat::UiPort::for_buffer(channel->buffer)->printf_network(
            "Auto-enabling OMEMO (received encrypted message)");
        channel->omemo.enabled = 1;
        channel->set_transport(weechat::channel::transport::OMEMO, 0);
    }

    
    // Auto-enable PGP for PM channels when receiving PGP encrypted messages
    if (x && channel && channel->type == weechat::channel::chat_type::PM && !channel->pgp.enabled)
    {
        weechat::UiPort::for_buffer(channel->buffer)->printf_network(
            "Auto-enabling PGP (received encrypted message)");
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

    if (encrypted && account.omemo && !skip_own_device_omemo_decode)
    {
        bool omemo_is_duplicate = false;
        // For MUC OMEMO (plan §4.1): the stanza 'from' is room@service/nick.
        // We must pass the *sender's real bare JID* (from the member table) to
        // decode() so that Signal sessions and trust are looked up correctly.
        // If we don't have the real JID yet (semi-anonymous or race), skip decode
        // entirely — never use the room bare JID for Signal session lookup.
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
        const bool is_muc_omemo = channel
            && channel->type == weechat::channel::chat_type::MUC;
        const auto decode_jid_opt =
            ::xmpp::resolve_omemo_decode_jid(is_muc_omemo, from_bare, muc_sender_real_jid);
        if (!decode_jid_opt)
        {
            const std::string sender_nick = ::jid(nullptr, from).resource;
            XDEBUG("omemo: skipping MUC decode — no real_jid for occupant nick={} in {}",
                   sender_nick.empty() ? from : sender_nick, channel_id ? channel_id : from_bare);
            if (is_mam_replay)
            {
                intext_storage = std::string(::xmpp::k_omemo_undecryptable_placeholder);
                encrypted = nullptr;
                goto message_handler_after_omemo;
            }
            weechat::UiPort::for_buffer(channel->buffer)->printf_error(
                fmt::format("OMEMO: cannot decrypt MUC message — real JID unknown for occupant {}",
                            sender_nick.empty() ? from : sender_nick));
            return 1;
        }
        const std::string &decode_jid_storage = *decode_jid_opt;
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
                account.mam_cache_store_omemo_plaintext_ids(
                    ch,
                    ::xmpp::omemo_plaintext_cache_ids({
                        stanza_id ? std::string_view(stanza_id) : std::string_view{},
                        origin_id ? std::string_view(origin_id) : std::string_view{},
                        id ? std::string_view(id) : std::string_view{}}),
                    omemo_cleartext_storage,
                    /*batched=*/is_mam_replay);
            }
        }
        if (!cleartext)
        {
            const auto failure = ::xmpp::disposition_for_omemo_decrypt_failure({
                is_self_outbound_copy,
                is_mam_replay,
                is_carbon_copy,
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
                XDEBUG("omemo: decryption failed for {} (from={})", decode_jid, from);
                return 1;
            }
        }
    }
    else
    {
        if (encrypted && !is_self_outbound_copy)
            weechat::UiPort::for_buffer(nullptr)->printf_error(
                "OMEMO: encrypted message but account.omemo is nullptr/false");
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
            encrypted != nullptr, cleartext != nullptr, is_self_outbound_copy, is_mam_replay,
            is_carbon_copy))
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
            (void)weechat::apply_render_event(
                channel->buffer,
                weechat::build_correction_render_event(
                    correction_target_storage,
                    ::xmpp::format_message_correction_text(text)));
        }
        return 1;
    }

    // XEP-0425 v0.3: Message Moderation (extends XEP-0424)
    if (auto moderation = ::xmpp::parse_moderated_retraction(::xmpp::StanzaView(stanza)))
    {
        if (!channel
            || !::xmpp::should_accept_moderation_from_sender(from_bare, channel->id))
            return 1;

        const char *moderation_channel_id = channel_id;
        account.mam_cache_retract_message(moderation_channel_id, moderation->target_id.c_str());

        std::optional<std::string_view> moderate_reason_view;
        if (moderation->reason)
            moderate_reason_view = *moderation->reason;
        const std::string tombstone =
            ::xmpp::format_moderation_tombstone(moderate_reason_view);

        const auto moderation_lookup = weechat::apply_render_event(
            channel->buffer,
            weechat::build_moderation_tombstone_render_event(
                moderation->target_id,
                tombstone,
                "xmpp_retracted,xmpp_moderated,notify_none"));

        auto mod_ui = weechat::UiPort::for_buffer(channel->buffer);
        if (moderation_lookup
            && *moderation_lookup == weechat::LineStoreLookupResult::Found)
        {
            if (moderation->reason)
            {
                mod_ui->printf_network(
                    fmt::format("{} moderated a message: {}", from_bare, *moderation->reason));
            }
            else
            {
                mod_ui->printf_network(fmt::format("{} moderated a message", from_bare));
            }
        }
        else if (moderation->reason)
        {
            mod_ui->printf_network(fmt::format(
                "{} moderated a message (not found in buffer): {}", from_bare, *moderation->reason));
        }
        else
        {
            mod_ui->printf_network(
                fmt::format("{} moderated a message (not found in buffer)", from_bare));
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

        const char *retraction_channel_id = channel_id;
        account.mam_cache_retract_message(retraction_channel_id, retraction->target_id.c_str());

        const auto retraction_lookup = weechat::apply_render_event(
            channel->buffer,
            weechat::build_retraction_tombstone_render_event(
                retraction->target_id,
                ::xmpp::format_retraction_tombstone(),
                "xmpp_retracted,notify_none",
                retraction_sender,
                retraction_occupant_id,
                prefer_occupant_id));

        auto retr_ui = weechat::UiPort::for_buffer(channel->buffer);
        if (retraction_lookup
            && *retraction_lookup == weechat::LineStoreLookupResult::Found)
        {
            retr_ui->printf_network(fmt::format("{} retracted a message", from_bare));
        }
        else if (!retraction_lookup
                 || *retraction_lookup != weechat::LineStoreLookupResult::SenderRejected)
        {
            retr_ui->printf_network(
                fmt::format("{} retracted a message (not found in buffer)", from_bare));
        }

        return 1;
    }


    // XEP-0444: Message Reactions
    if (auto reactions = ::xmpp::parse_message_reactions(::xmpp::StanzaView(stanza)))
    {
        if (channel)
        {
            (void)weechat::apply_render_event(
                channel->buffer,
                weechat::build_reactions_render_event(
                    reactions->target_id, reactions->emojis));
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
    const auto delay_view = inbound.child("delay", "urn:xmpp:delay");
    delay = delay_view.raw();
    timestamp_attr_storage = !override_delay_stamp.empty()
        ? std::string(override_delay_stamp) : delay_view.attr_string("stamp");
    timestamp = timestamp_attr_storage.empty() ? nullptr : timestamp_attr_storage.c_str();
    delay_from_attr_storage = delay_view.attr_string("from");
    const char *delay_from = delay_from_attr_storage.empty() ? nullptr : delay_from_attr_storage.c_str();
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
        const std::string needle = fmt::format("stanza_id_{}", stanza_id);
        if (weechat::line_store_buffer_contains_any_tag(channel->buffer, {needle}))
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
        const auto occ_id_view = inbound.child("occupant-id", "urn:xmpp:occupant-id:0");
        if (occ_id_view.valid())
        {
            const std::string occ_id = occ_id_view.attr_string("id");
            if (!occ_id.empty())
            {
                weechat_string_dyn_concat(dyn_tags, ",occupant_id_", -1);
                weechat_string_dyn_concat(dyn_tags, occ_id.c_str(), -1);
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
        bool no_store = inbound.child("no-store", "urn:xmpp:hints").valid()
                     || inbound.child("no-permanent-store", "urn:xmpp:hints").valid();
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
        for (const auto &ref : inbound)
        {
            if (xep0372_mentioned) break;
            auto ref_ns = ref.xmlns();
            if (!ref_ns || ref.name() != "reference"
                || *ref_ns != "urn:xmpp:reference:0")
                continue;
            const std::string ref_type = ref.attr_string("type");
            if (ref_type != "mention") continue;
            const std::string ref_uri = ref.attr_string("uri");
            if (ref_uri.empty()) continue;
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
    struct pending_icat_preview {
        std::string source;
        size_t width = 0;
        size_t height = 0;
        std::string mime;
    };
    std::optional<pending_icat_preview> pending_icat;

    // XEP-0066: Out of Band Data - extract URL from <x xmlns='jabber:x:oob'>
    const auto oob_view = inbound.child("x", "jabber:x:oob");
    std::string oob_suffix;
    if (oob_view.valid())
    {
        const std::string url_text = oob_view.child("url").text();
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

            const std::string desc_text = oob_view.child("desc").text();

                // Format: [URL: url] or [URL: description (url)]
                if (!desc_text.empty())
                {
                    oob_suffix = fmt::format(" {}[URL: {} ({})]{}",
                                            weechat::RuntimePort::default_runtime().color("blue"),
                                            desc_text, url_text,
                                            weechat::RuntimePort::default_runtime().color("resetcolor"));
                }
                else
                {
                    oob_suffix = fmt::format(" {}[URL: {}]{}",
                                            weechat::RuntimePort::default_runtime().color("blue"),
                                            url_text,
                                            weechat::RuntimePort::default_runtime().color("resetcolor"));
                }
        }
    }

    // XEP-0385 / XEP-0447 / XEP-0448: SIMS and SFS display suffixes
    std::string sims_suffix;
    const auto media_view = ::xmpp::StanzaView(stanza);
    const bool icat_enabled = weechat::config::instance
        && weechat::config::instance->look.icat.boolean();
    const bool is_sticker = icat_enabled && ::xmpp::stanza_has_sticker(media_view);
    const auto emoji_hash_keys = icat_enabled
        ? ::xmpp::collect_emoji_markup_hash_keys(media_view)
        : std::unordered_set<std::string>{};
    const auto custom_emoji_previews = icat_enabled
        ? ::xmpp::collect_custom_emoji_previews(media_view)
        : std::vector<::xmpp::CustomEmojiPreview>{};
    const auto skip_inline_file_suffix =
        [&](const ::xmpp::FileMetadata &meta) -> bool {
            if (!icat_enabled || !is_image_mime_type(meta.mime))
                return false;
            if (is_sticker)
                return true;
            if (emoji_hash_keys.empty())
                return false;
            return std::ranges::any_of(
                meta.hashes,
                [&](const ::xmpp::FileHash &hash) {
                    return emoji_hash_keys.contains(
                        ::xmpp::file_hash_key(hash.algo, hash.value_b64));
                });
        };

    for (const auto &share : ::xmpp::collect_sims_shares(media_view))
    {
        if (is_image_mime_type(share.meta.mime))
        {
            incoming_image_url = share.url;
            incoming_image_mime = share.meta.mime;
            incoming_image_width = share.meta.width;
            incoming_image_height = share.meta.height;
        }

        if (!skip_inline_file_suffix(share.meta))
        {
            sims_suffix += ::xmpp::format_file_share_suffix(
                share.meta.name, share.meta.mime, share.meta.size_raw, share.url);
        }

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
            std::optional<std::string> esfs_local_path;
            if (!esfs_stable_id.empty() && !esfs_channel_jid.empty())
            {
                if (auto prev = account.mam_cache_lookup_esfs_download(
                        esfs_channel_jid, esfs_stable_id))
                {
                    sims_suffix += ::xmpp::format_encrypted_file_saved_suffix(
                        enc.meta.name, *prev);
                    already_downloaded = true;
                    esfs_local_path = *prev;
                }
            }

            if (!already_downloaded)
            {
                if (is_mam_replay)
                {
                    if (auto path = esfs_download_sync(
                            enc.ciphertext_url, enc.meta.name, enc.key_b64, enc.iv_b64,
                            &account, esfs_channel_jid, esfs_stable_id))
                    {
                        sims_suffix += ::xmpp::format_encrypted_file_saved_suffix(
                            enc.meta.name, *path);
                        already_downloaded = true;
                        esfs_local_path = std::move(*path);
                    }
                    else
                    {
                        sims_suffix += ::xmpp::format_encrypted_file_suffix(
                            enc.meta.name, enc.meta.size_raw);
                    }
                }
                else
                {
                    esfs_start_download(enc.ciphertext_url, enc.meta.name, enc.key_b64, enc.iv_b64,
                                        channel ? channel->buffer : account.buffer,
                                        &account, esfs_channel_jid, esfs_stable_id);
                    sims_suffix += ::xmpp::format_encrypted_file_suffix(
                        enc.meta.name, enc.meta.size_raw);
                }
            }

            if (already_downloaded && esfs_local_path && is_image_mime_type(enc.meta.mime))
            {
                pending_icat = pending_icat_preview{
                    *esfs_local_path,
                    enc.meta.width,
                    enc.meta.height,
                    enc.meta.mime,
                };
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

        if (!skip_inline_file_suffix(sfs.meta))
        {
            sims_suffix += ::xmpp::format_file_share_suffix(
                sfs.meta.name, sfs.meta.mime, sfs.meta.size_raw, sfs_url);
        }
    }

    // XEP-0511: Link Metadata — parse <rdf:Description> containing OpenGraph metadata.
    // Previews are collected here and printed as separate buffer lines after the main message.
    // Live messages populate the LMDB og_previews cache; MAM replays look up the cache.
    std::vector<account::og_preview> og_previews_to_show;

    bool has_rdf_description = false;
    for (const auto &desc : inbound)
    {
        auto desc_ns = desc.xmlns();
        if (desc.name() != "Description" || !desc_ns
            || *desc_ns != "http://www.w3.org/1999/02/22-rdf-syntax-ns#")
            continue;

        has_rdf_description = true;
        account::og_preview p;
        for (const auto &prop : desc)
        {
            auto prop_ns = prop.xmlns();
            if (!prop_ns || *prop_ns != "https://ogp.me/ns#") continue;

            const std::string val = prop.text();
            if (val.empty()) continue;

            if (prop.name() == "title" && p.title.empty())
                p.title = val;
            else if (prop.name() == "description" && p.description.empty())
                p.description = val;
            else if (prop.name() == "url" && p.url.empty())
                p.url = val;
            else if (prop.name() == "image" && p.image.empty())
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
    if (!has_rdf_description && is_mam_replay && text)
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

    // /reply local echo already tagged id_<origin-id>; skip the server echo dup.
    bool skip_live_self_render = false;
    if (!date && is_from_self && stable_id && channel && channel->buffer)
    {
        const std::string id_needle = fmt::format("id_{}", stable_id);
        skip_live_self_render = weechat::line_store_buffer_contains_any_tag(
            channel->buffer, {id_needle});
    }

    // XEP-0461: emit reply context as a separate quote line above the message.
    // Nick column shows the replying user (same as the message line below it).
    // Body shows  │ quotedNick: excerpt  in dim/cyan so it reads as a quote block.
    // notify_none,no_log: no highlight, no duplicate log entry.
    auto ch_ui = weechat::UiPort::for_buffer(channel->buffer);
    if (!skip_live_self_render && !reply_prefix.empty())
    {
        const std::string quote_line =
            ::xmpp::format_reply_quote_body(reply_quote_nick, reply_prefix);
        std::string msg = fmt::format("{}\t{}", display_prefix, quote_line);
        ch_ui->printf_date_tags(date,
            "notify_none,no_log,xmpp_reply_quote", msg);
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

    const bool show_encrypted = encrypted || x || was_omemo_cached;
    const std::string status_prefix =
        weechat::format_message_status_prefix("", show_encrypted);

    if (!skip_live_self_render)
    {
        if (channel_id == from_bare && to == channel->id)
        {
            std::string msg = fmt::format("{}\t{}[to {}]: {}{}",
                                          display_prefix,
                                          edit, to, status_prefix,
                                          display_text ? display_text : "");
            ch_ui->printf_date_tags(date, *dyn_tags, msg);
        }
        else if (weechat_string_match(text, "/me *", 0))
        {
            std::string msg = fmt::format("{}\t{}{}{} {}",
                                          weechat::RuntimePort::default_runtime().prefix("action"),
                                          edit, display_prefix,
                                          status_prefix,
                                          display_text ? display_text+4 : "");
            ch_ui->printf_date_tags(date, *dyn_tags, msg);
        }
        else
        {
            std::string msg = fmt::format("{}\t{}{}{}",
                                          display_prefix,
                                          edit, status_prefix,
                                          display_text ? display_text : "");
            ch_ui->printf_date_tags(date, *dyn_tags, msg);
        }
    }

    // weechat-icat: display inline image after the message line (local path during
    // MAM replay; live delivery may pass HTTP URLs to icat.py directly).
    {
        const std::string channel_jid_str = channel_id ? std::string(channel_id) : std::string();
        const std::string stable_id_str = stable_id ? std::string(stable_id) : std::string();

        auto emit_icat = [&](const pending_icat_preview &preview) {
            weechat::icat_preview_request req;
            req.buffer = channel->buffer;
            req.source = preview.source;
            req.width = preview.width;
            req.height = preview.height;
            req.mime = preview.mime;
            req.mam_replay = is_mam_replay;
            req.channel_jid = channel_jid_str;
            req.stable_id = stable_id_str;
            invoke_icat_preview(req, account);
        };

        if (!custom_emoji_previews.empty())
        {
            for (const auto &emoji : custom_emoji_previews)
            {
                emit_icat(pending_icat_preview{
                    emoji.url,
                    emoji.width,
                    emoji.height,
                    emoji.mime,
                });
            }
        }
        else if (pending_icat)
        {
            emit_icat(*pending_icat);
        }
        else if (!incoming_image_url.empty())
        {
            emit_icat(pending_icat_preview{
                incoming_image_url,
                incoming_image_width,
                incoming_image_height,
                incoming_image_mime,
            });
        }

        // XEP-0231: BoB stickers (Movim XHTML-IM cid references).
        if (icat_enabled)
        {
            for (const auto &bob : ::xmpp::collect_bob_image_refs(media_view))
            {
                if (!bob.inline_b64.empty())
                {
                    const auto bytes = ::xmpp::bob_decode_base64(bob.inline_b64);
                    if (bytes.empty())
                        continue;
                    const std::string mime = bob.mime.empty()
                        ? "image/png" : bob.mime;
                    if (auto path = ::xmpp::bob_cache_store_bytes(
                            account, bob.cid, mime, bytes))
                    {
                        emit_icat(pending_icat_preview{*path, 0, 0, mime});
                    }
                    continue;
                }

                if (!from)
                    continue;
                ::xmpp::bob_start_fetch(account, from, bob.cid, bob.mime,
                                        channel->buffer, channel_jid_str,
                                        stable_id_str, is_mam_replay);
            }
        }
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
             ch_ui->printf_date_tags(date,
                 "notify_none,no_log,xmpp_og_preview", msg);
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

xmpp_stanza_t *weechat::connection::get_caps(::xmpp::StanzaView request,
                                             std::optional<std::string> *hash,
                                             const char *node)
{
    struct query_spec : stanza::spec {
        query_spec(const char *n) : spec("query") {
            attr("xmlns", "http://jabber.org/protocol/disco#info");
            if (n && *n) attr("node", n);
        }
    } qs(node);

    const std::string client_name = fmt::format(
        "weechat {}", weechat::RuntimePort::default_runtime().version_string());
    char **serial = weechat_string_dyn_alloc(256);
    weechat_string_dyn_concat(serial, "client/pc//", -1);
    weechat_string_dyn_concat(serial, client_name.c_str(), -1);
    weechat_string_dyn_concat(serial, "<", -1);

    struct identity_spec : stanza::spec {
        identity_spec(std::string_view name) : spec("identity") {
            attr("category", "client");
            attr("name", name);
            attr("type", "pc");
        }
    } ids(client_name);
    qs.child(ids);

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
        "urn:xmpp:bob",
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
            feature_spec(std::string_view v) : spec("feature") { attr("var", v); }
        } fs(ns);
        qs.child(fs);

        weechat_string_dyn_concat(serial, ns.data(), static_cast<int>(ns.size()));
        weechat_string_dyn_concat(serial, "<", -1);
    }

    struct xdata_spec : stanza::spec {
        xdata_spec() : spec("x") {
            attr("xmlns", "jabber:x:data");
            attr("type", "result");
        }
    } xs;

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
    struct value_spec1 : stanza::spec {
        explicit value_spec1(const char *v) : spec("value") { text(v ? v : ""); }
    };
    struct field_spec1 : stanza::spec {
        field_spec1(const char *var_, const char *val_, const char *type_)
            : spec("field")
        {
            attr("var", var_);
            if (type_) attr("type", type_);
            value_spec1 vs(val_);
            child(vs);
        }
    };
    auto add_feature1 = [&](const char *var, const char *type, const char *val) {
        field_spec1 fs(var, val, type);
        xs.child(fs);
        if (std::string_view(var) != "FORM_TYPE") {
            weechat_string_dyn_concat(serial, var, -1);
            weechat_string_dyn_concat(serial, "<", -1);
        }
        weechat_string_dyn_concat(serial, val, -1);
        weechat_string_dyn_concat(serial, "<", -1);
    };
    struct value_spec2 : stanza::spec {
        explicit value_spec2(const char *v) : spec("value") { text(v ? v : ""); }
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
    };
    auto add_feature2 = [&](const char *var, const char *type,
                            const char *val1, const char *val2) {
        field_spec2 fs2(var, type, val1, val2);
        xs.child(fs2);
        weechat_string_dyn_concat(serial, var,  -1);
        weechat_string_dyn_concat(serial, "<",  -1);
        weechat_string_dyn_concat(serial, val1, -1);
        weechat_string_dyn_concat(serial, "<",  -1);
        weechat_string_dyn_concat(serial, val2, -1);
        weechat_string_dyn_concat(serial, "<",  -1);
    };

    add_feature1("FORM_TYPE", "hidden", "urn:xmpp:dataforms:softwareinfo");
    add_feature2("ip_version", "text-multi", "ipv4", "ipv6");
    if (weechat_config_boolean(weechat::config::instance->look.share_os_info))
    {
        add_feature1("os",         nullptr, osinfo.sysname);
        add_feature1("os_version", nullptr, osinfo.release);
    }
    add_feature1("software",         nullptr, "weechat");
    add_feature1("software_version", nullptr,
                  weechat::RuntimePort::default_runtime().version_string().c_str());

    qs.child(xs);

    stanza::iq result_iq;
    result_iq.type("result");
    if (request.valid())
    {
        result_iq.id(request.attr_string("id"));
        if (auto from = request.from())
            result_iq.to(*from);
        if (auto to = request.to())
            result_iq.from(*to);
    }
    result_iq.child(qs);

    auto result_sp = result_iq.build(account.context);

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

    xmpp_stanza_clone(result_sp.get());
    return result_sp.get();
}

// XEP-0004: Data Forms — render a <x xmlns='jabber:x:data'> form to a buffer
void render_data_form(struct t_gui_buffer *buf, xmpp_stanza_t *x_form,
                      const char *jid, const char *node, const char *session_id)
{
    if (!x_form || !buf) return;

    const auto form_view = ::xmpp::StanzaView(x_form);
    const std::string title = form_view.child("title").text();
    const std::string instr = form_view.child("instructions").text();

    auto form_ui = weechat::UiPort::for_buffer(buf);
    form_ui->printf_date_tags_network(0, "xmpp_adhoc,notify_none",
        fmt::format("── Ad-Hoc Form{}{}{} ──{}",
            weechat::RuntimePort::default_runtime().color("bold"),
            title.empty() ? "" : ": ",
            title.empty() ? "" : title,
            title.empty() ? "" : "",
            weechat::RuntimePort::default_runtime().color("-bold")));
    if (!instr.empty())
        form_ui->printf_date_tags_network(0, "xmpp_adhoc,notify_none",
            fmt::format("  {}{}{}",
                weechat::RuntimePort::default_runtime().color("italic"),
                instr,
                weechat::RuntimePort::default_runtime().color("-italic")));

    int field_index = 0;

    // Print each field
    for (const auto &field : form_view)
    {
        if (field.name() != "field") continue;
        const std::string var_s   = field.attr_string("var");
        const std::string label_s = field.attr_string("label");
        const std::string ftype_s = field.attr_string("type");
        const char *var   = var_s.empty() ? nullptr : var_s.c_str();
        const char *label = label_s.empty() ? nullptr : label_s.c_str();
        const char *ftype = ftype_s.empty() ? nullptr : ftype_s.c_str();

        // Skip hidden fields — not user-visible
        if (ftype && std::string_view(ftype) == "hidden") continue;

        field_index++;

        // Collect all <value> children (text-multi and list-multi can have several)
        std::vector<std::string> values;
        for (const auto &v : field)
        {
            if (v.name() != "value") continue;
            values.push_back(v.text());
        }

        // Check if field is required
        bool required = field.child("required").valid();

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
                form_ui->printf_date_tags_network(0, "xmpp_adhoc,notify_none",
                    fmt::format("  {}{}{}",
                        weechat::RuntimePort::default_runtime().color("darkgray"),
                        v,
                        weechat::RuntimePort::default_runtime().color("resetcolor")));
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
                val_display  = std::string(weechat::RuntimePort::default_runtime().color(on ? "green" : "red"));
                val_display += on ? "true" : "false";
                val_display += weechat::RuntimePort::default_runtime().color("resetcolor");
            }
        }
        else if (values.empty())
        {
            val_display = std::string(weechat::RuntimePort::default_runtime().color("darkgray")) + "(empty)"
                        + weechat::RuntimePort::default_runtime().color("resetcolor");
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
                    val_display += std::string(weechat::RuntimePort::default_runtime().color("darkgray"))
                                   + " | " + weechat::RuntimePort::default_runtime().color("resetcolor");
                first = false;
                val_display += v;
            });
        }

        // Collect available options for list-single/list-multi
        // For the selected value(s), highlight them
        std::string options_str;
        if (is_list)
        {
            for (const auto &opt : field)
            {
                if (opt.name() != "option") continue;

                const std::string oval = opt.child("value").text();
                const std::string olabel_s = opt.attr_string("label");
                const char *olabel = olabel_s.empty() ? nullptr : olabel_s.c_str();

                if (!options_str.empty()) options_str += "  ";

                // Is this option currently selected?
                bool selected = false;
                for (auto &v : values)
                    if (!oval.empty() && v == oval) { selected = true; break; }

                if (selected)
                    options_str += weechat::RuntimePort::default_runtime().color("green");
                else
                    options_str += weechat::RuntimePort::default_runtime().color("darkgray");

                options_str += oval.empty() ? "?" : oval;
                if (olabel && !oval.empty() && std::string_view(olabel) != oval)
                {
                    options_str += '(';
                    options_str += olabel;
                    options_str += ')';
                }
                if (selected)
                    options_str += weechat::RuntimePort::default_runtime().color("resetcolor");
                else
                    options_str += weechat::RuntimePort::default_runtime().color("resetcolor");
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
        form_ui->printf_date_tags_network(0, "xmpp_adhoc,notify_none",
            fmt::format("  {}{}.{} {}{}{}{} {}{}{}{}{}{} = {}{}",
                weechat::RuntimePort::default_runtime().color("darkgray"), field_index, weechat::RuntimePort::default_runtime().color("resetcolor"),
                weechat::RuntimePort::default_runtime().color("bold"),
                label ? label : (var ? var : "?"),
                required ? "*" : "",
                weechat::RuntimePort::default_runtime().color("-bold"),
                weechat::RuntimePort::default_runtime().color("darkgray"),
                weechat::RuntimePort::default_runtime().color(type_color),
                var ? var : "?",
                ftype ? "/" : "",
                ftype ? ftype : "",
                weechat::RuntimePort::default_runtime().color("darkgray"),
                weechat::RuntimePort::default_runtime().color("resetcolor"),
                val_display,
                options_str.empty() ? "" : (std::string("  ") + options_str)));
    }

    // Show how to submit
    if (session_id && node && jid)
        form_ui->printf_date_tags_network(0, "xmpp_adhoc,notify_none",
            fmt::format("  {}Submit:{} /adhoc {} {} {} {}var{}={}value{} ...",
                weechat::RuntimePort::default_runtime().color("gray"), weechat::RuntimePort::default_runtime().color("resetcolor"),
                jid, node, session_id,
                weechat::RuntimePort::default_runtime().color("cyan"), weechat::RuntimePort::default_runtime().color("resetcolor"),
                weechat::RuntimePort::default_runtime().color("yellow"), weechat::RuntimePort::default_runtime().color("resetcolor")));
}
