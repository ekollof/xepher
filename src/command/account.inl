void command__display_account(weechat::account *account)
{
    int num_channels, num_pv;

    if (account->connected())
    {
        num_channels = 0;
        num_pv = 0;
        weechat_printf(
            nullptr,
            " %s %s%s%s %s(%s%s%s) [%s%s%s]%s, %d %s, %d pv",
            (account->connected()) ? "*" : " ",
            weechat_color("chat_server"),
            account->name.data(),
            weechat_color("reset"),
            weechat_color("chat_delimiters"),
            weechat_color("chat_server"),
            account->jid().data(),
            weechat_color("chat_delimiters"),
            weechat_color("reset"),
            (account->connected()) ? _("connected") : _("not connected"),
            weechat_color("chat_delimiters"),
            weechat_color("reset"),
            num_channels,
            NG_("channel", "channels", num_channels),
            num_pv);
    }
    else
    {
        weechat_printf(
            nullptr,
            "   %s%s%s %s(%s%s%s)%s",
            weechat_color("chat_server"),
            account->name.data(),
            weechat_color("reset"),
            weechat_color("chat_delimiters"),
            weechat_color("chat_server"),
            account->jid().data(),
            weechat_color("chat_delimiters"),
            weechat_color("reset"));
    }
}

void command__account_list(int argc, char **argv)
{
    int i, one_account_found;
    char *account_name = nullptr;

    for (i = 2; i < argc; i++)
    {
        if (!account_name)
            account_name = argv[i];
    }
    if (!account_name)
    {
        if (!weechat::accounts.empty())
        {
            weechat_printf(nullptr, "");
            weechat_printf(nullptr, _("All accounts:"));
            for (auto& ptr_account2 : weechat::accounts)
            {
                command__display_account(&ptr_account2.second);
            }
        }
        else
            weechat_printf(nullptr, _("No account"));
    }
    else
    {
        one_account_found = 0;
        for (auto& ptr_account2 : weechat::accounts)
        {
            if (weechat_strcasestr(ptr_account2.second.name.data(), account_name))
            {
                if (!one_account_found)
                {
                    weechat_printf(nullptr, "");
                    weechat_printf(nullptr,
                                   _("Servers with \"%s\":"),
                                   account_name);
                }
                one_account_found = 1;
                command__display_account(&ptr_account2.second);
            }
        }
        if (!one_account_found)
            weechat_printf(nullptr,
                           _("No account found with \"%s\""),
                           account_name);
    }
}

// ---------------------------------------------------------------------------
// XEP-0077: In-Band Registration
// ---------------------------------------------------------------------------
// Forward declaration — defined later in this file.
void command__add_account(const char *name, const char *jid, const char *password);

// Holds transient state for a single IBR attempt.  Created on the heap; the
// WeeChat timer callback owns it and deletes it when the operation completes
// (success, error, or disconnect).
struct ibr_state {
    std::string account_name;
    std::string jid;
    std::string password;
    std::string server;           // domain extracted from JID
    struct t_gui_buffer *buffer;  // WeeChat buffer to print messages into
    struct t_hook   *timer_hook;  // the periodic xmpp_run_once hook

    // Stored inline so its lifetime is tied to this struct.
    // xmpp_ctx_new holds a pointer to this — it must outlive ctx.
    xmpp_log_t  logger;
    xmpp_ctx_t  *ctx;
    xmpp_conn_t *conn;
    bool         done;            // set to true once we are finished

    ibr_state(std::string n, std::string j, std::string p,
              std::string srv, struct t_gui_buffer *buf)
        : account_name(std::move(n)), jid(std::move(j)), password(std::move(p)),
          server(std::move(srv)), buffer(buf),
          timer_hook(nullptr),
          logger{
              [](void * /*userdata*/, xmpp_log_level_t level,
                 const char *area, const char *msg) {
                  if (level >= XMPP_LEVEL_WARN)
                      weechat_printf(nullptr, _("%s%s (IBR/%s): %s"),
                                     weechat_prefix("network"),
                                     WEECHAT_XMPP_PLUGIN_NAME, area, msg);
              },
              nullptr
          },
          ctx(nullptr), conn(nullptr), done(false)
    {}

    ~ibr_state() {
        if (timer_hook) { weechat_unhook(timer_hook); timer_hook = nullptr; }
        if (conn)       { xmpp_conn_release(conn);    conn = nullptr; }
        if (ctx)        { xmpp_ctx_free(ctx);         ctx = nullptr; }
        // logger is a value member — destroyed automatically after the body.
    }
};

