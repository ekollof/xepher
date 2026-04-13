// ---------------------------------------------------------------------------
// Password prompt infrastructure
// ---------------------------------------------------------------------------
// Supports two actions: adding a new account or connecting an existing one.
// Only one prompt can be active at a time (global singleton pointer).

// Forward declarations needed by prompt callbacks
void command__add_account(const char *name, const char *jid, const char *password);
int  command__connect_account(weechat::account *account);

enum class prompt_action { add, connect };

struct password_prompt_ctx {
    prompt_action        action;
    std::string          account_name;
    std::string          jid;            // only used for action::add
    struct t_gui_buffer *buffer;
    struct t_hook       *modifier_hook;  // masks displayed input with *
    struct t_hook       *input_run_hook; // captures Enter
};

static password_prompt_ctx *g_prompt = nullptr;

// Modifier callback: replaces every visible character in the input bar with '*'
static char *prompt_input_mask_cb(const void * /*pointer*/, void * /*data*/,
                                  const char *modifier, const char *modifier_data,
                                  const char *string)
{
    (void) modifier;
    (void) modifier_data;

    if (!g_prompt || !string) return nullptr;

    // Count UTF-8 characters (each char in the WeeChat string view may be
    // multi-byte). We mask character by character preserving cursor position
    // marker (which WeeChat appends as a special invisible char at the end).
    // The simplest correct approach: count display chars, return that many '*'.
    int display_len = weechat_strlen_screen(string);
    if (display_len < 0) display_len = static_cast<int>(strlen(string));

    std::string masked(static_cast<size_t>(display_len), '*');
    return strdup(masked.c_str());
}

