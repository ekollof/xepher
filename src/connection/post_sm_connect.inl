void weechat::connection::run_post_connect_setup(bool resumed_session)
{
    if (account.sm_post_connect_done)
        return;
    account.sm_post_connect_done = true;

    if (!resumed_session)
    {
        /* Send initial <presence/> so that we appear online to contacts */
        /* children: <c/> caps, <status/>, <x vcard-temp:x:update/>, optionally <x jabber:x:signed/> */
        {
            std::optional<std::string> cap_hash;
            this->get_caps(::xmpp::StanzaView{}, &cap_hash);

            struct caps_spec : stanza::spec {
                caps_spec(const char *ver) : spec("c") {
                    xmlns<jabber_org::protocol::caps>();
                    attr("hash", "sha-1");
                    attr("node", "http://weechat.org");
                    attr("ver", ver ? ver : "");
                }
            } caps_ch(cap_hash ? cap_hash->c_str() : "");

            struct status_spec : stanza::spec {
                status_spec(std::string_view s) : spec("status") { text(s); }
            } status_ch(account.status());

            // XEP-0153: vCard-Based Avatars — broadcast own photo hash in presence
            weechat::user *self_user = weechat::user::search(&account, account.jid().data());
            struct vcard_x_spec : stanza::spec {
                vcard_x_spec(std::string_view hash) : spec("x") {
                    xmlns<vcard_temp::x::update>();
                    struct photo_spec : stanza::spec {
                        photo_spec(std::string_view h) : spec("photo") {
                            if (!h.empty()) text(h);
                        }
                    } photo(hash);
                    child(photo);
                }
            } vcard_ch(self_user && !self_user->profile.avatar_hash.empty()
                       ? self_user->profile.avatar_hash : std::string{});

            struct initial_presence : stanza::spec {
                initial_presence(std::string_view from_jid,
                                 caps_spec &c, status_spec &s, vcard_x_spec &v)
                    : spec("presence") {
                    attr("from", from_jid);
                    child(c);
                    child(s);
                    child(v);
                }
            };

            if (!account.pgp_keyid().empty())
            {
                auto signature = account.pgp.sign(account.buffer,
                                                  account.pgp_keyid().data(),
                                                  account.status().data());
                struct pgp_x_spec : stanza::spec {
                    pgp_x_spec(std::string_view sig) : spec("x") {
                        xmlns<jabber::x::signed_>();
                        text(sig);
                    }
                } pgp_ch(signature ? *signature : "");

                struct pres_with_pgp : initial_presence {
                    pres_with_pgp(std::string_view from_jid,
                                  caps_spec &c, status_spec &s, vcard_x_spec &v,
                                  pgp_x_spec &p)
                        : initial_presence(from_jid, c, s, v) {
                        child(p);
                    }
                } pres(account.jid(), caps_ch, status_ch, vcard_ch, pgp_ch);
                this->send(pres.build(account.context).get());
            }
            else
            {
                initial_presence pres(account.jid(), caps_ch, status_ch, vcard_ch);
                this->send(pres.build(account.context).get());
            }
        }
    }


    if (!resumed_session)
    {
        // XEP-0437: subscribe to Room Activity Indicators by sending a presence
        // stanza to our own bare JID with an empty <rai> element.
        {
            stanza::presence rai_pres;
            rai_pres.to(account.jid());
            rai_pres.rai_indicator();
            this->send(rai_pres.build(account.context).get());
        }
    }


    if (!resumed_session)
    {
        // XEP-0172: publish own nickname via PEP so contacts see a display name.
        // Only publish when the nick has changed since the last publish in this
        // process; unconditional publishing every reconnect sends a PEP push
        // notification to all subscribed sessions, triggering an unnecessary MAM
        // catchup on every other active client.
        //
        // Seed last_published_nick_ from LMDB on the first connect after a
        // WeeChat restart (cache is empty then).  This prevents a spurious
        // re-publish on every restart when the nick hasn't actually changed.
        {
            std::string nick_str(account.nickname());
            if (!nick_str.empty() && account.last_published_nick_.empty())
                account.last_published_nick_ = account.mam_cursor_get("pep:nick");

            if (!nick_str.empty() && nick_str != account.last_published_nick_)
            {
                XDEBUG("Publishing XEP-0172 nick (changed: '{}' → '{}')",
                       account.last_published_nick_, nick_str);
                struct nick_el : stanza::spec {
                    nick_el(std::string_view n) : spec("nick") {
                        xmlns<jabber_org::protocol::nick>();
                        text(n);
                    }
                } nick_payload(nick_str);
                stanza::xep0060::item nick_item;
                nick_item.payload(nick_payload);
                stanza::xep0060::publish nick_pub("http://jabber.org/protocol/nick");
                nick_pub.item(nick_item);
                stanza::xep0060::pubsub nick_ps;
                nick_ps.publish(nick_pub);
                this->send(stanza::iq()
                            .from(account.jid())
                            .type("set")
                            .id(stanza::uuid(account.context))
                            .xep0060()
                            .pubsub(nick_ps)
                            .build(account.context)
                            .get());
                account.last_published_nick_ = nick_str;
                // Persist so the guard survives a WeeChat restart.
                account.mam_cursor_set("pep:nick", nick_str);
            }
            else if (!nick_str.empty())
            {
                XDEBUG("Skipping XEP-0172 nick publish (unchanged: '{}')", nick_str);
            }
        }
    }


    if (!resumed_session)
    {
        {
            const std::string carbons_enable_id = stanza::uuid(account.context);
            account.pending_carbons_enable_iq_ = carbons_enable_id;
            this->send(stanza::iq()
                        .from(account.jid())
                        .type("set")
                        .id(carbons_enable_id)
                        .xep0280()
                        .enable()
                        .build(account.context)
                        .get());
        }
    }


    if (!resumed_session)
    {
        this->send(stanza::iq()
                    .from(account.jid())
                    .to(account.jid())
                    .type("get")
                    .id(stanza::uuid(account.context))
                    .rfc6121()
                    .query(stanza::rfc6121::query())
                    .build(account.context)
                    .get());

        this->send(stanza::iq()
                    .from(account.jid())
                    .to(account.jid())
                    .type("get")
                    .id(stanza::uuid(account.context))
                    .xep0049()
                    .query(stanza::xep0049::query().bookmarks())
                    .build(account.context)
                    .get());

        // XEP-0402 §5: fetch PEP Native Bookmarks node on connect so we have
        // the full bookmark list, not just push events from the current session.
        {
            stanza::xep0060::items bm_items("urn:xmpp:bookmarks:1");
            stanza::xep0060::pubsub bm_ps;
            bm_ps.items(bm_items);
            this->send(stanza::iq()
                        .from(account.jid())
                        .to(account.jid())
                        .type("get")
                        .id(stanza::uuid(account.context))
                         .xep0060()
                         .pubsub(bm_ps)
                         .build(account.context)
                         .get());
        }

        // XEP-0490 §4: fetch own MDS node on connect to synchronize displayed
        // state across devices (not just push notifications from this session).
        {
            const std::string mds_fetch_id = stanza::uuid(account.context);
            account.pending_mds_fetch_iq_ = mds_fetch_id;
            stanza::xep0060::items mds_items("urn:xmpp:mds:displayed:0");
            stanza::xep0060::pubsub mds_ps;
            mds_ps.items(mds_items);
            this->send(stanza::iq()
                        .from(account.jid())
                        .to(account.jid())
                        .type("get")
                        .id(mds_fetch_id)
                        .xep0060()
                        .pubsub(mds_ps)
                        .build(account.context)
                        .get());
        }
    }

    account.omemo.init(account.buffer, account.name.data());

    if (!resumed_session)
    {
        auto fetch_devicelist = [&](std::string_view node) {
            std::string uid = stanza::uuid(account.context);
            stanza::xep0060::items items_spec(node);
            stanza::xep0060::pubsub ps_spec;
            ps_spec.items(items_spec);
            this->send(stanza::iq()
                        .from(account.jid())
                        .to(account.jid())
                        .type("get")
                        .id(uid)
                        .xep0060()
                        .pubsub(ps_spec)
                        .build(account.context)
                        .get());
            if (account.omemo)
                account.omemo.pending_iq_jid[uid] = std::string(account.jid());
        };

        // Query our own axolotl devicelist on connect.
        fetch_devicelist("eu.siacs.conversations.axolotl.devicelist");

        if (account.omemo)
        {
            std::string jid_str(account.jid());

            // Only publish our axolotl bundle when the local prekey pool has
            // actually changed (newly generated, repaired, or signed-prekey
            // rotated).  Unconditional publishing every reconnect sends a PEP
            // push notification to all subscribed sessions, triggering an
            // unnecessary MAM catchup on every other active client.
            if (account.omemo.needs_bundle_publish(account.context))
            {
                if (std::shared_ptr<xmpp_stanza_t> legacy_bundle_stanza {
                        account.omemo.get_axolotl_bundle(account.context, jid_str.data(), nullptr),
                        xmpp_stanza_release})
                    this->send(legacy_bundle_stanza.get());
            }

            // Do NOT publish our devicelist here.  We first fetch the server's
            // current list (IQ sent above).  The IQ result handler merges our
            // device_id into the server list and only republishes if our device
            // is absent.  Publishing here — before the result arrives — would
            // overwrite sibling clients' device entries and trigger a ping-pong
            // storm with any other active client on the same account.

            // Probe our own bundle node to verify it still exists on the server.
            // needs_bundle_publish() only fires when local prekeys changed; if
            // the server lost the node (e.g. purged) but our prekeys are
            // unchanged, the bundle would never be republished and peers cannot
            // send OMEMO to us.  The item-not-found error handler republishes it.
            if (account.omemo.device_id != 0)
            {
                const auto bundle_node = fmt::format(
                    "eu.siacs.conversations.axolotl.bundles:{}",
                    account.omemo.device_id);
                std::string bundle_uid = stanza::uuid(account.context);
                stanza::xep0060::items bundle_items(bundle_node);
                stanza::xep0060::pubsub bundle_ps;
                bundle_ps.items(bundle_items);
                this->send(stanza::iq()
                            .from(account.jid())
                            .to(account.jid())
                            .type("get")
                            .id(bundle_uid)
                            .xep0060()
                            .pubsub(bundle_ps)
                            .build(account.context)
                            .get());
                account.omemo.pending_iq_jid[bundle_uid] = std::string(account.jid());
            }
        }
    }


    if (!resumed_session)
    {
        // Discover HTTP File Upload service (XEP-0363)
        XDEBUG("Discovering upload service...");

        {
            std::string server_domain = jid(nullptr, account.jid()).domain;
            this->send(stanza::iq()
                        .from(account.jid())
                        .to(server_domain)
                        .type("get")
                        .id(stanza::uuid(account.context))
                        .xep0030()
                        .query_items()
                        .build(account.context)
                        .get());
        }

        // Query MAM globally to discover recent conversations.
        // If we have a persisted RSM cursor from a previous session, use it as
        // an <after> token so we only fetch messages we haven't seen yet.
        // Otherwise fall back to the last 7 days.
        time_t now = time(nullptr);
        time_t fetch_days = weechat::config::instance
            ? static_cast<time_t>(weechat::config::instance->look.mam_fetch_days.integer())
            : 3;
        time_t start = now - (fetch_days * 86400);  // configurable fallback
        std::string global_mam_cursor = account.mam_cursor_get("global");
        const bool has_cursor = !global_mam_cursor.empty();

        {
            std::string global_mam_id = stanza::uuid(account.context);
            account.add_mam_query(global_mam_id.c_str(), "",  // Empty 'with' means global query
                                 has_cursor ? std::optional<time_t>{} : std::optional<time_t>(start),
                                 std::optional<time_t>(now));
            // Defer OMEMO key-transport sends until all archived messages are
            // replayed (Conversations-style postponed session completion).
            account.omemo.global_mam_catchup = true;

            stanza::xep0059::set rsm_set;
            rsm_set.max(50);

            stanza::xep0313::query mam_query;
            if (!has_cursor)
            {
                // No cursor: use a data form with a start-time filter
                stanza::xep0313::x_filter xf;
                xf.start(format_utc_timestamp(start));
                mam_query.filter(xf).rsm(rsm_set);
            }
            else
            {
                // Have cursor: RSM <after> — no time filter, resume from last seen message
                rsm_set.after(global_mam_cursor);
                mam_query.rsm(rsm_set);
            }

            this->send(stanza::iq()
                        .type("set")
                        .id(global_mam_id)
                        .xep0313()
                        .query(mam_query)
                        .build(account.context)
                        .get());
        }
    }

    // Helper: send a XEP-0442 MAM query against a pubsub node.
        // Uses XEP-0413 Order-By (creation date descending) so we get the newest items.
        // The result arrives as a sequence of forwarded <message> stanzas followed by
        // a <fin> IQ result, handled in iq_handler.inl's pubsub MAM fin block.
        auto send_pubsub_mam_query = [&](std::string_view service_jid,
                                         std::string_view node_name,
                                         int max_items)
        {
            std::string uid = stanza::uuid(account.context);

            // <order xmlns='urn:xmpp:order-by:1' by='creation'/> (XEP-0413 §3)
            // Request newest-first so the RSM <max> limit gives us the most recent items.
            struct order_spec : stanza::spec {
                order_spec() : spec("order") {
                    xmlns<urn::xmpp::order_by::_1>();
                    attr("by", "creation");
                }
            };

            stanza::xep0059::set rsm_set;
            rsm_set.max(static_cast<unsigned>(max_items));

            struct pubsub_mam_query : stanza::xep0313::query {
                pubsub_mam_query(std::string_view node_name_,
                                 order_spec &ord,
                                 stanza::xep0059::set &rsm)
                    : spec("query") {
                    xmlns<urn::xmpp::mam::_2>();
                    attr("node", node_name_);
                    child(ord);
                    child(rsm);
                }
            };

            order_spec ord;
            pubsub_mam_query mam_q(node_name, ord, rsm_set);

            account.pubsub_mam_queries[uid] = {std::string(service_jid), std::string(node_name), {}, max_items};

            this->send(stanza::iq()
                        .from(account.jid())
                        .to(service_jid)
                        .type("set")
                        .id(uid)
                        .xep0313()
                        .query(mam_q)
                        .build(account.context)
                        .get());
        };

        // Helper: restore one feed buffer by its feed_key ("service/node").
        // Creates the in-memory channel, re-fetches the last page of items.
        // Safe to call multiple times for the same key (try_emplace is idempotent).
        // If MAM support on the service is not yet known, defers the fetch until
        // the disco#info response for the service arrives.
        auto restore_feed = [&](std::string_view feed_key)
        {
            if (feed_key.empty() || !account.feed_is_open(feed_key)) return;

            // Re-create the in-memory channel object if it was lost on disconnect
            account.channels.try_emplace(
                std::string(feed_key),
                account, weechat::channel::chat_type::FEED,
                std::string(feed_key), std::string(feed_key));

            // Parse feed_key "service_jid/node" at the first '/'
            auto slash = feed_key.find('/');
            if (slash == std::string::npos) return;

            std::string service_jid = std::string(feed_key.substr(0, slash));
            std::string node_name   = std::string(feed_key.substr(slash + 1));

            // Clear any stale cursor so reconnect always fetches the latest page
            account.mam_cursor_clear(fmt::format("pubsub:{}", feed_key));

            const int max_items = 20;

            // XEP-0442: if the service is known to support MAM, query via MAM.
            if (account.pubsub_mam_services.contains(service_jid))
            {
                send_pubsub_mam_query(service_jid, node_name, max_items);
                return;
            }

            // PEP feeds have the user's own bare JID as the service JID (contains '@').
            // Sending disco#info to a bare JID is unreliable: many servers (including
            // Prosody) return type='error' for such queries, leaving the deferred feed
            // stuck forever.  For PEP nodes, fall straight through to XEP-0060 items.
            bool is_pep = service_jid.contains('@');
            if (is_pep)
            {
                std::string fuid = stanza::uuid(account.context);
                stanza::xep0060::items pep_items(node_name);
                pep_items.max_items(static_cast<unsigned>(max_items));
                stanza::xep0060::pubsub pep_ps;
                pep_ps.items(pep_items);
                account.pubsub_fetch_ids[fuid] = {std::string(service_jid), std::string(node_name), {}, max_items};
                account.connection.send(stanza::iq()
                            .from(account.jid())
                            .to(service_jid)
                            .type("get")
                            .id(fuid)
                            .xep0060()
                            .pubsub(pep_ps)
                            .build(account.context)
                            .get());
                return;
            }

            // The disco#items response handler already queries disco#info for every
            // server component and records upload services. We piggyback on the same
            // disco#info round-trip: check if a MAM-discovery query for this service
            // is already in flight (keyed by service_jid in pubsub_mam_disco_queries).
            bool disco_in_flight = false;
            for (const auto &[disco_id, svc] : account.pubsub_mam_disco_queries)
            {
                if (svc == service_jid) { disco_in_flight = true; break; }
            }

            if (!disco_in_flight)
            {
                // Send a fresh disco#info to learn whether this service supports MAM.
                const std::string disco_id = stanza::uuid(account.context);
                account.pubsub_mam_disco_queries[disco_id] = service_jid;

                account.connection.send(stanza::iq()
                            .from(account.jid())
                            .to(service_jid)
                            .type("get")
                            .id(disco_id)
                            .xep0030()
                            .query()
                            .build(account.context)
                            .get());
            }

            // Defer the actual fetch until the disco#info result arrives.
            account.pubsub_mam_deferred_feeds[std::string(service_jid)].push_back(std::string(feed_key));
        };

        // Track which feed_keys have been scheduled for restore to avoid
        // sending duplicate IQs when both the LMDB list and the buffer walk
        // know about the same feed.
        std::unordered_set<std::string> restored_feeds;

    // Primary restore source: LMDB feed-open registry.
    if (weechat::xmpp_feeds_enabled())
    {
        for (const auto &feed_key : account.feed_open_list())
        {
            if (!resumed_session)
            {
                restore_feed(feed_key);
                restored_feeds.insert(feed_key);
            }
            else if (!feed_key.empty() && account.feed_is_open(feed_key))
            {
                account.channels.try_emplace(
                    std::string(feed_key),
                    account, weechat::channel::chat_type::FEED,
                    std::string(feed_key), std::string(feed_key));
            }
        }
    }

    // Any JID registered via pm_open_register() is a PM conversation to restore.
    for (const auto &pm_jid : account.pm_open_list())
    {
        if (!account.channels.contains(pm_jid))
        {
            XDEBUG("restoring PM channel from LMDB cache: {}", pm_jid);
            try {
                account.channels.emplace(
                    std::make_pair(pm_jid, weechat::channel {
                            account, weechat::channel::chat_type::PM,
                            pm_jid, pm_jid
                        }));
            } catch (const std::exception &ex) {
                weechat::UiPort::for_buffer(nullptr)->printf_error(fmt::format(
                    "xmpp: failed to restore PM channel {}: {}",
                    pm_jid, ex.what()));
            }
        }
    }

    // XEP-0198 resume preserves the c2s session but MUC membership is separate
    // directed presence. Re-join open MUC buffers so nicklists, MAM catch-up,
    // and occupant discovery run again (status 110 + joining flag).
    if (resumed_session)
    {
        const std::string account_nick(account.nickname());
        const std::string account_jid(account.jid());

        for (auto &[room_jid, ch] : account.channels)
        {
            if (ch.type != weechat::channel::chat_type::MUC || !ch.buffer)
                continue;
            if (::xmpp::is_biboumi_gateway_room(room_jid))
                continue;

            std::string_view bookmark_nick;
            if (auto bm_it = account.bookmarks.find(room_jid);
                bm_it != account.bookmarks.end())
                bookmark_nick = bm_it->second.nick;

            const std::string pres_jid = ::xmpp::muc_presence_jid(
                room_jid, bookmark_nick, account_nick, account_jid);

            ch.joining = true;
            weechat::UiPort::for_buffer(account.buffer)->printf_network(
                fmt::format("Re-joining MUC after SM resume: {}", room_jid));
            ::xmpp::send_muc_join_presence(account, pres_jid);
        }
    }

    // Initialize Client State Indication (XEP-0352)
    account.last_activity = time(nullptr);
    account.csi_active = true;

    this->send(stanza::xep0352::active()
               .build(account.context)
               .get());

    account.csi_activity_hooks[0] = (struct t_hook *)weechat_hook_signal(
        "input_text_changed", &account::activity_cb, &account, nullptr);
    account.csi_activity_hooks[1] = (struct t_hook *)weechat_hook_signal(
        "buffer_switch", &account::activity_cb, &account, nullptr);
    account.csi_activity_hooks[2] = (struct t_hook *)weechat_hook_signal(
        "key_pressed", &account::activity_cb, &account, nullptr);

    account.idle_timer_hook = (struct t_hook *)weechat_hook_timer(
        60 * 1000, 0, 0, &account::idle_timer_cb, &account, nullptr);

    (void) weechat_hook_signal_send("xmpp_account_connected",
                                    WEECHAT_HOOK_SIGNAL_STRING, account.name.data());
}