// WeeChat timer callback: drives the libstrophe event loop for the IBR
// connection.  When done is set the callback deletes the ibr_state (which
// unhooks itself) and returns WEECHAT_RC_OK for the last call.
static int ibr_timer_cb(const void *pointer, void *data, int /*remaining*/)
{
    (void) data;
    auto *st = const_cast<ibr_state *>(reinterpret_cast<const ibr_state *>(pointer));
    if (!st) return WEECHAT_RC_OK;

    xmpp_run_once(st->ctx, 10);

    if (st->done) {
        // Unhook before deleting so weechat doesn't call us again
        struct t_hook *h = st->timer_hook;
        st->timer_hook = nullptr;
        { std::unique_ptr<ibr_state> owned(st); } // RAII delete
        weechat_unhook(h);
    }
    return WEECHAT_RC_OK;
}

// IQ result handler for the IBR <iq type='set'> response.
static int ibr_set_result_handler(xmpp_conn_t *conn, xmpp_stanza_t *stanza, void *userdata)
{
    auto *st = reinterpret_cast<ibr_state *>(userdata);
    if (!st || st->done) return 0;

    const char *type = xmpp_stanza_get_type(stanza);
    if (type && strcmp(type, "result") == 0) {
        // Success — save the account
        weechat_printf(st->buffer,
                       _("%s%s: registration successful for %s — adding account"),
                       weechat_prefix("network"), WEECHAT_XMPP_PLUGIN_NAME,
                       st->jid.c_str());
        command__add_account(st->account_name.c_str(), st->jid.c_str(), st->password.c_str());
    } else {
        // Error
        const char *condition = "unknown";
        xmpp_stanza_t *err = xmpp_stanza_get_child_by_name(stanza, "error");
        if (err) {
            xmpp_stanza_t *cond = xmpp_stanza_get_child_by_name(err, "conflict");
            if (!cond) cond = xmpp_stanza_get_child_by_name(err, "not-acceptable");
            if (!cond) cond = xmpp_stanza_get_child_by_name(err, "service-unavailable");
            if (!cond) cond = xmpp_stanza_get_child_by_name(err, "forbidden");
            if (!cond) cond = xmpp_stanza_get_child_by_name(err, "bad-request");
            if (cond)  condition = xmpp_stanza_get_name(cond);
        }
        weechat_printf(st->buffer,
                       _("%s%s: registration failed: %s"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                       condition);
    }

    xmpp_disconnect(conn);
    st->done = true;
    return 1; // consume
}

// IQ result handler for the IBR <iq type='get'> field-list response.
static int ibr_get_result_handler(xmpp_conn_t *conn, xmpp_stanza_t *stanza, void *userdata)
{
    auto *st = reinterpret_cast<ibr_state *>(userdata);
    if (!st || st->done) return 0;

    const char *type = xmpp_stanza_get_type(stanza);
    if (!type || strcmp(type, "result") != 0) {
        // Server returned an error to our field-list query
        const char *condition = "unknown";
        xmpp_stanza_t *err = xmpp_stanza_get_child_by_name(stanza, "error");
        if (err) {
            xmpp_stanza_t *cond = xmpp_stanza_get_child_by_name(err, "service-unavailable");
            if (!cond) cond = xmpp_stanza_get_child_by_name(err, "forbidden");
            if (cond) condition = xmpp_stanza_get_name(cond);
        }
        weechat_printf(st->buffer,
                       _("%s%s: server rejected IBR query: %s"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                       condition);
        xmpp_disconnect(conn);
        st->done = true;
        return 1;
    }

    xmpp_stanza_t *query = xmpp_stanza_get_child_by_ns(stanza, "jabber:iq:register");
    if (!query) {
        weechat_printf(st->buffer,
                       _("%s%s: IBR: server response missing <query xmlns='jabber:iq:register'>"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        xmpp_disconnect(conn);
        st->done = true;
        return 1;
    }

    // Already registered?
    if (xmpp_stanza_get_child_by_name(query, "registered")) {
        weechat_printf(st->buffer,
                       _("%s%s: IBR: already registered on %s"),
                       weechat_prefix("network"), WEECHAT_XMPP_PLUGIN_NAME,
                       st->server.c_str());
        xmpp_disconnect(conn);
        st->done = true;
        return 1;
    }

    // XEP-0077 §6: x:data form takes precedence over flat fields.
    // A terminal client cannot fill in arbitrary form fields interactively,
    // so we detect this, print the form instructions and field list, and abort
    // with a helpful message directing the user to the server's web registration.
    xmpp_stanza_t *xdata = xmpp_stanza_get_child_by_ns(query, "jabber:x:data");
    if (xdata) {
        weechat_printf(st->buffer,
                       _("%s%s: IBR: server requires a data form for registration"
                         " — web registration required"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);

        // Print the form title/instructions if present
        xmpp_stanza_t *title_el = xmpp_stanza_get_child_by_name(xdata, "title");
        if (title_el) {
            char *title_txt = xmpp_stanza_get_text(title_el);
            if (title_txt) {
                weechat_printf(st->buffer, _("%s%s: IBR form title: %s"),
                               weechat_prefix("network"), WEECHAT_XMPP_PLUGIN_NAME,
                               title_txt);
                xmpp_free(st->ctx, title_txt);
            }
        }
        xmpp_stanza_t *instr_el = xmpp_stanza_get_child_by_name(xdata, "instructions");
        if (instr_el) {
            char *instr_txt = xmpp_stanza_get_text(instr_el);
            if (instr_txt) {
                weechat_printf(st->buffer, _("%s%s: IBR form instructions: %s"),
                               weechat_prefix("network"), WEECHAT_XMPP_PLUGIN_NAME,
                               instr_txt);
                xmpp_free(st->ctx, instr_txt);
            }
        }

        // Print each field's var and label so the user can identify what's needed
        for (xmpp_stanza_t *field = xmpp_stanza_get_children(xdata);
             field; field = xmpp_stanza_get_next(field)) {
            if (!xmpp_stanza_get_name(field) ||
                strcmp(xmpp_stanza_get_name(field), "field") != 0) continue;
            const char *var   = xmpp_stanza_get_attribute(field, "var");
            const char *label = xmpp_stanza_get_attribute(field, "label");
            if (var) {
                if (label)
                    weechat_printf(st->buffer, _("%s%s: IBR form field: %s (%s)"),
                                   weechat_prefix("network"), WEECHAT_XMPP_PLUGIN_NAME,
                                   var, label);
                else
                    weechat_printf(st->buffer, _("%s%s: IBR form field: %s"),
                                   weechat_prefix("network"), WEECHAT_XMPP_PLUGIN_NAME,
                                   var);
            }
        }

        weechat_printf(st->buffer,
                       _("%s%s: IBR: use the server's web interface to register an account"),
                       weechat_prefix("network"), WEECHAT_XMPP_PLUGIN_NAME);
        xmpp_disconnect(conn);
        st->done = true;
        return 1;
    }

    // Warn about required fields beyond username+password that we cannot fill
    {
        static const char *const known_fields[] = { "username", "password", nullptr };
        for (xmpp_stanza_t *child = xmpp_stanza_get_children(query);
             child; child = xmpp_stanza_get_next(child)) {
            const char *cname = xmpp_stanza_get_name(child);
            if (!cname) continue;
            bool is_known = false;
            for (int i = 0; known_fields[i]; ++i) {
                if (strcmp(cname, known_fields[i]) == 0) { is_known = true; break; }
            }
            // Skip housekeeping elements
            if (strcmp(cname, "registered") == 0 || strcmp(cname, "instructions") == 0 ||
                strcmp(cname, "x") == 0) continue;
            if (!is_known) {
                weechat_printf(st->buffer,
                               _("%s%s: IBR: server requires unknown field <%s/>"
                                 " — registration may fail"),
                               weechat_prefix("network"), WEECHAT_XMPP_PLUGIN_NAME,
                               cname);
            }
        }
    }

    // OOB redirect?
    xmpp_stanza_t *oob = xmpp_stanza_get_child_by_ns(stanza, "jabber:x:oob");
    if (!oob) oob = xmpp_stanza_get_child_by_ns(query, "jabber:x:oob");
    if (oob) {
        xmpp_stanza_t *url_el = xmpp_stanza_get_child_by_name(oob, "url");
        char *url_text = url_el ? xmpp_stanza_get_text(url_el) : nullptr;
        weechat_printf(st->buffer,
                       _("%s%s: IBR: server requires out-of-band registration: %s"),
                       weechat_prefix("network"), WEECHAT_XMPP_PLUGIN_NAME,
                       url_text ? url_text : "(no URL provided)");
        if (url_text) xmpp_free(st->ctx, url_text);
        xmpp_disconnect(conn);
        st->done = true;
        return 1;
    }

    // Build the <iq type='set'> registration request
    xmpp_ctx_t *ctx = st->ctx;
    xmpp_stanza_t *iq = xmpp_iq_new(ctx, "set", "ibr-set");
    xmpp_stanza_set_to(iq, st->server.c_str());

    xmpp_stanza_t *q = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(q, "query");
    xmpp_stanza_set_ns(q, "jabber:iq:register");

    // Extract local part of JID for username
    char *node = xmpp_jid_node(ctx, st->jid.c_str());

    xmpp_stanza_t *username_el = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(username_el, "username");
    xmpp_stanza_t *username_text = xmpp_stanza_new(ctx);
    xmpp_stanza_set_text(username_text, node ? node : st->jid.c_str());
    xmpp_stanza_add_child(username_el, username_text);
    xmpp_stanza_release(username_text);
    xmpp_stanza_add_child(q, username_el);
    xmpp_stanza_release(username_el);

    xmpp_stanza_t *password_el = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(password_el, "password");
    xmpp_stanza_t *password_text = xmpp_stanza_new(ctx);
    xmpp_stanza_set_text(password_text, st->password.c_str());
    xmpp_stanza_add_child(password_el, password_text);
    xmpp_stanza_release(password_text);
    xmpp_stanza_add_child(q, password_el);
    xmpp_stanza_release(password_el);

    if (node) xmpp_free(ctx, node);

    xmpp_stanza_add_child(iq, q);
    xmpp_stanza_release(q);

    // Register result handler for the set IQ before sending
    xmpp_id_handler_add(conn, ibr_set_result_handler, "ibr-set", st);

    xmpp_send(conn, iq);
    xmpp_stanza_release(iq);

    return 1; // consume
}

// Connection event callback for the IBR raw connection.
static void ibr_conn_handler(xmpp_conn_t *conn, xmpp_conn_event_t status,
                              int error, xmpp_stream_error_t *stream_error, void *userdata)
{
    (void) stream_error;
    auto *st = reinterpret_cast<ibr_state *>(userdata);
    if (!st) return;

    if (status == XMPP_CONN_RAW_CONNECT) {
        // Open the XML stream to the server (required after raw connect)
        xmpp_conn_open_stream_default(conn);
        return;
    }

    if (status == XMPP_CONN_CONNECT) {
        // Stream is open, authenticated feature negotiation skipped.
        // Send IBR field-list query.
        xmpp_ctx_t *ctx = st->ctx;
        xmpp_stanza_t *iq = xmpp_iq_new(ctx, "get", "ibr-get");
        xmpp_stanza_set_to(iq, st->server.c_str());

        xmpp_stanza_t *query = xmpp_stanza_new(ctx);
        xmpp_stanza_set_name(query, "query");
        xmpp_stanza_set_ns(query, "jabber:iq:register");
        xmpp_stanza_add_child(iq, query);
        xmpp_stanza_release(query);

        xmpp_id_handler_add(conn, ibr_get_result_handler, "ibr-get", st);

        xmpp_send(conn, iq);
        xmpp_stanza_release(iq);
        return;
    }

    // Disconnect or failure
    if (!st->done) {
        if (error != 0) {
            weechat_printf(st->buffer,
                           _("%s%s: IBR: connection failed (error %d: %s)"),
                           weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                           error, strerror(error));
        }
        st->done = true;
    }
}

// Entry point: start an IBR attempt for the given account/JID/password.
// buffer is where messages should be printed (may be nullptr for core buffer).
static void ibr_register(const char *account_name, const char *jid, const char *password,
                          struct t_gui_buffer *buffer)
{
    // Validate basic args
    if (!account_name || !*account_name || !jid || !*jid || !password || !*password) {
        weechat_printf(buffer,
                       _("%s%s: /account register requires <name> <jid> <password>"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return;
    }

    // Extract server domain from JID using a temporary context
    xmpp_log_t nolog = { nullptr, nullptr };
    xmpp_ctx_t *tmp_ctx = xmpp_ctx_new(nullptr, &nolog);
    if (!tmp_ctx) {
        weechat_printf(buffer,
                       _("%s%s: IBR: failed to create xmpp context"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return;
    }
    char *domain_raw = xmpp_jid_domain(tmp_ctx, jid);
    std::string server = domain_raw ? domain_raw : "";
    if (domain_raw) xmpp_free(tmp_ctx, domain_raw);
    xmpp_ctx_free(tmp_ctx);

    if (server.empty()) {
        weechat_printf(buffer,
                       _("%s%s: IBR: could not extract server domain from JID '%s'"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, jid);
        return;
    }

    // Allocate IBR state (owned by timer callback, deleted on completion).
    // The logger is a value member of ibr_state, so no separate heap allocation needed.
    auto st = std::make_unique<ibr_state>(account_name, jid, password, server, buffer);

    st->ctx = xmpp_ctx_new(nullptr, &st->logger);
    if (!st->ctx) {
        weechat_printf(buffer,
                       _("%s%s: IBR: failed to create xmpp context"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return;
    }

    st->conn = xmpp_conn_new(st->ctx);
    if (!st->conn) {
        weechat_printf(buffer,
                       _("%s%s: IBR: failed to create xmpp connection"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return;
    }

    // Trust TLS for registration (certificate may not be fully validated yet)
    int flags = xmpp_conn_get_flags(st->conn);
    flags |= XMPP_CONN_FLAG_TRUST_TLS;
    xmpp_conn_set_flags(st->conn, flags);

    // Set a dummy JID (server domain) so libstrophe knows what stream to open
    xmpp_conn_set_jid(st->conn, server.c_str());

    weechat_printf(buffer,
                   _("%s%s: IBR: connecting to %s for registration of %s..."),
                   weechat_prefix("network"), WEECHAT_XMPP_PLUGIN_NAME,
                   server.c_str(), jid);

    int rc = xmpp_connect_raw(st->conn, nullptr, 0, ibr_conn_handler, st.get());
    if (rc != XMPP_EOK) {
        weechat_printf(buffer,
                       _("%s%s: IBR: xmpp_connect_raw failed (rc=%d)"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, rc);
        return;
    }

    // Drive the event loop from a periodic WeeChat timer (10 ms ticks).
    // Transfer ownership to the timer callback — it will delete st when done.
    ibr_state *st_raw = st.release();
    st_raw->timer_hook = weechat_hook_timer(10, 0, 0, ibr_timer_cb, st_raw, nullptr);
    if (!st_raw->timer_hook) {
        weechat_printf(buffer,
                       _("%s%s: IBR: failed to register timer hook"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        delete st_raw;
        return;
    }
}

void command__account_register(struct t_gui_buffer *buffer, int argc, char **argv)
{
    if (argc < 5) {
        weechat_printf(buffer,
                       _("%s%s: usage: /account register <name> <jid> <password>"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return;
    }
    ibr_register(argv[2], argv[3], argv[4], buffer);
}

void command__add_account(const char *name, const char *jid, const char *password)
{
    weechat::account *account = nullptr;
    if (weechat::account::search(account, name, true))
    {
        weechat_printf(
            nullptr,
            _("%s%s: account \"%s\" already exists, can't add it!"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
            name);
        return;
    }

    if (!jid || !password) {
        weechat_printf(
            nullptr,
            _("%s%s: jid and password required"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return;
    }

    ;
    account = &weechat::accounts.emplace(
        std::piecewise_construct, std::forward_as_tuple(name),
        std::forward_as_tuple(weechat::config::instance->file, name)).first->second;
    if (!account)
    {
        weechat_printf(
            nullptr,
            _("%s%s: unable to add account"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return;
    }

    account->name = name;
    if (jid)
        account->jid(jid);
    if (password)
        account->password(password);
    if (jid) {
        xmpp_string_guard jid_node_g(account->context, xmpp_jid_node(account->context, jid));
        account->nickname(jid_node_g.c_str());
    }

    weechat_printf(
        nullptr,
        _("%s: account %s%s%s %s(%s%s%s)%s added"),
        WEECHAT_XMPP_PLUGIN_NAME,
        weechat_color("chat_server"),
        account->name.data(),
        weechat_color("reset"),
        weechat_color("chat_delimiters"),
        weechat_color("chat_server"),
        jid ? jid : "???",
        weechat_color("chat_delimiters"),
        weechat_color("reset"));
}

void command__account_add(struct t_gui_buffer *buffer, int argc, char **argv)
{
    char *name, *jid = nullptr, *password = nullptr;

    (void) buffer;

    switch (argc)
    {
        case 5:
            password = argv[4];
            // fall through
        case 4:
            jid = argv[3];
            // fall through
        case 3:
            name = argv[2];
            command__add_account(name, jid, password);
            break;
        default:
            weechat_printf(nullptr, _("account add: wrong number of arguments"));
            break;
    }
}

int command__connect_account(weechat::account *account)
{
    if (!account)
        return 0;

    if (account->connected())
    {
        weechat_printf(
            nullptr,
            _("%s%s: already connected to account \"%s\"!"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
            account->name.data());
    }

    account->connect(true);  // Manual connect from user command

    return 1;
}

int command__account_connect(struct t_gui_buffer *buffer, int argc, char **argv)
{
    int i, connect_ok;
    weechat::account *ptr_account = nullptr;

    (void) buffer;
    (void) argc;
    (void) argv;

    connect_ok = 1;

    for (i = 2; i < argc; i++)
    {
        if (weechat::account::search(ptr_account, argv[i]))
        {
            if (!command__connect_account(ptr_account))
            {
                connect_ok = 0;
            }
        }
        else
        {
            weechat_printf(
                nullptr,
                _("%s%s: account not found \"%s\" "
                  "(add one first with: /account add)"),
                weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                argv[i]);
        }
    }

    return (connect_ok) ? WEECHAT_RC_OK : WEECHAT_RC_ERROR;
}

int command__disconnect_account(weechat::account *account)
{
    if (!account)
        return 0;

    if (!account->connected())
    {
        weechat_printf(
            nullptr,
            _("%s%s: not connected to account \"%s\"!"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
            account->name.data());
    }

    account->disconnect(0);

    return 1;
}

int command__account_disconnect(struct t_gui_buffer *buffer, int argc, char **argv)
{
    int i, disconnect_ok;
    weechat::account *ptr_account;

    (void) argc;
    (void) argv;

    disconnect_ok = 1;

    if (argc < 2)
    {
        weechat::channel *ptr_channel;

        buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

        if (ptr_account)
        {
            if (!command__disconnect_account(ptr_account))
            {
                disconnect_ok = 0;
            }
        }
    }
    for (i = 2; i < argc; i++)
    {
        if (weechat::account::search(ptr_account, argv[i]))
        {
            if (!command__disconnect_account(ptr_account))
            {
                disconnect_ok = 0;
            }
        }
        else
        {
            weechat_printf(
                nullptr,
                _("%s%s: account not found \"%s\" "),
                weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                argv[i]);
        }
    }

    return (disconnect_ok) ? WEECHAT_RC_OK : WEECHAT_RC_ERROR;
}

int command__account_reconnect(struct t_gui_buffer *buffer, int argc, char **argv)
{
    command__account_disconnect(buffer, argc, argv);
    return command__account_connect(buffer, argc, argv);
}

// ---------------------------------------------------------------------------
// XEP-0077 §3.2: Account cancellation (/account unregister <account>)
// ---------------------------------------------------------------------------

struct ibr_unregister_context {
    weechat::account *account;
    struct t_gui_buffer *buffer;
};

static int ibr_unregister_result_handler(xmpp_conn_t * /*conn*/, xmpp_stanza_t *stanza, void *userdata)
{
    auto ctx = std::unique_ptr<ibr_unregister_context>(
        reinterpret_cast<ibr_unregister_context *>(userdata));
    if (!ctx) return 0;

    const char *type = xmpp_stanza_get_type(stanza);
    if (type && strcmp(type, "result") == 0) {
        weechat_printf(ctx->buffer,
                       _("%s%s: account cancelled on server — deleting local account"),
                       weechat_prefix("network"), WEECHAT_XMPP_PLUGIN_NAME);
        std::string name = ctx->account->name;
        ctx->account->disconnect(0);
        weechat::accounts.erase(name);
        weechat::config::write();
    } else {
        const char *condition = "unknown";
        xmpp_stanza_t *err = xmpp_stanza_get_child_by_name(stanza, "error");
        if (err) {
            static const char *const error_names[] = {
                "forbidden", "not-allowed", "service-unavailable",
                "feature-not-implemented", "bad-request", nullptr
            };
            for (int i = 0; error_names[i]; ++i) {
                xmpp_stanza_t *c = xmpp_stanza_get_child_by_name(err, error_names[i]);
                if (c) { condition = xmpp_stanza_get_name(c); break; }
            }
        }
        weechat_printf(ctx->buffer,
                       _("%s%s: IBR unregister failed: %s"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, condition);
    }

    return 1; // consume
}

void command__account_unregister(struct t_gui_buffer *buffer, int argc, char **argv)
{
    if (argc < 3) {
        weechat_printf(buffer,
                       _("%s%s: usage: /account unregister <account>"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return;
    }

    weechat::account *account = nullptr;
    if (!weechat::account::search(account, argv[2])) {
        weechat_printf(buffer,
                       _("%s%s: account \"%s\" not found"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, argv[2]);
        return;
    }

    if (!account->connected()) {
        weechat_printf(buffer,
                       _("%s%s: account \"%s\" is not connected"
                         " — connect first with /account connect %s"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                       argv[2], argv[2]);
        return;
    }

    xmpp_ctx_t *ctx = account->context;

    xmpp_stanza_t *iq = xmpp_iq_new(ctx, "set", "ibr-unregister");
    xmpp_string_guard domain_g(ctx, xmpp_jid_domain(ctx, account->jid().data()));
    if (domain_g.c_str())
        xmpp_stanza_set_to(iq, domain_g.c_str());

    xmpp_stanza_t *query = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(query, "query");
    xmpp_stanza_set_ns(query, "jabber:iq:register");

    xmpp_stanza_t *remove_el = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(remove_el, "remove");
    xmpp_stanza_add_child(query, remove_el);
    xmpp_stanza_release(remove_el);

    xmpp_stanza_add_child(iq, query);
    xmpp_stanza_release(query);

    auto uctx = std::make_unique<ibr_unregister_context>(ibr_unregister_context{ account, buffer });
    xmpp_id_handler_add(account->connection, ibr_unregister_result_handler,
                        "ibr-unregister", uctx.release());
    account->connection.send(iq);
    xmpp_stanza_release(iq);

    weechat_printf(buffer,
                   _("%s%s: IBR: sending account cancellation request..."),
                   weechat_prefix("network"), WEECHAT_XMPP_PLUGIN_NAME);
}

// ---------------------------------------------------------------------------
// XEP-0077 §3.3: In-band password change (/account password <account> <newpw>)
// ---------------------------------------------------------------------------

struct ibr_passwd_context {
    weechat::account *account;
    struct t_gui_buffer *buffer;
    std::string new_password;
};

static int ibr_passwd_result_handler(xmpp_conn_t * /*conn*/, xmpp_stanza_t *stanza, void *userdata)
{
    auto ctx = std::unique_ptr<ibr_passwd_context>(
        reinterpret_cast<ibr_passwd_context *>(userdata));
    if (!ctx) return 0;

    const char *type = xmpp_stanza_get_type(stanza);
    if (type && strcmp(type, "result") == 0) {
        ctx->account->password(ctx->new_password);
        weechat::config::write();
        weechat_printf(ctx->buffer,
                       _("%s%s: password changed successfully"),
                       weechat_prefix("network"), WEECHAT_XMPP_PLUGIN_NAME);
    } else {
        const char *condition = "unknown";
        xmpp_stanza_t *err = xmpp_stanza_get_child_by_name(stanza, "error");
        if (err) {
            static const char *const error_names[] = {
                "not-authorized", "forbidden", "not-allowed",
                "bad-request", "not-acceptable", nullptr
            };
            for (int i = 0; error_names[i]; ++i) {
                xmpp_stanza_t *c = xmpp_stanza_get_child_by_name(err, error_names[i]);
                if (c) { condition = xmpp_stanza_get_name(c); break; }
            }
        }
        weechat_printf(ctx->buffer,
                       _("%s%s: IBR password change failed: %s"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, condition);
    }

    return 1; // consume
}

void command__account_passwd(struct t_gui_buffer *buffer, int argc, char **argv)
{
    if (argc < 4) {
        weechat_printf(buffer,
                       _("%s%s: usage: /account password <account> <new-password>"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return;
    }

    weechat::account *account = nullptr;
    if (!weechat::account::search(account, argv[2])) {
        weechat_printf(buffer,
                       _("%s%s: account \"%s\" not found"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, argv[2]);
        return;
    }

    if (!account->connected()) {
        weechat_printf(buffer,
                       _("%s%s: account \"%s\" is not connected"
                         " — connect first with /account connect %s"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                       argv[2], argv[2]);
        return;
    }

    if (!xmpp_conn_is_secured(account->connection)) {
        weechat_printf(buffer,
                       _("%s%s: password change requires an encrypted (TLS) connection"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return;
    }

    xmpp_ctx_t *ctx = account->context;
    xmpp_string_guard node_g(ctx, xmpp_jid_node(ctx, account->jid().data()));
    xmpp_string_guard domain_g(ctx, xmpp_jid_domain(ctx, account->jid().data()));

    xmpp_stanza_t *iq = xmpp_iq_new(ctx, "set", "ibr-passwd");
    if (domain_g.c_str())
        xmpp_stanza_set_to(iq, domain_g.c_str());

    xmpp_stanza_t *query = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(query, "query");
    xmpp_stanza_set_ns(query, "jabber:iq:register");

    // <username>
    xmpp_stanza_t *username_el = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(username_el, "username");
    xmpp_stanza_t *username_txt = xmpp_stanza_new(ctx);
    xmpp_stanza_set_text(username_txt, node_g.c_str() ? node_g.c_str() : account->jid().data());
    xmpp_stanza_add_child(username_el, username_txt);
    xmpp_stanza_release(username_txt);
    xmpp_stanza_add_child(query, username_el);
    xmpp_stanza_release(username_el);

    // <password>
    xmpp_stanza_t *password_el = xmpp_stanza_new(ctx);
    xmpp_stanza_set_name(password_el, "password");
    xmpp_stanza_t *password_txt = xmpp_stanza_new(ctx);
    xmpp_stanza_set_text(password_txt, argv[3]);
    xmpp_stanza_add_child(password_el, password_txt);
    xmpp_stanza_release(password_txt);
    xmpp_stanza_add_child(query, password_el);
    xmpp_stanza_release(password_el);

    xmpp_stanza_add_child(iq, query);
    xmpp_stanza_release(query);

    auto pctx = std::make_unique<ibr_passwd_context>(ibr_passwd_context{ account, buffer, std::string(argv[3]) });
    xmpp_id_handler_add(account->connection, ibr_passwd_result_handler,
                        "ibr-passwd", pctx.release());
    account->connection.send(iq);
    xmpp_stanza_release(iq);

    weechat_printf(buffer,
                   _("%s%s: IBR: sending password change request..."),
                   weechat_prefix("network"), WEECHAT_XMPP_PLUGIN_NAME);
}

void command__account_delete(struct t_gui_buffer *buffer, int argc, char **argv)
{
    (void) buffer;

    if (argc < 3)
    {
        weechat_printf(
            nullptr,
            _("%sToo few arguments for command\"%s %s\" "
              "(help on command: /help %s)"),
            weechat_prefix("error"),
            argv[0], argv[1], argv[0] + 1);
        return;
    }

    weechat::account *account = nullptr;

    if (!weechat::account::search(account, argv[2]))
    {
        weechat_printf(
            nullptr,
            _("%s%s: account \"%s\" not found for \"%s\" command"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
            argv[2], "xmpp delete");
        return;
    }
    if (account->connected())
    {
        weechat_printf(
            nullptr,
            _("%s%s: you cannot delete account \"%s\" because you"
              "are connected. Try \"/xmpp disconnect %s\" first."),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
            argv[2], argv[2]);
        return;
    }

    std::string account_name = account->name;
    weechat::accounts.erase(account->name);
    weechat_printf(
        nullptr,
        _("%s: account %s%s%s has been deleted"),
        WEECHAT_XMPP_PLUGIN_NAME,
        weechat_color("chat_server"),
        !account_name.empty() ? account_name.data() : "???",
        weechat_color("reset"));
}

int command__account(const void *pointer, void *data,
                     struct t_gui_buffer *buffer, int argc,
                     char **argv, char **argv_eol)
{

    (void) pointer;
    (void) data;
    (void) buffer;

    if (argc <= 1 || weechat_strcasecmp(argv[1], "list") == 0)
    {
        command__account_list(argc, argv);
        return WEECHAT_RC_OK;
    }

    if (argc > 1)
    {
        if (weechat_strcasecmp(argv[1], "add") == 0)
        {
            command__account_add(buffer, argc, argv);
            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "register") == 0)
        {
            command__account_register(buffer, argc, argv);
            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "unregister") == 0)
        {
            command__account_unregister(buffer, argc, argv);
            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "password") == 0)
        {
            command__account_passwd(buffer, argc, argv);
            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "connect") == 0)
        {
            command__account_connect(buffer, argc, argv);
            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "disconnect") == 0)
        {
            command__account_disconnect(buffer, argc, argv);
            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "reconnect") == 0)
        {
            command__account_reconnect(buffer, argc, argv);
            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "delete") == 0)
        {
            command__account_delete(buffer, argc, argv);
            return WEECHAT_RC_OK;
        }

        WEECHAT_COMMAND_ERROR;
    }

    return WEECHAT_RC_OK;
}

