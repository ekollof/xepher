
    return true;
}

bool weechat::connection::conn_handler(event status, int error, xmpp_stream_error_t *stream_error)
{
    // Guard against libstrophe callbacks firing after plugin shutdown has begun.
    // accounts.clear() in plugin::end() destroys account/omemo/connection objects;
    // any callback firing after that point would dereference freed memory.
    if (weechat::g_plugin_unloading)
        return false;

    if (status == event::connect)
    {
        account.disconnected = 0;

        // Populate bare JID cache (avoids re-parsing bound JID on every stanza)
        {
            std::string full = xmpp_conn_get_bound_jid(static_cast<xmpp_conn_t*>(*this));
            auto slash = full.find('/');
            account.jid_bare_cache_ = (slash != std::string::npos) ? full.substr(0, slash) : std::move(full);
        }

        // Only add handlers once (they persist across reconnects via libstrophe)
        if (!account.sm_handlers_registered)
        {
            this->handler_add<jabber::iq::version>(
                "iq", nullptr, [](xmpp_conn_t *conn, xmpp_stanza_t *stanza, void *userdata) {
                    auto& connection = *reinterpret_cast<weechat::connection*>(userdata);
                    if (connection != conn) return 0;
                    return connection.version_handler(stanza) ? 1 : 0;
                });
            this->handler_add<urn::xmpp::time>(
                "iq", nullptr, [](xmpp_conn_t *conn, xmpp_stanza_t *stanza, void *userdata) {
                    auto& connection = *reinterpret_cast<weechat::connection*>(userdata);
                    if (connection != conn) return 0;
                    return connection.time_handler(stanza) ? 1 : 0;
                });
            this->handler_add(
                "presence", nullptr, [](xmpp_conn_t *conn, xmpp_stanza_t *stanza, void *userdata) {
                    auto& connection = *reinterpret_cast<weechat::connection*>(userdata);
                    if (connection != conn) return 0;

                    // Increment SM counter for top-level stanzas only (called by libstrophe)
                    if (connection.account.sm_enabled)
                        connection.account.sm_h_inbound++;

                    return connection.presence_handler(stanza, false) ? 1 : 0;
                });
            this->handler_add(
                "message", /*type*/ nullptr, [](xmpp_conn_t *conn, xmpp_stanza_t *stanza, void *userdata) {
                    auto& connection = *reinterpret_cast<weechat::connection*>(userdata);
                    if (connection != conn) return 0;

                    // Increment SM counter for top-level stanzas only (called by libstrophe)
                    if (connection.account.sm_enabled)
                        connection.account.sm_h_inbound++;

                    return connection.message_handler(stanza, false) ? 1 : 0;
                });
            this->handler_add(
                "iq", nullptr, [](xmpp_conn_t *conn, xmpp_stanza_t *stanza, void *userdata) {
                    auto& connection = *reinterpret_cast<weechat::connection*>(userdata);
                    if (connection != conn) return 0;

                    // Increment SM counter for top-level stanzas only (called by libstrophe)
                    if (connection.account.sm_enabled)
                        connection.account.sm_h_inbound++;

                    return connection.iq_handler(stanza, false) ? 1 : 0;
                });

            // Stream Management handlers (XEP-0198)
            for (const char *sm_name : {"enabled", "resumed", "failed", "a", "r"})
                this->handler_add(
                    sm_name, nullptr, [](xmpp_conn_t *conn, xmpp_stanza_t *stanza, void *userdata) {
                        auto& connection = *reinterpret_cast<weechat::connection*>(userdata);
                        if (connection != conn) return 0;
                        return connection.sm_handler(stanza) ? 1 : 0;
                    });
            
            account.sm_handlers_registered = true;
        }

        // XEP-0198 §3.1: <enable>/<resume> MUST be sent immediately after
        // stream feature negotiation, before any application stanzas, so that
        // SM covers the entire session from the start.
        if (account.sm_available)
        {
            if (!account.sm_id.empty() && account.sm_h_inbound > 0)
            {
                XDEBUG("Attempting to resume SM session (id={}, h={})...",
                       account.sm_id.data(),
                       account.sm_h_inbound);
                this->send(stanza::xep0198::resume(account.sm_h_inbound, account.sm_id)
                           .build(account.context)
                           .get());
            }
            else
            {
                this->send(stanza::xep0198::enable(true, 300)
                           .build(account.context)
                           .get());
            }
        }

        /* Send initial <presence/> so that we appear online to contacts */
        /* children: <c/> caps, <status/>, <x vcard-temp:x:update/>, optionally <x jabber:x:signed/> */
        {
            // Compute entity caps hash
            xmpp_stanza_t *caps_raw = xmpp_stanza_new(account.context);
            xmpp_stanza_set_name(caps_raw, "caps");
            char *cap_hash_raw = nullptr;
            caps_raw = this->get_caps(caps_raw, &cap_hash_raw);
            xmpp_stanza_release(caps_raw);
            std::unique_ptr<char[]> cap_hash(cap_hash_raw);

            struct caps_spec : stanza::spec {
                caps_spec(const char *ver) : spec("c") {
                    xmlns<jabber_org::protocol::caps>();
                    attr("hash", "sha-1");
                    attr("node", "http://weechat.org");
                    attr("ver", ver ? ver : "");
                }
            } caps_ch(cap_hash.get());

            struct status_spec : stanza::spec {
                status_spec(std::string_view s) : spec("status") { text(s); }
            } status_ch(account.status());

            // XEP-0153: vCard-Based Avatars — broadcast own photo hash in presence
            weechat::user *self_user = weechat::user::search(&account, account.jid().data());
            struct vcard_x_spec : stanza::spec {
                vcard_x_spec(const std::string &hash) : spec("x") {
                    xmlns<vcard_temp::x::update>();
                    struct photo_spec : stanza::spec {
                        photo_spec(const std::string &h) : spec("photo") {
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

        // XEP-0172: publish own nickname via PEP so contacts see a display name
        if (!account.nickname().empty())
        {
            std::string nick_str(account.nickname());
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
        }

        this->send(stanza::iq()
                    .from(account.jid())
                    .type("set")
                    .id(stanza::uuid(account.context))
                    .xep0280()
                    .enable()
                    .build(account.context)
                    .get());

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

        fetch_devicelist("urn:xmpp:omemo:2:devices");
        // Query our own legacy (OMEMO:1/axolotl) devicelist as well.
        // Some sibling clients still publish only this namespace.
        fetch_devicelist("eu.siacs.conversations.axolotl.devicelist");

        account.omemo.init(account.buffer, account.name.data());

        if (account.omemo)
        {
            std::string jid_str(account.jid());

            // Publish our bundle unconditionally on connect so remote clients
            // always have fresh pre-keys for our device.
            if (std::shared_ptr<xmpp_stanza_t> bundle_stanza {
                    account.omemo.get_bundle(account.context, jid_str.data(), nullptr),
                    xmpp_stanza_release})
                this->send(bundle_stanza.get());

            // Also publish the legacy bundle node so OMEMO:1 clients can
            // target our current device id during first-contact bootstrap.
            if (std::shared_ptr<xmpp_stanza_t> legacy_bundle_stanza {
                    account.omemo.get_legacy_bundle(account.context, jid_str.data(), nullptr),
                    xmpp_stanza_release})
                this->send(legacy_bundle_stanza.get());

            // Do NOT publish our devicelist here.  We first fetch the server's
            // current list (IQ sent above).  The IQ result handler merges our
            // device_id into the server list and only republishes if our device
            // is absent.  Publishing here — before the result arrives — would
            // overwrite sibling clients' device entries and trigger a ping-pong
            // storm with any other active client on the same account.
        }

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
        time_t start = now - (7 * 86400);  // Last 7 days (fallback)
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

            stanza::xep0313::query mam_query;
            if (!has_cursor)
            {
                // No cursor: use a data form with a start-time filter
                std::ostringstream time_ss;
                time_ss << std::put_time(gmtime(&start), "%Y-%m-%dT%H:%M:%SZ");
                stanza::xep0313::x_filter xf;
                xf.start(time_ss.str());
                mam_query.filter(xf);
            }
            else
            {
                // Have cursor: RSM <after> — no time filter, resume from last seen message
                stanza::xep0059::set rsm_set;
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

        // Helper: send a XEP-0442 MAM query against a pubsub node.
        // Uses XEP-0413 Order-By (creation date descending) so we get the newest items.
        // The result arrives as a sequence of forwarded <message> stanzas followed by
        // a <fin> IQ result, handled in iq_handler.inl's pubsub MAM fin block.
        auto send_pubsub_mam_query = [&](const std::string &service_jid,
                                         const std::string &node_name,
                                         int max_items)
        {
            std::string uid = stanza::uuid(account.context);

            // <order xmlns='urn:xmpp:order-by:1' field='creation-date'/> (XEP-0413)
            // Request newest-first so the RSM <max> limit gives us the most recent items.
            struct order_spec : stanza::spec {
                order_spec() : spec("order") {
                    xmlns<urn::xmpp::order_by::_1>();
                    attr("field", "creation-date");
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

            account.pubsub_mam_queries[uid] = {service_jid, node_name, {}, max_items};

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
        auto restore_feed = [&](const std::string &feed_key)
        {
            if (feed_key.empty()) return;

            // Re-create the in-memory channel object if it was lost on disconnect
            account.channels.try_emplace(
                feed_key,
                account, weechat::channel::chat_type::FEED,
                feed_key, feed_key);

            // Parse feed_key "service_jid/node" at the first '/'
            auto slash = feed_key.find('/');
            if (slash == std::string::npos) return;

            std::string service_jid = feed_key.substr(0, slash);
            std::string node_name   = feed_key.substr(slash + 1);

            // Clear any stale cursor so reconnect always fetches the latest page
            account.mam_cursor_clear(fmt::format("pubsub:{}", feed_key));

            const int max_items = 20;

            // XEP-0442: if the service is known to support MAM, query via MAM.
            if (account.pubsub_mam_services.count(service_jid))
            {
                send_pubsub_mam_query(service_jid, node_name, max_items);
                return;
            }

            // PEP feeds have the user's own bare JID as the service JID (contains '@').
            // Sending disco#info to a bare JID is unreliable: many servers (including
            // Prosody) return type='error' for such queries, leaving the deferred feed
            // stuck forever.  For PEP nodes, fall straight through to XEP-0060 items.
            bool is_pep = service_jid.find('@') != std::string::npos;
            if (is_pep)
            {
                std::string fuid = stanza::uuid(account.context);
                stanza::xep0060::items pep_items(node_name);
                pep_items.max_items(static_cast<unsigned>(max_items));
                stanza::xep0060::pubsub pep_ps;
                pep_ps.items(pep_items);
                account.pubsub_fetch_ids[fuid] = {service_jid, node_name, {}, max_items};
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
            account.pubsub_mam_deferred_feeds[service_jid].push_back(feed_key);
        };

        // Track which feed_keys have been scheduled for restore to avoid
        // sending duplicate IQs when both the LMDB list and the buffer walk
        // know about the same feed.
        std::unordered_set<std::string> restored_feeds;

        // Primary restore source: LMDB feed-open registry.
        // This is the authoritative list and works across full WeeChat restarts.
        for (const auto &feed_key : account.feed_open_list())
        {
            restore_feed(feed_key);
            restored_feeds.insert(feed_key);
        }

        // Secondary restore source: open WeeChat buffers (survives plugin
        // reload within the same WeeChat session even if LMDB was not yet
        // written, e.g. feeds opened before this commit).
        struct t_hdata *hdata_buffer = weechat_hdata_get("buffer");
        struct t_gui_buffer *ptr_buffer = (struct t_gui_buffer*)weechat_hdata_get_list(hdata_buffer, "gui_buffers");

        while (ptr_buffer)
        {
            if (weechat_buffer_get_pointer(ptr_buffer, "plugin") == weechat_plugin)
            {
                const char *ptr_type = weechat_buffer_get_string(ptr_buffer, "localvar_type");
                const char *ptr_account_name = weechat_buffer_get_string(ptr_buffer, "localvar_account");
                const char *ptr_remote_jid = weechat_buffer_get_string(ptr_buffer, "localvar_remote_jid");

                // Restore PM buffers only (MUCs will be restored via bookmarks)
                if (ptr_type && std::string_view(ptr_type) == "private" &&
                    ptr_account_name && account.name == ptr_account_name &&
                    ptr_remote_jid && ptr_remote_jid[0])
                {
                    // Check if channel already exists
                    if (!account.channels.contains(ptr_remote_jid))
                    {
                        // Create channel object for existing buffer
                        account.channels.emplace(
                            std::make_pair(ptr_remote_jid, weechat::channel {
                                    account, weechat::channel::chat_type::PM,
                                    ptr_remote_jid, ptr_remote_jid
                                }));
                    }
                }
                // Restore FEED buffers from previous session that weren't
                // already handled by the LMDB list above.
                else if (ptr_type && std::string_view(ptr_type) == "feed" &&
                         ptr_account_name && account.name == ptr_account_name &&
                         ptr_remote_jid && ptr_remote_jid[0])
                {
                    std::string feed_key(ptr_remote_jid);
                    // Also register in LMDB for future restarts
                    account.feed_open_register(feed_key);
                    if (!restored_feeds.count(feed_key))
                    {
                        restore_feed(feed_key);
                        restored_feeds.insert(feed_key);
                    }
                }
            }
            ptr_buffer = (struct t_gui_buffer*)weechat_hdata_move(hdata_buffer, ptr_buffer, 1);
        }

        // Tertiary restore source: LMDB pm_open registry.
        // Any JID registered via pm_open_register() (written when a PM MAM
        // fetch completes) is a PM conversation to restore.  This handles the
        // case where WeeChat was fully restarted (no open buffers) AND the
        // global MAM catchup returned no new messages so no PM channels were
        // created by the MAM result handler.
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
                    weechat_printf(nullptr, "%sxmpp: failed to restore PM channel %s: %s",
                                   weechat_prefix("error"), pm_jid.c_str(), ex.what());
                }
            }
        }

        // Initialize Client State Indication (XEP-0352)
        account.last_activity = time(nullptr);
        account.csi_active = true;
        
        // Send initial active state
        this->send(stanza::xep0352::active()
                   .build(account.context)
                   .get());
        
        // Hook user activity signals to detect when user becomes active
        account.csi_activity_hooks[0] = (struct t_hook *)weechat_hook_signal("input_text_changed", &account::activity_cb, &account, nullptr);
        account.csi_activity_hooks[1] = (struct t_hook *)weechat_hook_signal("buffer_switch", &account::activity_cb, &account, nullptr);
        account.csi_activity_hooks[2] = (struct t_hook *)weechat_hook_signal("key_pressed", &account::activity_cb, &account, nullptr);
        
        // Set up idle timer (check every 60 seconds)
        account.idle_timer_hook = (struct t_hook *)weechat_hook_timer(60 * 1000, 0, 0,
                                 &account::idle_timer_cb, &account, nullptr);

        (void) weechat_hook_signal_send("xmpp_account_connected",
                                        WEECHAT_HOOK_SIGNAL_STRING, account.name.data());
    }
    else
    {
        account.jid_bare_cache_.clear();

        const char *status_text = status == event::disconnect ? "disconnect" :
                                  status == event::fail ? "fail" :
                                  status == event::raw_connect ? "raw-connect" :
                                  "unknown";

        weechat_printf(account.buffer,
                       "%sconnection event: %s (error=%d%s%s)",
                       weechat_prefix(stream_error ? "error" : "network"),
                       status_text,
                       error,
                       error != 0 ? ", " : "",
                       error != 0 ? std::strerror(error) : "");

        if (stream_error)
        {
            const char *err_text = stream_error->text;
            const char *err_type = stream_error->type == XMPP_SE_BAD_FORMAT ? "bad-format" :
                                   stream_error->type == XMPP_SE_BAD_NS_PREFIX ? "bad-namespace-prefix" :
                                   stream_error->type == XMPP_SE_CONFLICT ? "conflict" :
                                   stream_error->type == XMPP_SE_CONN_TIMEOUT ? "connection-timeout" :
                                   stream_error->type == XMPP_SE_HOST_GONE ? "host-gone" :
                                   stream_error->type == XMPP_SE_HOST_UNKNOWN ? "host-unknown" :
                                   stream_error->type == XMPP_SE_IMPROPER_ADDR ? "improper-addressing" :
                                   stream_error->type == XMPP_SE_INTERNAL_SERVER_ERROR ? "internal-server-error" :
                                   stream_error->type == XMPP_SE_INVALID_FROM ? "invalid-from" :
                                   stream_error->type == XMPP_SE_INVALID_ID ? "invalid-id" :
                                   stream_error->type == XMPP_SE_INVALID_NS ? "invalid-namespace" :
                                   stream_error->type == XMPP_SE_INVALID_XML ? "invalid-xml" :
                                   stream_error->type == XMPP_SE_NOT_AUTHORIZED ? "not-authorized" :
                                   stream_error->type == XMPP_SE_POLICY_VIOLATION ? "policy-violation" :
                                   stream_error->type == XMPP_SE_REMOTE_CONN_FAILED ? "remote-connection-failed" :
                                   stream_error->type == XMPP_SE_RESOURCE_CONSTRAINT ? "resource-constraint" :
                                   stream_error->type == XMPP_SE_RESTRICTED_XML ? "restricted-xml" :
                                   stream_error->type == XMPP_SE_SEE_OTHER_HOST ? "see-other-host" :
                                   stream_error->type == XMPP_SE_SYSTEM_SHUTDOWN ? "system-shutdown" :
                                   stream_error->type == XMPP_SE_UNDEFINED_CONDITION ? "undefined-condition" :
                                   stream_error->type == XMPP_SE_UNSUPPORTED_ENCODING ? "unsupported-encoding" :
                                   stream_error->type == XMPP_SE_UNSUPPORTED_STANZA_TYPE ? "unsupported-stanza-type" :
                                   stream_error->type == XMPP_SE_UNSUPPORTED_VERSION ? "unsupported-version" :
                                   stream_error->type == XMPP_SE_XML_NOT_WELL_FORMED ? "xml-not-well-formed" :
                                   "unknown";
            
            weechat_printf(account.buffer, "%sStream error: %s%s%s",
                          weechat_prefix("error"),
                          err_type,
                          err_text ? " - " : "",
                          err_text ? err_text : "");
        }
        
        // Clear SM session on clean disconnect (server-initiated or normal close)
        // This prevents trying to resume a session the server has already closed
        if (status == event::disconnect && error == 0)
        {
            if (!account.sm_id.empty())
            {
                weechat_printf(account.buffer, "%sSM session %s closed by server",
                              weechat_prefix("network"), account.sm_id.data());
            }
            account.sm_id = "";
            account.sm_h_inbound = 0;
            account.sm_h_outbound = 0;
            account.sm_last_ack = 0;
        }

        // On <conflict>, the server kicked us because another session connected
        // with the same full JID (user@domain/resource). Clear the stored resource
        // so that the next connect() call generates a fresh random one, avoiding
        // an infinite kick-reconnect-kick storm between two instances on the same
        // account. We still reconnect so the user's session is restored.
        if (stream_error && stream_error->type == XMPP_SE_CONFLICT)
        {
            weechat_printf(account.buffer,
                           "%s<conflict>: resource collision — will reconnect with "
                           "a new resource",
                           weechat_prefix("network"));
            account.resource("");
        }

        account.disconnect(1);
    }

    return true;
}

std::string rand_string(int length)
{
    std::string s(length, '\0');
    for(int i = 0; i < length; ++i){
        s[i] = '0' + rand()%72; // starting on '0', ending on '}'
        if (!((s[i] >= '0' && s[i] <= '9') ||
              (s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= 'a' && s[i] <= 'z')))
            i--; // reroll
    }
    return s;
}

int weechat::connection::connect(std::string jid, std::string password, weechat::tls_policy tls)
{
    static const unsigned ka_timeout_sec = 60;
    static const unsigned ka_timeout_ivl = 1;

    m_conn.set_keepalive(ka_timeout_sec, ka_timeout_ivl);

    const char *resource = account.resource().data();
    if (!(resource && !std::string_view(resource).empty()))
    {
        const std::string rand = rand_string(8);
        auto ident = fmt::format("weechat.{}", rand);

        account.resource(ident);
        resource = account.resource().data();
    }
    {
        ::jid parsed(nullptr, jid);
        std::string full_jid = fmt::format("{}@{}/{}", parsed.local, parsed.domain, resource);
        m_conn.set_jid(full_jid.c_str());
    }
    {
        std::unique_ptr<char, decltype(&free)> evaled_pass(
            weechat_string_eval_expression(password.data(), nullptr, nullptr, nullptr), free);
        m_conn.set_pass(evaled_pass.get());
    }

    int flags = m_conn.get_flags();
    switch (tls)
    {
        case weechat::tls_policy::disable:
            flags |= XMPP_CONN_FLAG_DISABLE_TLS;
            break;
        case weechat::tls_policy::normal:
            flags &= ~XMPP_CONN_FLAG_DISABLE_TLS;
            flags &= ~XMPP_CONN_FLAG_TRUST_TLS;
            break;
        case weechat::tls_policy::trust:
            flags |= XMPP_CONN_FLAG_TRUST_TLS;
            break;
        default:
            break;
    }
    m_conn.set_flags(flags);

    // Register a certfail handler so that TLS certificate verification failures
    // are surfaced as warnings rather than silently leaving conn->tls == nullptr.
    // A nullptr conn->tls with a TLS write interface still set causes a crash in
    // libstrophe's tls_write() when xmpp_run_once() flushes the send queue.
    // Accepting the cert here keeps conn->tls valid; the user sees a clear
    // warning and can use tls_policy::trust to suppress it intentionally.
    m_conn.set_certfail_handler([](const xmpp_tlscert_t *cert,
                                   const char *const errormsg) -> int {
        xmpp_conn_t *conn = xmpp_tlscert_get_conn(cert);
        // JID is not yet known at TLS handshake time; use the cert subject
        // (contains the server CN/hostname) and expiry for context instead.
        const char *subject  = cert ? xmpp_tlscert_get_string(cert, XMPP_CERT_SUBJECT)  : nullptr;
        const char *notafter = cert ? xmpp_tlscert_get_string(cert, XMPP_CERT_NOTAFTER) : nullptr;
        const char *dnsname  = cert ? xmpp_tlscert_get_dnsname(cert, 0)                 : nullptr;
        // Prefer SAN dnsname > subject > remote host from conn
        const char *host = dnsname ? dnsname
                         : subject ? subject
                         : (conn ? xmpp_conn_get_bound_jid(conn) : nullptr);
        weechat_printf(
            nullptr,
            _("%s%s: TLS certificate warning for %s (expires %s): %s — connecting anyway"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
            host     ? host     : "(unknown host)",
            notafter ? notafter : "?",
            errormsg ? errormsg : "(no details)");
        return 1; // accept cert, keep conn->tls valid, avoid nullptr-deref crash
    });

    if (!connect_client(
            nullptr, 0, [](xmpp_conn_t *conn, xmpp_conn_event_t status,
                           int error, xmpp_stream_error_t *stream_error,
                           void *userdata) {
                auto& connection = *reinterpret_cast<weechat::connection*>(userdata);
                if (connection != conn) return;
                connection.conn_handler(static_cast<event>(status), error, stream_error);
            }))
    {
        weechat_printf(
            nullptr,
            _("%s%s: error connecting to %s"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
            jid.data());
        return false;
    }

    return true;
}

void weechat::connection::process(xmpp_ctx_t *context, const unsigned long timeout)
{
    xmpp_run_once(context ? context : this->context(), timeout);
}
