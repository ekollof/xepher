
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

        xmpp_stanza_t *pres__c, *pres__status, *pres__status__text,
            *pres__x, *pres__x__text;

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
            this->handler_add(
                "enabled", nullptr, [](xmpp_conn_t *conn, xmpp_stanza_t *stanza, void *userdata) {
                    auto& connection = *reinterpret_cast<weechat::connection*>(userdata);
                    if (connection != conn) return 0;
                    return connection.sm_handler(stanza) ? 1 : 0;
                });
            this->handler_add(
                "resumed", nullptr, [](xmpp_conn_t *conn, xmpp_stanza_t *stanza, void *userdata) {
                    auto& connection = *reinterpret_cast<weechat::connection*>(userdata);
                    if (connection != conn) return 0;
                    return connection.sm_handler(stanza) ? 1 : 0;
                });
            this->handler_add(
                "failed", nullptr, [](xmpp_conn_t *conn, xmpp_stanza_t *stanza, void *userdata) {
                    auto& connection = *reinterpret_cast<weechat::connection*>(userdata);
                    if (connection != conn) return 0;
                    return connection.sm_handler(stanza) ? 1 : 0;
                });
            this->handler_add(
                "a", nullptr, [](xmpp_conn_t *conn, xmpp_stanza_t *stanza, void *userdata) {
                    auto& connection = *reinterpret_cast<weechat::connection*>(userdata);
                    if (connection != conn) return 0;
                    return connection.sm_handler(stanza) ? 1 : 0;
                });
            this->handler_add(
                "r", nullptr, [](xmpp_conn_t *conn, xmpp_stanza_t *stanza, void *userdata) {
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
                weechat_printf(account.buffer, "%sAttempting to resume SM session (id=%s, h=%u)...",
                              weechat_prefix("network"),
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
        /* children layout: [0]=<c/> [1]=<status/> [2]=<x vcard-temp:x:update/> [3]=<x pgp/> [4]=NULL */
        auto children = std::unique_ptr<xmpp_stanza_t*[]>(new xmpp_stanza_t*[4 + 1]);

        pres__c = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(pres__c, "c");
        xmpp_stanza_set_ns(pres__c, "http://jabber.org/protocol/caps");
        xmpp_stanza_set_attribute(pres__c, "hash", "sha-1");
        xmpp_stanza_set_attribute(pres__c, "node", "http://weechat.org");

        xmpp_stanza_t *caps = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(caps, "caps");
        char *cap_hash;
        caps = this->get_caps(caps, &cap_hash);
        xmpp_stanza_release(caps);
        xmpp_stanza_set_attribute(pres__c, "ver", cap_hash);
        free(cap_hash);

        children[0] = pres__c;

        pres__status = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(pres__status, "status");

        pres__status__text = xmpp_stanza_new(account.context);
        xmpp_stanza_set_text(pres__status__text, account.status().data());
        xmpp_stanza_add_child(pres__status, pres__status__text);
        xmpp_stanza_release(pres__status__text);

        children[1] = pres__status;

        /* XEP-0153: vCard-Based Avatars — broadcast own photo hash in presence */
        {
            xmpp_stanza_t *vcard_x = xmpp_stanza_new(account.context);
            xmpp_stanza_set_name(vcard_x, "x");
            xmpp_stanza_set_ns(vcard_x, "vcard-temp:x:update");

            xmpp_stanza_t *photo_elem = xmpp_stanza_new(account.context);
            xmpp_stanza_set_name(photo_elem, "photo");

            weechat::user *self_user = weechat::user::search(&account, account.jid().data());
            if (self_user && !self_user->profile.avatar_hash.empty())
            {
                xmpp_stanza_t *photo_text = xmpp_stanza_new(account.context);
                xmpp_stanza_set_text(photo_text, self_user->profile.avatar_hash.data());
                xmpp_stanza_add_child(photo_elem, photo_text);
                xmpp_stanza_release(photo_text);
            }

            xmpp_stanza_add_child(vcard_x, photo_elem);
            xmpp_stanza_release(photo_elem);

            children[2] = vcard_x;
        }

        children[3] = NULL;

        if (!account.pgp_keyid().empty())
        {
            pres__x = xmpp_stanza_new(account.context);
            xmpp_stanza_set_name(pres__x, "x");
            xmpp_stanza_set_ns(pres__x, "jabber:x:signed");

            pres__x__text = xmpp_stanza_new(account.context);
            auto signature = account.pgp.sign(account.buffer, account.pgp_keyid().data(), account.status().data());
            xmpp_stanza_set_text(pres__x__text, signature ? signature->c_str() : "");
            xmpp_stanza_add_child(pres__x, pres__x__text);
            xmpp_stanza_release(pres__x__text);

            children[3] = pres__x;
            children[4] = NULL;
        }

        {
            xmpp_stanza_t *pres = stanza__presence(account.context, nullptr, children.get(),
                                                    nullptr, account.jid().data(), nullptr, nullptr);
            this->send(pres);
            xmpp_stanza_release(pres);
        }

        // XEP-0172: publish own nickname via PEP so contacts see a display name
        if (!account.nickname().empty())
        {
            xmpp_stanza_t *nick_iq = ::xmpp::xep0172::publish_nick(
                account.context, std::string(account.nickname()).c_str());
            xmpp_stanza_set_from(nick_iq, account.jid().data());
            this->send(nick_iq);
            xmpp_stanza_release(nick_iq);
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

        children[1] = NULL;
        children[0] =
        stanza__iq_pubsub_items(account.context, NULL,
                                "urn:xmpp:omemo:2:devices");
        children[0] =
        stanza__iq_pubsub(account.context, NULL, children.get(),
                          with_noop("http://jabber.org/protocol/pubsub"));
        xmpp_string_guard uuid_g(account.context, xmpp_uuid_gen(account.context));
        const char *uuid = uuid_g.ptr;
        children[0] =
        stanza__iq(account.context, NULL, children.get(), NULL, uuid,
                   account.jid().data(), account.jid().data(),
                   "get");
        // Register IQ id so the result handler can identify this as our own JID's devicelist
        if (uuid && account.omemo)
            account.omemo.pending_iq_jid[uuid] = std::string(account.jid());
        // freed by uuid_g

        this->send(children[0]);
        xmpp_stanza_release(children[0]);

        // Query our own legacy (OMEMO:1/axolotl) devicelist as well.
        // Some sibling clients still publish only this namespace.
        children[1] = NULL;
        children[0] =
        stanza__iq_pubsub_items(account.context, NULL,
                                "eu.siacs.conversations.axolotl.devicelist");
        children[0] =
        stanza__iq_pubsub(account.context, NULL, children.get(),
                          with_noop("http://jabber.org/protocol/pubsub"));
        xmpp_string_guard legacy_uuid_g(account.context, xmpp_uuid_gen(account.context));
        const char *legacy_uuid = legacy_uuid_g.ptr;
        children[0] =
        stanza__iq(account.context, NULL, children.get(), NULL, legacy_uuid,
                   account.jid().data(), account.jid().data(),
                   "get");
        if (legacy_uuid && account.omemo)
            account.omemo.pending_iq_jid[legacy_uuid] = std::string(account.jid());

        this->send(children[0]);
        xmpp_stanza_release(children[0]);

        account.omemo.init(account.buffer, account.name.data());

        if (account.omemo)
        {
            std::string jid_str(account.jid());

            // Publish our bundle unconditionally on connect so remote clients
            // always have fresh pre-keys for our device.
            children[0] =
            account.omemo.get_bundle(account.context, jid_str.data(), NULL);
            if (children[0])
            {
                this->send(children[0]);
                xmpp_stanza_release(children[0]);
            }

            // Also publish the legacy bundle node so OMEMO:1 clients can
            // target our current device id during first-contact bootstrap.
            children[0] =
            account.omemo.get_legacy_bundle(account.context, jid_str.data(), NULL);
            if (children[0])
            {
                this->send(children[0]);
                xmpp_stanza_release(children[0]);
            }

            // Do NOT publish our devicelist here.  We first fetch the server's
            // current list (IQ sent above).  The IQ result handler merges our
            // device_id into the server list and only republishes if our device
            // is absent.  Publishing here — before the result arrives — would
            // overwrite sibling clients' device entries and trigger a ping-pong
            // storm with any other active client on the same account.
        }

        // Discover HTTP File Upload service (XEP-0363)
        weechat_printf(account.buffer, "%sDiscovering upload service...",
                      weechat_prefix("network"));
        
        // Build disco#items query manually
        xmpp_string_guard disco_items_id_g(account.context, xmpp_uuid_gen(account.context));
        const char *disco_items_id = disco_items_id_g.ptr;
        xmpp_stanza_t *items_iq = xmpp_iq_new(account.context, "get", disco_items_id);
        char *server_domain = xmpp_jid_domain(account.context, account.jid().data());
        xmpp_stanza_set_to(items_iq, server_domain);
        xmpp_stanza_set_from(items_iq, account.jid().data());
        
        xmpp_stanza_t *items_query = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(items_query, "query");
        xmpp_stanza_set_ns(items_query, "http://jabber.org/protocol/disco#items");
        xmpp_stanza_add_child(items_iq, items_query);
        xmpp_stanza_release(items_query);
        
        this->send(items_iq);
        xmpp_stanza_release(items_iq);
        xmpp_free(account.context, server_domain);
        // freed by disco_items_id_g

        // Query MAM globally to discover recent conversations
        time_t now = time(NULL);
        time_t start = now - (7 * 86400);  // Last 7 days
        xmpp_string_guard global_mam_id_g(account.context, xmpp_uuid_gen(account.context));
        const char *global_mam_id = global_mam_id_g.ptr;
        account.add_mam_query(global_mam_id, "",  // Empty 'with' means global query
                             std::optional<time_t>(start), std::optional<time_t>(now));
        // Defer OMEMO key-transport sends until all archived messages are
        // replayed (Conversations-style postponed session completion).
        account.omemo.global_mam_catchup = true;
        
        // Build MAM query manually (global query - no 'with' field)
        xmpp_stanza_t *iq = xmpp_iq_new(account.context, "set", global_mam_id);
        xmpp_stanza_set_id(iq, global_mam_id);
        
        xmpp_stanza_t *query = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(query, "query");
        xmpp_stanza_set_ns(query, "urn:xmpp:mam:2");
        
        xmpp_stanza_t *x = xmpp_stanza_new(account.context);
        xmpp_stanza_set_name(x, "x");
        xmpp_stanza_set_ns(x, "jabber:x:data");
        xmpp_stanza_set_attribute(x, "type", "submit");
        
        // FORM_TYPE field
        {
            xmpp_stanza_t *field = xmpp_stanza_new(account.context);
            xmpp_stanza_set_name(field, "field");
            xmpp_stanza_set_attribute(field, "var", "FORM_TYPE");
            xmpp_stanza_set_attribute(field, "type", "hidden");
            
            xmpp_stanza_t *value = xmpp_stanza_new(account.context);
            xmpp_stanza_set_name(value, "value");
            
            xmpp_stanza_t *text = xmpp_stanza_new(account.context);
            xmpp_stanza_set_text(text, "urn:xmpp:mam:2");
            xmpp_stanza_add_child(value, text);
            xmpp_stanza_release(text);
            
            xmpp_stanza_add_child(field, value);
            xmpp_stanza_release(value);
            
            xmpp_stanza_add_child(x, field);
            xmpp_stanza_release(field);
        }
        
        // Start time field
        {
            xmpp_stanza_t *field = xmpp_stanza_new(account.context);
            xmpp_stanza_set_name(field, "field");
            xmpp_stanza_set_attribute(field, "var", "start");
            
            xmpp_stanza_t *value = xmpp_stanza_new(account.context);
            xmpp_stanza_set_name(value, "value");
            
            xmpp_stanza_t *text = xmpp_stanza_new(account.context);
            char time_buf[256];
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%SZ", gmtime(&start));
            xmpp_stanza_set_text(text, time_buf);
            xmpp_stanza_add_child(value, text);
            xmpp_stanza_release(text);
            
            xmpp_stanza_add_child(field, value);
            xmpp_stanza_release(value);
            
            xmpp_stanza_add_child(x, field);
            xmpp_stanza_release(field);
        }
        
        xmpp_stanza_add_child(query, x);
        xmpp_stanza_release(x);
        
        xmpp_stanza_add_child(iq, query);
        xmpp_stanza_release(query);
        
        this->send(iq);
        xmpp_stanza_release(iq);
        // freed by global_mam_id_g

        // Restore existing PM buffers from previous session
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
                if (ptr_type && strcmp(ptr_type, "private") == 0 &&
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
            }
            ptr_buffer = (struct t_gui_buffer*)weechat_hdata_move(hdata_buffer, ptr_buffer, 1);
        }

        // Initialize Client State Indication (XEP-0352)
        account.last_activity = time(NULL);
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
    if (!(resource && strlen(resource)))
    {
        const std::string rand = rand_string(8);
        char ident[64] = {0};
        snprintf(ident, sizeof(ident), "weechat.%s", rand.c_str());

        account.resource(ident);
        resource = account.resource().data();
    }
    {
        char *jid_node = xmpp_jid_node(account.context, jid.data());
        char *jid_domain = xmpp_jid_domain(account.context, jid.data());
        char *full_jid = xmpp_jid_new(account.context, jid_node, jid_domain, resource);
        m_conn.set_jid(full_jid);
        xmpp_free(account.context, full_jid);
        xmpp_free(account.context, jid_domain);
        xmpp_free(account.context, jid_node);
    }
    {
        std::unique_ptr<char, decltype(&free)> evaled_pass(
            weechat_string_eval_expression(password.data(), NULL, NULL, NULL), free);
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
    // are surfaced as warnings rather than silently leaving conn->tls == NULL.
    // A NULL conn->tls with a TLS write interface still set causes a crash in
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
        return 1; // accept cert, keep conn->tls valid, avoid NULL-deref crash
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
