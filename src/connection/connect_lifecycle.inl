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
                        sm_increment_handled_count(connection.account.sm_h_inbound);

                    return connection.presence_handler(stanza, false) ? 1 : 0;
                });
            this->handler_add(
                "message", /*type*/ nullptr, [](xmpp_conn_t *conn, xmpp_stanza_t *stanza, void *userdata) {
                    auto& connection = *reinterpret_cast<weechat::connection*>(userdata);
                    if (connection != conn) return 0;

                    // Increment SM counter for top-level stanzas only (called by libstrophe)
                    if (connection.account.sm_enabled)
                        sm_increment_handled_count(connection.account.sm_h_inbound);

                    return connection.message_handler(stanza, false) ? 1 : 0;
                });
            this->handler_add(
                "iq", nullptr, [](xmpp_conn_t *conn, xmpp_stanza_t *stanza, void *userdata) {
                    auto& connection = *reinterpret_cast<weechat::connection*>(userdata);
                    if (connection != conn) return 0;

                    // Increment SM counter for top-level stanzas only (called by libstrophe)
                    if (connection.account.sm_enabled)
                        sm_increment_handled_count(connection.account.sm_h_inbound);

                    return connection.iq_handler(stanza, false) ? 1 : 0;
                });

            // Stream Management handlers (XEP-0198)
            // Each SM stanza name needs a DISTINCT (handler_fn_ptr, userdata) pair.
            // A single lambda in a loop produces one function pointer for all 5 calls;
            // libstrophe's _handler_add() dedup check would fire for calls 2-5 and
            // silently drop them ("Stanza handler already exists.").
            // Fix: write 5 separate lambda literals — each is a distinct type → distinct
            // function pointer — so all 5 (fn_ptr, userdata=this) pairs are unique.
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

        account.sm_post_connect_done = false;
        account.sm_awaiting_negotiation = false;
        account.sm_resume_attempted = false;

        // XEP-0198 §3.1: negotiate SM before any application stanzas.
        if (account.sm_available)
        {
            account.sm_awaiting_negotiation = true;
            if (!account.sm_id.empty())
            {
                account.sm_resume_attempted = true;
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
        else
        {
            run_post_connect_setup(false);
        }
    }
    else
    {
        account.sm_awaiting_negotiation = false;
        account.sm_resume_attempted = false;

        account.jid_bare_cache_.clear();
        account.pending_carbons_enable_iq_.reset();

        const char *status_text = status == event::disconnect ? "disconnect" :
                                  status == event::fail ? "fail" :
                                  status == event::raw_connect ? "raw-connect" :
                                  "unknown";

        if (status == event::fail && !account.sm_reconnect_host.empty())
        {
            weechat::UiPort::for_buffer(account.buffer)->printf_network(
                "SM reconnect location failed — falling back to standard connection");
            account.sm_reconnect_host.clear();
            account.sm_reconnect_port = 0;
        }

        {
            auto ui = weechat::UiPort::for_buffer(account.buffer);
            const std::string conn_msg = fmt::format(
                "connection event: {} (error={}{}{})",
                status_text, error,
                error != 0 ? ", " : "",
                error != 0 ? std::strerror(error) : "");
            if (stream_error)
                ui->printf_error(conn_msg);
            else
                ui->printf_network(conn_msg);
        }

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
            
            weechat::UiPort::for_buffer(account.buffer)->printf_error(fmt::format(
                "Stream error: {}{}{}",
                err_type,
                err_text ? " - " : "",
                err_text ? err_text : ""));
        }
        
        // Drop SM state on any disconnect so abrupt socket loss does not resume
        // a session the server has already torn down.
        if (status == event::disconnect)
        {
            if (!account.sm_id.empty() && error == 0)
            {
                weechat::UiPort::for_buffer(account.buffer)->printf_network(fmt::format(
                    "SM session {} closed by server", account.sm_id));
            }
            account.sm_id = "";
            account.sm_h_inbound = 0;
            account.sm_h_outbound = 0;
            account.sm_last_ack = 0;
            account.sm_outqueue.clear();
        }

        // On <conflict>, the server kicked us because another session connected
        // with the same full JID (user@domain/resource). Clear the stored resource
        // so that the next connect() call generates a fresh random one, avoiding
        // an infinite kick-reconnect-kick storm between two instances on the same
        // account. We still reconnect so the user's session is restored.
        if (stream_error && stream_error->type == XMPP_SE_CONFLICT)
        {
            weechat::UiPort::for_buffer(account.buffer)->printf_network(
                "<conflict>: resource collision — will reconnect with "
                "a new resource");
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

    // Recreate the underlying xmpp_conn_t to clean up all stale state
    // (handlers, timers, flags) from the previous session. xmpp_disconnect
    // alone doesn't clear timed handlers, causing "Timed handler already
    // exists" and "Flags can be set only for disconnected connection" on
    // reconnect after /account disconnect.
    m_conn.create(*account.context);

    // A fresh xmpp_conn_t has no handlers — re-register on conn_handler.
    account.sm_handlers_registered = false;

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
    flags |= XMPP_CONN_FLAG_DISABLE_SM;  // Disable libstrophe's built-in SM
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
        weechat::UiPort::for_buffer(nullptr)->printf_error(fmt::format(
            "{}: TLS certificate warning for {} (expires {}): {} — connecting anyway",
            WEECHAT_XMPP_PLUGIN_NAME,
            host     ? host     : "(unknown host)",
            notafter ? notafter : "?",
            errormsg ? errormsg : "(no details)"));
        return 1; // accept cert, keep conn->tls valid, avoid nullptr-deref crash
    });

    const char *altdomain = nullptr;
    unsigned short altport = 0;
    if (!account.sm_id.empty()
        && !account.sm_reconnect_host.empty()
        && account.sm_reconnect_port != 0)
    {
        altdomain = account.sm_reconnect_host.c_str();
        altport = account.sm_reconnect_port;
        XDEBUG("SM resume: connecting to preferred location {}:{}",
               account.sm_reconnect_host, altport);
    }

    if (!connect_client(
            altdomain, altport, [](xmpp_conn_t *conn, xmpp_conn_event_t status,
                           int error, xmpp_stream_error_t *stream_error,
                           void *userdata) {
                auto& connection = *reinterpret_cast<weechat::connection*>(userdata);
                if (connection != conn) return;
                connection.conn_handler(static_cast<event>(status), error, stream_error);
            }))
    {
        weechat::UiPort::for_buffer(nullptr)->printf_error(fmt::format(
            "{}: error connecting to {}",
            WEECHAT_XMPP_PLUGIN_NAME, jid));
        return false;
    }

    return true;
}

void weechat::connection::process(xmpp_ctx_t *context, const unsigned long timeout)
{
    xmpp_run_once(context ? context : this->context(), timeout);
    if (account.connected() && !account.omemo.deferred_live_key_transports.empty())
        account.omemo.process_deferred_live_key_transports(account);
}