// command_run callback: fires when the user presses Enter in the input bar
static int prompt_input_return_cb(const void * /*pointer*/, void * /*data*/,
                                  struct t_gui_buffer *buffer,
                                  const char * /*command*/)
{
    if (!g_prompt) return WEECHAT_RC_OK;
    if (buffer != g_prompt->buffer) return WEECHAT_RC_OK;

    // Capture the raw input text before WeeChat clears it
    const char *raw = weechat_buffer_get_string(buffer, "input");
    std::string password = raw ? raw : "";

    // Unhook both hooks first so we don't re-enter
    if (g_prompt->modifier_hook)  { weechat_unhook(g_prompt->modifier_hook);  g_prompt->modifier_hook  = nullptr; }
    if (g_prompt->input_run_hook) { weechat_unhook(g_prompt->input_run_hook); g_prompt->input_run_hook = nullptr; }

    // Clear the input bar (don't let the password be echoed on Enter)
    weechat_buffer_set(buffer, "input", "");

    // Take ownership of the prompt context
    auto ctx = std::unique_ptr<password_prompt_ctx>(g_prompt);
    g_prompt = nullptr;

    if (password.empty()) {
        weechat_printf(ctx->buffer,
                       _("%s%s: password prompt cancelled (empty input)"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK_EAT; // eat the Enter so nothing is sent
    }

    if (ctx->action == prompt_action::add) {
        // Delegate to the same function used when password is given on cmdline
        command__add_account(ctx->account_name.c_str(),
                             ctx->jid.c_str(),
                             password.c_str());
    } else {
        // prompt_action::connect — find account and connect
        weechat::account *account = nullptr;
        if (weechat::account::search(account, ctx->account_name.c_str())) {
            // Store the password in secure storage, set the reference, then connect
            std::string sec_key = fmt::format("xmpp_{}", ctx->account_name);
            std::string sec_cmd = fmt::format("/secure set {} {}", sec_key, password);
            weechat_command(nullptr, sec_cmd.c_str());
            std::string sec_ref = fmt::format("${{sec.data.{}}}", sec_key);
            account->password(sec_ref);
            command__connect_account(account);
        } else {
            weechat_printf(ctx->buffer,
                           _("%s%s: account \"%s\" not found"),
                           weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                           ctx->account_name.c_str());
        }
    }

    return WEECHAT_RC_OK_EAT; // eat the Enter keypress
}

// Start an interactive password prompt on the given buffer.
// The prompt context is heap-allocated and owned by the hooks until Enter is pressed.
static void prompt_password(struct t_gui_buffer *buffer,
                            prompt_action action,
                            std::string account_name,
                            std::string jid = {})
{
    if (g_prompt) {
        weechat_printf(buffer,
                       _("%s%s: a password prompt is already active"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return;
    }

    auto ctx = std::make_unique<password_prompt_ctx>();
    ctx->action       = action;
    ctx->account_name = std::move(account_name);
    ctx->jid          = std::move(jid);
    ctx->buffer       = buffer;

    ctx->modifier_hook = weechat_hook_modifier(
        "input_text_display_with_cursor",
        prompt_input_mask_cb, nullptr, nullptr);

    ctx->input_run_hook = weechat_hook_command_run(
        "/input return",
        prompt_input_return_cb, nullptr, nullptr);

    g_prompt = ctx.release();

    weechat_printf(buffer,
                   _("%s%s: enter password (hidden): "),
                   weechat_prefix("network"), WEECHAT_XMPP_PLUGIN_NAME);
    // Switch focus to the buffer so the user types there
    weechat_buffer_set(buffer, "display", "1");
    // Clear any leftover input
    weechat_buffer_set(buffer, "input", "");
}

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
    if (type && std::string_view(type) == "result") {
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
    if (!type || std::string_view(type) != "result") {
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
            xmpp_string_guard title_g(st->ctx, xmpp_stanza_get_text(title_el));
            if (title_g)
                weechat_printf(st->buffer, _("%s%s: IBR form title: %s"),
                               weechat_prefix("network"), WEECHAT_XMPP_PLUGIN_NAME,
                               title_g.c_str());
        }
        xmpp_stanza_t *instr_el = xmpp_stanza_get_child_by_name(xdata, "instructions");
        if (instr_el) {
            xmpp_string_guard instr_g(st->ctx, xmpp_stanza_get_text(instr_el));
            if (instr_g)
                weechat_printf(st->buffer, _("%s%s: IBR form instructions: %s"),
                               weechat_prefix("network"), WEECHAT_XMPP_PLUGIN_NAME,
                               instr_g.c_str());
        }

        // Print each field's var and label so the user can identify what's needed
        for (xmpp_stanza_t *field = xmpp_stanza_get_children(xdata);
             field; field = xmpp_stanza_get_next(field)) {
            if (!xmpp_stanza_get_name(field) ||
                std::string_view(xmpp_stanza_get_name(field)) != "field") continue;
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
                if (std::string_view(cname) == known_fields[i]) { is_known = true; break; }
            }
            // Skip housekeeping elements
            if (std::string_view sv{cname};
                sv == "registered" || sv == "instructions" || sv == "x") continue;
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
        xmpp_string_guard url_g(st->ctx, url_el ? xmpp_stanza_get_text(url_el) : nullptr);
        weechat_printf(st->buffer,
                       _("%s%s: IBR: server requires out-of-band registration: %s"),
                       weechat_prefix("network"), WEECHAT_XMPP_PLUGIN_NAME,
                       url_g ? url_g.c_str() : "(no URL provided)");
        xmpp_disconnect(conn);
        st->done = true;
        return 1;
    }

    // Build the <iq type='set'> registration request
    xmpp_ctx_t *ctx = st->ctx;
    std::string username = jid(nullptr, st->jid).local;
    if (username.empty()) username = st->jid;

    struct ibr_set_spec : stanza::spec {
        ibr_set_spec(const std::string &uname, const std::string &pass) : spec("iq") {
            attr("type", "set");
            attr("id", "ibr-set");
            struct query_spec : stanza::spec {
                query_spec(const std::string &u, const std::string &p) : spec("query") {
                    attr("xmlns", "jabber:iq:register");
                    struct text_el : stanza::spec {
                        text_el(std::string_view tag, std::string_view val) : spec(tag) {
                            text(val);
                        }
                    };
                    text_el un("username", u);
                    text_el pw("password", p);
                    child(un);
                    child(pw);
                }
            } q(uname, pass);
            child(q);
        }
    } ibr_iq(username, st->password);
    auto built = ibr_iq.build(ctx);
    xmpp_stanza_set_to(built.get(), st->server.c_str());

    // Register result handler for the set IQ before sending
    xmpp_id_handler_add(conn, ibr_set_result_handler, "ibr-set", st);

    xmpp_send(conn, built.get());

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
        struct ibr_get_spec : stanza::spec {
            ibr_get_spec() : spec("iq") {
                attr("type", "get");
                attr("id", "ibr-get");
                struct query_spec : stanza::spec {
                    query_spec() : spec("query") { attr("xmlns", "jabber:iq:register"); }
                } q;
                child(q);
            }
        } ibr_get_iq;
        auto built = ibr_get_iq.build(ctx);
        xmpp_stanza_set_to(built.get(), st->server.c_str());
        xmpp_id_handler_add(conn, ibr_get_result_handler, "ibr-get", st);
        xmpp_send(conn, built.get());
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
    std::string server = ::jid(nullptr, jid).domain;
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
    st->timer_hook = weechat_hook_timer(10, 0, 0, ibr_timer_cb, st.get(), nullptr);
    if (!st->timer_hook) {
        weechat_printf(buffer,
                       _("%s%s: IBR: failed to register timer hook"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return; // st destroyed by unique_ptr on scope exit
    }
    st.release(); // ownership transferred to timer callback
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
    if (password) {
        // Store the password in WeeChat secure storage and save a reference.
        std::string sec_key = fmt::format("xmpp_{}", name);
        std::string sec_cmd = fmt::format("/secure set {} {}", sec_key, password);
        weechat_command(nullptr, sec_cmd.c_str());
        std::string sec_ref = fmt::format("${{sec.data.{}}}", sec_key);
        account->password(sec_ref);
    }
    if (jid)
        account->nickname(::jid(nullptr, jid).local);

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
    switch (argc)
    {
        case 5:
            // name jid password — all provided, store directly
            command__add_account(argv[2], argv[3], argv[4]);
            break;
        case 4:
            // name jid — no password, prompt for it
            prompt_password(buffer, prompt_action::add,
                            std::string(argv[2]), std::string(argv[3]));
            break;
        case 3:
            // name only — still need jid+password
            command__add_account(argv[2], nullptr, nullptr);
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

    connect_ok = 1;

    for (i = 2; i < argc; i++)
    {
        if (weechat::account::search(ptr_account, argv[i]))
        {
            // Check whether a password is configured (raw value may be a
            // ${sec.data.*} reference — treat it as present if non-empty).
            std::string_view pw = ptr_account->password();
            if (pw.empty()) {
                // No password stored — prompt interactively
                prompt_password(buffer, prompt_action::connect,
                                std::string(ptr_account->name));
                // connect will happen inside the prompt callback; skip here
            } else {
                if (!command__connect_account(ptr_account))
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
    if (type && std::string_view(type) == "result") {
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
    std::string domain = ::jid(nullptr, account->jid()).domain;

    struct ibr_unregister_spec : stanza::spec {
        ibr_unregister_spec() : spec("iq") {
            attr("type", "set");
            attr("id", "ibr-unregister");
            struct query_spec : stanza::spec {
                query_spec() : spec("query") {
                    attr("xmlns", "jabber:iq:register");
                    struct remove_spec : stanza::spec {
                        remove_spec() : spec("remove") {}
                    } rm;
                    child(rm);
                }
            } q;
            child(q);
        }
    } unreg_iq;
    auto built = unreg_iq.build(ctx);
    if (!domain.empty())
        xmpp_stanza_set_to(built.get(), domain.c_str());

    auto uctx = std::make_unique<ibr_unregister_context>(ibr_unregister_context{ account, buffer });
    xmpp_id_handler_add(account->connection, ibr_unregister_result_handler,
                        "ibr-unregister", uctx.release());
    account->connection.send(built.get());

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
    if (type && std::string_view(type) == "result") {
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
    ::jid acct_jid(nullptr, account->jid());
    std::string uname = acct_jid.local.empty() ? account->jid() : acct_jid.local;

    struct ibr_passwd_spec : stanza::spec {
        ibr_passwd_spec(const std::string &uname, const char *pass) : spec("iq") {
            attr("type", "set");
            attr("id", "ibr-passwd");
            struct query_spec : stanza::spec {
                query_spec(const std::string &u, const char *p) : spec("query") {
                    attr("xmlns", "jabber:iq:register");
                    struct text_el : stanza::spec {
                        text_el(std::string_view tag, std::string_view val) : spec(tag) {
                            text(val);
                        }
                    };
                    text_el un("username", u);
                    text_el pw("password", p ? p : "");
                    child(un);
                    child(pw);
                }
            } q(uname, pass);
            child(q);
        }
    } passwd_iq(uname, argv[3]);
    auto built = passwd_iq.build(ctx);
    if (!acct_jid.domain.empty())
        xmpp_stanza_set_to(built.get(), acct_jid.domain.c_str());

    auto pctx = std::make_unique<ibr_passwd_context>(ibr_passwd_context{ account, buffer, std::string(argv[3]) });
    xmpp_id_handler_add(account->connection, ibr_passwd_result_handler,
                        "ibr-passwd", pctx.release());
    account->connection.send(built.get());

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
    // Remove secure storage entry if present
    {
        std::string sec_key = fmt::format("xmpp_{}", account_name);
        std::string sec_cmd = fmt::format("/secure del {}", sec_key);
        weechat_command(nullptr, sec_cmd.c_str());
    }
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

