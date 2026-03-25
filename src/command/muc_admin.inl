// XEP-0224: Attention — send <attention> to get a contact's attention
int command__buzz(const void *pointer, void *data,
                  struct t_gui_buffer *buffer, int argc,
                  char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;
    (void) argc;
    (void) argv;
    (void) argv_eol;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_channel)
    {
        weechat_printf(buffer, "%sxmpp: you must be in a channel to buzz someone",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        weechat_printf(buffer, "%sxmpp: you are not connected to server",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    if (ptr_channel->type == weechat::channel::chat_type::MUC)
    {
        weechat_printf(buffer, "%sxmpp: /buzz can only be used in PM channels",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    // Send <message> with <attention> element (XEP-0224)
    xmpp_stanza_t *message = xmpp_message_new(ptr_account->context, "chat",
                                               ptr_channel->id.data(), NULL);

    xmpp_string_guard id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
    const char *id = id_g.ptr;
    xmpp_stanza_set_id(message, id);
    // freed by id_g

    xmpp_stanza_t *attention = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(attention, "attention");
    xmpp_stanza_set_ns(attention, "urn:xmpp:attention:0");
    xmpp_stanza_add_child(message, attention);
    xmpp_stanza_release(attention);

    ptr_account->connection.send(message);
    xmpp_stanza_release(message);

    weechat_printf(buffer, "%sxmpp: buzz sent to %s",
                  weechat_prefix("network"), ptr_channel->id.data());

    return WEECHAT_RC_OK;
}

// XEP-0382: Spoiler Messages — send a message with a spoiler warning
int command__spoiler(const void *pointer, void *data,
                     struct t_gui_buffer *buffer, int argc,
                     char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;
    (void) argv;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_channel)
    {
        weechat_printf(buffer, "%sxmpp: you must be in a channel to send spoiler messages",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        weechat_printf(buffer, "%sxmpp: you are not connected to server",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    // Usage: /spoiler [hint:] <message>
    // If first arg ends with ':', treat it as a hint
    if (argc < 2)
    {
        weechat_printf(buffer, "%sxmpp: missing message text",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    const char *text = argv_eol[1];

    // Check if "hint: message" form is used
    if (argc >= 3 && argv[1][strlen(argv[1])-1] == ':')
    {
        const char *hint = argv[1];
        // Strip trailing colon
        std::string hint_str(hint, strlen(hint)-1);
        text = argv_eol[2];

        xmpp_stanza_t *message = xmpp_message_new(ptr_account->context,
                        ptr_channel->type == weechat::channel::chat_type::MUC
                        ? "groupchat" : "chat",
                        ptr_channel->id.data(), NULL);

        xmpp_string_guard id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
        const char *id = id_g.ptr;
        xmpp_stanza_set_id(message, id);
        // freed by id_g

        xmpp_message_set_body(message, text);

        xmpp_stanza_t *spoiler = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(spoiler, "spoiler");
        xmpp_stanza_set_ns(spoiler, "urn:xmpp:spoiler:0");
        // Set hint text as text node child
        xmpp_stanza_t *hint_node = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_text(hint_node, hint_str.c_str());
        xmpp_stanza_add_child(spoiler, hint_node);
        xmpp_stanza_release(hint_node);
        xmpp_stanza_add_child(message, spoiler);
        xmpp_stanza_release(spoiler);

        // Add store hint
        xmpp_stanza_t *store = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(store, "store");
        xmpp_stanza_set_ns(store, "urn:xmpp:hints");
        xmpp_stanza_add_child(message, store);
        xmpp_stanza_release(store);

        ptr_account->connection.send(message);
        xmpp_stanza_release(message);
    }
    else
    {
        xmpp_stanza_t *message = xmpp_message_new(ptr_account->context,
                        ptr_channel->type == weechat::channel::chat_type::MUC
                        ? "groupchat" : "chat",
                        ptr_channel->id.data(), NULL);

        xmpp_string_guard id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
        const char *id = id_g.ptr;
        xmpp_stanza_set_id(message, id);
        // freed by id_g

        xmpp_message_set_body(message, text);

        // <spoiler/> with no hint
        xmpp_stanza_t *spoiler = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(spoiler, "spoiler");
        xmpp_stanza_set_ns(spoiler, "urn:xmpp:spoiler:0");
        xmpp_stanza_add_child(message, spoiler);
        xmpp_stanza_release(spoiler);

        // Add store hint
        xmpp_stanza_t *store = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(store, "store");
        xmpp_stanza_set_ns(store, "urn:xmpp:hints");
        xmpp_stanza_add_child(message, store);
        xmpp_stanza_release(store);

        ptr_account->connection.send(message);
        xmpp_stanza_release(message);
    }

    weechat_printf(buffer, "%sxmpp: spoiler message sent",
                  weechat_prefix("network"));

    return WEECHAT_RC_OK;
}

// XEP-0050: Ad-Hoc Commands — list and execute server/component commands
// Usage:
//   /adhoc <jid>                         — list available commands
//   /adhoc <jid> <node>                  — execute a command
//   /adhoc <jid> <node> <sessionid> [field=value ...]  — submit a form step
int command__adhoc(const void *pointer, void *data,
                   struct t_gui_buffer *buffer, int argc,
                   char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;
    (void) argv_eol;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_account->connected())
    {
        weechat_printf(buffer, "%sxmpp: you are not connected to server",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    if (argc < 2)
    {
        weechat_printf(buffer, "%sxmpp: /adhoc <jid> [<node> [<sessionid> [field=value ...]]]",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    const char *target_jid = argv[1];

    if (argc == 2)
    {
        // List available commands via disco#items with commands node.
        // Open an interactive picker so the user can choose a command to run.
        using picker_t = weechat::ui::picker<std::string>;
        weechat::account *acct = ptr_account;
        std::string tjid_str = target_jid;

        auto p_holder = std::make_shared<picker_t *>(nullptr);
        auto *p = new picker_t(
            "xmpp.picker.adhoc",
            fmt::format("Ad-hoc commands on {}  (XEP-0050)  — select to execute", target_jid),
            {},   // populated async as disco#items result arrives
            [acct, tjid_str](const std::string &node_uri) {
                // on_select: run /adhoc <jid> <node>
                std::string cmd = fmt::format("/adhoc {} {}", tjid_str, node_uri);
                weechat_command(acct->buffer, cmd.c_str());
            },
            [acct, p_holder]() {
                // on_close: null out any pending adhoc_queries that reference this picker.
                picker_t *raw = *p_holder;
                for (auto &[id, info] : acct->adhoc_queries)
                    if (info.picker == raw) info.picker = nullptr;
            },
            buffer);
        *p_holder = p;
        (void) p;

        xmpp_string_guard query_id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
        const char *query_id = query_id_g.ptr;

        weechat::account::adhoc_query_info info;
        info.target_jid = target_jid;
        info.buffer = buffer;
        info.is_list = true;
        info.picker = *p_holder;
        ptr_account->adhoc_queries[query_id] = info;

        xmpp_stanza_t *iq = xmpp_iq_new(ptr_account->context, "get", query_id);
        xmpp_stanza_set_to(iq, target_jid);
        xmpp_stanza_t *query_stanza = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(query_stanza, "query");
        xmpp_stanza_set_ns(query_stanza, "http://jabber.org/protocol/disco#items");
        xmpp_stanza_set_attribute(query_stanza, "node",
                                  "http://jabber.org/protocol/commands");
        xmpp_stanza_add_child(iq, query_stanza);
        xmpp_stanza_release(query_stanza);
        ptr_account->connection.send(iq);
        xmpp_stanza_release(iq);
        // freed by query_id_g

        return WEECHAT_RC_OK;
    }

    const char *node = argv[2];

    if (argc == 3)
    {
        // Execute a command (first step)
        xmpp_string_guard exec_id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
        const char *exec_id = exec_id_g.ptr;

        weechat::account::adhoc_query_info info;
        info.target_jid = target_jid;
        info.buffer = buffer;
        info.is_list = false;
        info.node = node;
        ptr_account->adhoc_queries[exec_id] = info;

        xmpp_stanza_t *iq = xmpp_iq_new(ptr_account->context, "set", exec_id);
        xmpp_stanza_set_to(iq, target_jid);
        xmpp_stanza_t *command = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(command, "command");
        xmpp_stanza_set_ns(command, "http://jabber.org/protocol/commands");
        xmpp_stanza_set_attribute(command, "node", node);
        xmpp_stanza_set_attribute(command, "action", "execute");
        xmpp_stanza_add_child(iq, command);
        xmpp_stanza_release(command);
        ptr_account->connection.send(iq);
        xmpp_stanza_release(iq);
        // freed by exec_id_g

        weechat_printf(buffer, "%sxmpp: executing command %s on %s…",
                      weechat_prefix("network"), node, target_jid);
        return WEECHAT_RC_OK;
    }

    // argc >= 4: submit a form step
    // argv[3] = sessionid, argv[4..] = field=value pairs
    const char *session_id = argv[3];
    xmpp_string_guard submit_id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
    const char *submit_id = submit_id_g.ptr;

    weechat::account::adhoc_query_info info;
    info.target_jid = target_jid;
    info.buffer = buffer;
    info.is_list = false;
    info.node = node;
    info.session_id = session_id;
    ptr_account->adhoc_queries[submit_id] = info;

    xmpp_stanza_t *iq = xmpp_iq_new(ptr_account->context, "set", submit_id);
    xmpp_stanza_set_to(iq, target_jid);
    xmpp_stanza_t *command = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(command, "command");
    xmpp_stanza_set_ns(command, "http://jabber.org/protocol/commands");
    xmpp_stanza_set_attribute(command, "node", node);
    xmpp_stanza_set_attribute(command, "sessionid", session_id);
    xmpp_stanza_set_attribute(command, "action", "execute");

    // Build x data form with provided field=value pairs
    xmpp_stanza_t *x_form = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(x_form, "x");
    xmpp_stanza_set_ns(x_form, "jabber:x:data");
    xmpp_stanza_set_attribute(x_form, "type", "submit");

    for (int i = 4; i < argc; i++)
    {
        // Parse "field=value" pairs
        const char *eq = strchr(argv[i], '=');
        if (!eq) continue;
        std::string field_var(argv[i], eq - argv[i]);
        const char *field_val = eq + 1;

        xmpp_stanza_t *field = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(field, "field");
        xmpp_stanza_set_attribute(field, "var", field_var.c_str());
        xmpp_stanza_t *value = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(value, "value");
        xmpp_stanza_t *value_text = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_text(value_text, field_val);
        xmpp_stanza_add_child(value, value_text);
        xmpp_stanza_release(value_text);
        xmpp_stanza_add_child(field, value);
        xmpp_stanza_release(value);
        xmpp_stanza_add_child(x_form, field);
        xmpp_stanza_release(field);
    }

    xmpp_stanza_add_child(command, x_form);
    xmpp_stanza_release(x_form);
    xmpp_stanza_add_child(iq, command);
    xmpp_stanza_release(command);
    ptr_account->connection.send(iq);
    xmpp_stanza_release(iq);
    // freed by submit_id_g

    weechat_printf(buffer, "%sxmpp: submitting form for command %s (session %s)…",
                  weechat_prefix("network"), node, session_id);
    return WEECHAT_RC_OK;
}

/* -----------------------------------------------------------------------
 * XEP-0045 MUC management commands: /kick, /ban, /topic, /nick
 * ----------------------------------------------------------------------- */

int command__kick(const void *pointer, void *data,
                  struct t_gui_buffer *buffer, int argc,
                  char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_channel || ptr_channel->type != weechat::channel::chat_type::MUC)
    {
        weechat_printf(buffer,
                        _("%s%s: \"%s\" command can only be executed in a MUC buffer"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, "kick");
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        weechat_printf(buffer,
                        _("%s%s: you are not connected to server"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    if (argc < 2)
    {
        weechat_printf(buffer,
                        _("%s%s: missing argument for \"%s\" command"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, "kick");
        return WEECHAT_RC_OK;
    }

    const char *nick = argv[1];
    const char *reason = argc > 2 ? argv_eol[2] : NULL;

    /* IQ set to room: <query xmlns='…muc#admin'><item nick='NICK' role='none'/></query> */
    xmpp_string_guard kick_id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
    xmpp_stanza_t *iq = xmpp_iq_new(ptr_account->context, "set", kick_id_g.c_str());
    xmpp_stanza_set_to(iq, ptr_channel->id.data());

    xmpp_stanza_t *query = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(query, "query");
    xmpp_stanza_set_ns(query, "http://jabber.org/protocol/muc#admin");

    xmpp_stanza_t *item = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(item, "item");
    xmpp_stanza_set_attribute(item, "nick", nick);
    xmpp_stanza_set_attribute(item, "role", "none");

    if (reason)
    {
        xmpp_stanza_t *reason_elem = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(reason_elem, "reason");
        xmpp_stanza_t *reason_text = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_text(reason_text, reason);
        xmpp_stanza_add_child(reason_elem, reason_text);
        xmpp_stanza_release(reason_text);
        xmpp_stanza_add_child(item, reason_elem);
        xmpp_stanza_release(reason_elem);
    }

    xmpp_stanza_add_child(query, item);
    xmpp_stanza_release(item);
    xmpp_stanza_add_child(iq, query);
    xmpp_stanza_release(query);

    ptr_account->connection.send(iq);
    xmpp_stanza_release(iq);

    weechat_printf(buffer, _("%sKicked %s from %s%s%s"),
                   weechat_prefix("network"), nick,
                   ptr_channel->id.data(),
                   reason ? ": " : "",
                   reason ? reason : "");

    return WEECHAT_RC_OK;
}

int command__ban(const void *pointer, void *data,
                 struct t_gui_buffer *buffer, int argc,
                 char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_channel || ptr_channel->type != weechat::channel::chat_type::MUC)
    {
        weechat_printf(buffer,
                        _("%s%s: \"%s\" command can only be executed in a MUC buffer"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, "ban");
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        weechat_printf(buffer,
                        _("%s%s: you are not connected to server"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    if (argc < 2)
    {
        weechat_printf(buffer,
                        _("%s%s: missing argument for \"%s\" command"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, "ban");
        return WEECHAT_RC_OK;
    }

    const char *target_jid = argv[1];
    const char *reason = argc > 2 ? argv_eol[2] : NULL;

    /* IQ set to room: <query xmlns='…muc#admin'><item jid='JID' affiliation='outcast'/></query> */
    xmpp_string_guard ban_id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
    xmpp_stanza_t *iq = xmpp_iq_new(ptr_account->context, "set", ban_id_g.c_str());
    xmpp_stanza_set_to(iq, ptr_channel->id.data());

    xmpp_stanza_t *query = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(query, "query");
    xmpp_stanza_set_ns(query, "http://jabber.org/protocol/muc#admin");

    xmpp_stanza_t *item = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(item, "item");
    xmpp_stanza_set_attribute(item, "jid", target_jid);
    xmpp_stanza_set_attribute(item, "affiliation", "outcast");

    if (reason)
    {
        xmpp_stanza_t *reason_elem = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(reason_elem, "reason");
        xmpp_stanza_t *reason_text = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_text(reason_text, reason);
        xmpp_stanza_add_child(reason_elem, reason_text);
        xmpp_stanza_release(reason_text);
        xmpp_stanza_add_child(item, reason_elem);
        xmpp_stanza_release(reason_elem);
    }

    xmpp_stanza_add_child(query, item);
    xmpp_stanza_release(item);
    xmpp_stanza_add_child(iq, query);
    xmpp_stanza_release(query);

    ptr_account->connection.send(iq);
    xmpp_stanza_release(iq);

    weechat_printf(buffer, _("%sBanned %s from %s%s%s"),
                   weechat_prefix("network"), target_jid,
                   ptr_channel->id.data(),
                   reason ? ": " : "",
                   reason ? reason : "");

    return WEECHAT_RC_OK;
}

int command__topic(const void *pointer, void *data,
                   struct t_gui_buffer *buffer, int argc,
                   char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;
    (void) argv;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_channel || ptr_channel->type != weechat::channel::chat_type::MUC)
    {
        weechat_printf(buffer,
                        _("%s%s: \"%s\" command can only be executed in a MUC buffer"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, "topic");
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        weechat_printf(buffer,
                        _("%s%s: you are not connected to server"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    /* <message type='groupchat' to='room'><subject>TEXT</subject></message>
     * An empty <subject/> clears the topic. */
    const char *new_topic = argc > 1 ? argv_eol[1] : "";

    xmpp_stanza_t *msg = xmpp_message_new(ptr_account->context,
                                           "groupchat", ptr_channel->id.data(), NULL);

    xmpp_stanza_t *subject = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(subject, "subject");

    if (argc > 1)
    {
        xmpp_stanza_t *subject_text = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_text(subject_text, new_topic);
        xmpp_stanza_add_child(subject, subject_text);
        xmpp_stanza_release(subject_text);
    }

    xmpp_stanza_add_child(msg, subject);
    xmpp_stanza_release(subject);

    ptr_account->connection.send(msg);
    xmpp_stanza_release(msg);

    return WEECHAT_RC_OK;
}

int command__muc_nick(const void *pointer, void *data,
                      struct t_gui_buffer *buffer, int argc,
                      char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;
    (void) argv_eol;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_channel || ptr_channel->type != weechat::channel::chat_type::MUC)
    {
        weechat_printf(buffer,
                        _("%s%s: \"%s\" command can only be executed in a MUC buffer"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, "nick");
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        weechat_printf(buffer,
                        _("%s%s: you are not connected to server"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    if (argc < 2)
    {
        /* Print current nick */
        const char *current_nick = weechat_buffer_get_string(buffer, "localvar_nick");
        weechat_printf(buffer, _("%sCurrent nick in %s: %s"),
                        weechat_prefix("network"),
                        ptr_channel->id.data(),
                        current_nick ? current_nick : "(unknown)");
        return WEECHAT_RC_OK;
    }

    const char *new_nick = argv[1];

    /* Send presence to room@muc/newnick — the server will respond with
     * a presence from room@muc/newnick that updates our local nick. */
    const char *new_full_jid = xmpp_jid_new(
        ptr_account->context,
        xmpp_jid_node(ptr_account->context, ptr_channel->id.data()),
        xmpp_jid_domain(ptr_account->context, ptr_channel->id.data()),
        new_nick);

    xmpp_stanza_t *pres = xmpp_presence_new(ptr_account->context);
    xmpp_stanza_set_from(pres, ptr_account->jid().data());
    xmpp_stanza_set_to(pres, new_full_jid);

    ptr_account->connection.send(pres);
    xmpp_stanza_release(pres);

    return WEECHAT_RC_OK;
}

int command__feed(const void *pointer, void *data,
                  struct t_gui_buffer *buffer, int argc,
                  char **argv, char **argv_eol)
{
    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;

    (void) pointer;
    (void) data;
    (void) argv_eol;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_account->connected())
    {
        weechat_printf(buffer,
                       _("%s%s: you are not connected to server"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    if (argc < 2)
    {
        weechat_printf(buffer,
                       _("%s%s: usage:\n"
                         "  /feed <service-jid> [--all | <node>] [--limit N] [--before <id>]\n"
                         "  /feed post <service-jid> <node> <text>      — publish a post (XEP-0472)\n"
                         "  /feed reply <service-jid> <node> <item-id> <text>  — reply to a post\n"
                         "  /feed retract <service-jid> <node> <item-id>       — retract a post"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    // XEP-0472 write path: post / reply / retract sub-commands
    std::string_view subcmd(argv[1]);

    // ── /feed post <service> <node> <text> ──────────────────────────────────
    if (subcmd == "post" || subcmd == "reply" || subcmd == "retract")
    {
        if (argc < (subcmd == "reply" ? 6 : (subcmd == "retract" ? 5 : 5)))
        {
            if (subcmd == "post")
                weechat_printf(buffer,
                               _("%s%s: usage: /feed post <service-jid> <node> <text>"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
            else if (subcmd == "reply")
                weechat_printf(buffer,
                               _("%s%s: usage: /feed reply <service-jid> <node> <item-id> <text>"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
            else
                weechat_printf(buffer,
                               _("%s%s: usage: /feed retract <service-jid> <node> <item-id>"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
            return WEECHAT_RC_OK;
        }

        const std::string pub_service = argv[2];
        const std::string pub_node    = argv[3];

        if (subcmd == "retract")
        {
            // ── /feed retract <service> <node> <item-id> ──────────────────
            const std::string retract_id = argv[4];

            xmpp_stanza_t *iq = xmpp_iq_new(ptr_account->context, "set",
                                             xmpp_uuid_gen(ptr_account->context));
            xmpp_stanza_set_to(iq, pub_service.c_str());
            xmpp_stanza_set_from(iq, ptr_account->jid().data());

            xmpp_stanza_t *pubsub = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(pubsub, "pubsub");
            xmpp_stanza_set_ns(pubsub, "http://jabber.org/protocol/pubsub");

            xmpp_stanza_t *retract_el = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(retract_el, "retract");
            xmpp_stanza_set_attribute(retract_el, "node", pub_node.c_str());
            xmpp_stanza_set_attribute(retract_el, "notify", "true");

            xmpp_stanza_t *item_el = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(item_el, "item");
            xmpp_stanza_set_id(item_el, retract_id.c_str());
            xmpp_stanza_add_child(retract_el, item_el);
            xmpp_stanza_release(item_el);

            xmpp_stanza_add_child(pubsub, retract_el);
            xmpp_stanza_release(retract_el);
            xmpp_stanza_add_child(iq, pubsub);
            xmpp_stanza_release(pubsub);

            ptr_account->connection.send(iq);
            xmpp_stanza_release(iq);

            weechat_printf(buffer, "%sRetracted item %s from %s/%s",
                           weechat_prefix("network"),
                           retract_id.c_str(), pub_service.c_str(), pub_node.c_str());
            return WEECHAT_RC_OK;
        }

        // ── /feed post / /feed reply ─────────────────────────────────────
        // For reply: argv[4] = in-reply-to item id, argv_eol[5] = body text
        // For post:  argv_eol[4] = body text
        std::string reply_to_id;
        const char *body_raw = nullptr;
        if (subcmd == "reply")
        {
            reply_to_id = argv[4];
            body_raw    = argv_eol[5];
        }
        else
        {
            body_raw = argv_eol[4];
        }

        if (!body_raw || !*body_raw)
        {
            weechat_printf(buffer, _("%s%s: post text must not be empty"),
                           weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
            return WEECHAT_RC_OK;
        }
        const std::string body(body_raw);

        // Generate a stable UUID for the item
        xmpp_string_guard item_uuid_g(ptr_account->context,
                                      xmpp_uuid_gen(ptr_account->context));
        const std::string item_uuid = item_uuid_g.ptr ? item_uuid_g.ptr : "post";

        // ISO-8601 timestamp (UTC)
        char ts_buf[32];
        {
            std::time_t now = std::time(nullptr);
            std::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
        }

        // Atom tag URI: tag:<domain>,<date>:posts/<uuid>
        xmpp_string_guard domain_g(ptr_account->context,
                                   xmpp_jid_domain(ptr_account->context,
                                                   ptr_account->jid().data()));
        char date_buf[12];
        {
            std::time_t now = std::time(nullptr);
            std::strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", std::gmtime(&now));
        }
        const std::string atom_id = fmt::format("tag:{},{};posts/{}",
                                                domain_g.ptr ? domain_g.ptr : "xmpp",
                                                date_buf, item_uuid);

        // Build Atom <entry> element
        auto make_text_el = [&](xmpp_ctx_t *ctx,
                                const char *tag,
                                const char *text) -> xmpp_stanza_t * {
            xmpp_stanza_t *el = xmpp_stanza_new(ctx);
            xmpp_stanza_set_name(el, tag);
            xmpp_stanza_t *t = xmpp_stanza_new(ctx);
            xmpp_stanza_set_text(t, text);
            xmpp_stanza_add_child(el, t);
            xmpp_stanza_release(t);
            return el;
        };

        xmpp_stanza_t *entry = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(entry, "entry");
        xmpp_stanza_set_ns(entry, "http://www.w3.org/2005/Atom");

        // <title type='text'>body</title>
        xmpp_stanza_t *title_el = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(title_el, "title");
        xmpp_stanza_set_attribute(title_el, "type", "text");
        {
            xmpp_stanza_t *t = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_text(t, body.c_str());
            xmpp_stanza_add_child(title_el, t);
            xmpp_stanza_release(t);
        }
        xmpp_stanza_add_child(entry, title_el);
        xmpp_stanza_release(title_el);

        // <id>tag:…</id>
        {
            xmpp_stanza_t *id_el = make_text_el(ptr_account->context, "id", atom_id.c_str());
            xmpp_stanza_add_child(entry, id_el);
            xmpp_stanza_release(id_el);
        }

        // <published>timestamp</published>  <updated>timestamp</updated>
        {
            xmpp_stanza_t *pub_el = make_text_el(ptr_account->context, "published", ts_buf);
            xmpp_stanza_add_child(entry, pub_el);
            xmpp_stanza_release(pub_el);
            xmpp_stanza_t *upd_el = make_text_el(ptr_account->context, "updated", ts_buf);
            xmpp_stanza_add_child(entry, upd_el);
            xmpp_stanza_release(upd_el);
        }

        // <author><name>…</name><uri>xmpp:jid</uri></author>
        {
            xmpp_string_guard bare_g(ptr_account->context,
                                     xmpp_jid_bare(ptr_account->context,
                                                   ptr_account->jid().data()));
            xmpp_stanza_t *author_el = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(author_el, "author");
            {
                xmpp_stanza_t *name_el = make_text_el(ptr_account->context, "name",
                                                       bare_g.ptr ? bare_g.ptr : ptr_account->jid().data());
                xmpp_stanza_add_child(author_el, name_el);
                xmpp_stanza_release(name_el);
                std::string xmpp_uri = fmt::format("xmpp:{}", bare_g.ptr ? bare_g.ptr : "");
                xmpp_stanza_t *uri_el = make_text_el(ptr_account->context, "uri", xmpp_uri.c_str());
                xmpp_stanza_add_child(author_el, uri_el);
                xmpp_stanza_release(uri_el);
            }
            xmpp_stanza_add_child(entry, author_el);
            xmpp_stanza_release(author_el);
        }

        // <thr:in-reply-to> for replies (XEP-0472 §4.2)
        if (!reply_to_id.empty())
        {
            const std::string reply_xmpp_uri = fmt::format(
                "xmpp:{}?;node={};item={}", pub_service, pub_node, reply_to_id);
            xmpp_stanza_t *reply_el = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(reply_el, "thr:in-reply-to");
            xmpp_stanza_set_attribute(reply_el, "xmlns:thr",
                                      "http://purl.org/syndication/thread/1.0");
            xmpp_stanza_set_attribute(reply_el, "ref", reply_to_id.c_str());
            xmpp_stanza_set_attribute(reply_el, "href", reply_xmpp_uri.c_str());
            xmpp_stanza_add_child(entry, reply_el);
            xmpp_stanza_release(reply_el);
        }

        // Wrap in <item id='uuid'><entry…/></item>
        xmpp_stanza_t *item_children[2] = {entry, nullptr};
        xmpp_stanza_t *item_el = stanza__iq_pubsub_publish_item(
            ptr_account->context, nullptr, item_children, with_noop(item_uuid.c_str()));

        // Wrap in <publish node='…'><item…/></publish>
        xmpp_stanza_t *pub_children2[2] = {item_el, nullptr};
        xmpp_stanza_t *publish_el = stanza__iq_pubsub_publish(
            ptr_account->context, nullptr, pub_children2, with_noop(pub_node.c_str()));

        // <publish-options> with required XEP-0472 node config
        xmpp_stanza_t *pub_opts = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(pub_opts, "publish-options");
        {
            xmpp_stanza_t *x = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(x, "x");
            xmpp_stanza_set_ns(x, "jabber:x:data");
            xmpp_stanza_set_attribute(x, "type", "submit");

            auto add_field = [&](const char *var, const char *val) {
                xmpp_stanza_t *f = xmpp_stanza_new(ptr_account->context);
                xmpp_stanza_set_name(f, "field");
                xmpp_stanza_set_attribute(f, "var", var);
                xmpp_stanza_t *v = xmpp_stanza_new(ptr_account->context);
                xmpp_stanza_set_name(v, "value");
                xmpp_stanza_t *vt = xmpp_stanza_new(ptr_account->context);
                xmpp_stanza_set_text(vt, val);
                xmpp_stanza_add_child(v, vt); xmpp_stanza_release(vt);
                xmpp_stanza_add_child(f, v);  xmpp_stanza_release(v);
                xmpp_stanza_add_child(x, f);  xmpp_stanza_release(f);
            };

            add_field("FORM_TYPE", "http://jabber.org/protocol/pubsub#publish-options");
            add_field("pubsub#persist_items", "true");
            add_field("pubsub#max_items",     "max");
            add_field("pubsub#notify_retract","true");
            add_field("pubsub#send_last_published_item", "never");
            // XEP-0472: set pubsub#type to identify this as a social feed
            add_field("pubsub#type", "urn:xmpp:microblog:0");

            xmpp_stanza_add_child(pub_opts, x);
            xmpp_stanza_release(x);
        }

        // Assemble: <pubsub><publish/><publish-options/></pubsub>
        xmpp_stanza_t *ps_children[3] = {publish_el, pub_opts, nullptr};
        xmpp_stanza_t *pubsub_el = stanza__iq_pubsub(
            ptr_account->context, nullptr, ps_children,
            with_noop("http://jabber.org/protocol/pubsub"));

        // Wrap in <iq type='set'>
        xmpp_string_guard uid_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
        xmpp_stanza_t *iq_children[2] = {pubsub_el, nullptr};
        xmpp_stanza_t *iq = stanza__iq(ptr_account->context, nullptr, iq_children,
                                       nullptr, uid_g.ptr,
                                       ptr_account->jid().data(),
                                       pub_service.c_str(),
                                       "set");

        ptr_account->connection.send(iq);
        xmpp_stanza_release(iq);

        if (subcmd == "reply")
            weechat_printf(buffer, "%sPosted reply to %s on %s/%s",
                           weechat_prefix("network"),
                           reply_to_id.c_str(), pub_service.c_str(), pub_node.c_str());
        else
            weechat_printf(buffer, "%sPosted to %s/%s (id: %s)",
                           weechat_prefix("network"),
                           pub_service.c_str(), pub_node.c_str(), item_uuid.c_str());

        return WEECHAT_RC_OK;
    }

    std::string service_jid = argv[1];

    // Check for --all, --limit N, --before <id> anywhere in args after service-jid
    bool fetch_all = false;
    int  max_items = 20;   // default: request 20 most-recent items
    std::string node_name;
    std::string before_cursor;  // RSM <before> cursor ("" = latest page)
    for (int i = 2; i < argc; ++i)
    {
        std::string_view arg(argv[i]);
        if (arg == "--all")
            fetch_all = true;
        else if (arg == "--limit" && i + 1 < argc)
        {
            char *end = nullptr;
            long v = std::strtol(argv[i + 1], &end, 10);
            if (end && *end == '\0' && v > 0)
                max_items = static_cast<int>(v);
            ++i;   // consume the value token
        }
        else if (arg == "--before" && i + 1 < argc)
        {
            before_cursor = argv[i + 1];
            ++i;   // consume the value token
        }
        else if (node_name.empty())
            node_name = argv[i];
    }

    if (!node_name.empty())
    {
        // Specific node requested: fetch items directly.
        std::string feed_key = fmt::format("{}/{}", service_jid, node_name);

        // If no --before was given, check LMDB for a persisted RSM cursor so
        // that successive /feed invocations automatically page forward through
        // older items without the user having to copy-paste a cursor id.
        if (before_cursor.empty())
        {
            std::string cursor_key = fmt::format("pubsub:{}", feed_key);
            before_cursor = ptr_account->mam_cursor_get(cursor_key);
            if (!before_cursor.empty())
                weechat_printf(buffer,
                               "%sResuming from saved cursor for %s/%s…",
                               weechat_prefix("network"),
                               service_jid.c_str(), node_name.c_str());
        }

        // Ensure the FEED buffer exists before we send the IQ so the result
        // handler can find it.
        ptr_account->channels.try_emplace(
            feed_key,
            *ptr_account,
            weechat::channel::chat_type::FEED,
            feed_key,
            feed_key);

        weechat_printf(buffer, "%sFetching PubSub feed %s from %s (XEP-0060)…",
                       weechat_prefix("network"), node_name.c_str(), service_jid.c_str());

        // Build: <pubsub><items node=".." max_items="N"/><set xmlns=RSM><max>N</max><before>cursor</before></set></pubsub>
        std::array<xmpp_stanza_t *, 3> pub_children = {nullptr, nullptr, nullptr};
        pub_children[0] = stanza__iq_pubsub_items(ptr_account->context, nullptr,
                                                  node_name.c_str(), max_items);

        // RSM <set>
        {
            xmpp_stanza_t *rset = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(rset, "set");
            xmpp_stanza_set_ns(rset, "http://jabber.org/protocol/rsm");

            // <max>N</max>
            xmpp_stanza_t *max_el = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(max_el, "max");
            xmpp_stanza_t *max_t = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_text(max_t, std::to_string(max_items).c_str());
            xmpp_stanza_add_child(max_el, max_t); xmpp_stanza_release(max_t);
            xmpp_stanza_add_child(rset, max_el); xmpp_stanza_release(max_el);

            // <before>cursor</before>  (empty element = latest page)
            xmpp_stanza_t *before_el = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(before_el, "before");
            if (!before_cursor.empty())
            {
                xmpp_stanza_t *before_t = xmpp_stanza_new(ptr_account->context);
                xmpp_stanza_set_text(before_t, before_cursor.c_str());
                xmpp_stanza_add_child(before_el, before_t);
                xmpp_stanza_release(before_t);
            }
            xmpp_stanza_add_child(rset, before_el); xmpp_stanza_release(before_el);

            pub_children[1] = rset;
        }

        pub_children[0] = stanza__iq_pubsub(ptr_account->context, nullptr, pub_children.data(),
                                            with_noop("http://jabber.org/protocol/pubsub"));
        // stanza__iq_pubsub consumed both children; reset second slot
        pub_children[1] = nullptr;

        xmpp_string_guard uid_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
        const char *uid = uid_g.ptr;

        pub_children[0] = stanza__iq(ptr_account->context, nullptr, pub_children.data(),
                                     nullptr, uid,
                                     ptr_account->jid().data(),
                                     service_jid.c_str(),
                                     "get");

        if (uid)
            ptr_account->pubsub_fetch_ids[uid] = {service_jid, node_name, before_cursor, max_items};

        ptr_account->connection.send(pub_children[0]);
        xmpp_stanza_release(pub_children[0]);
    }
    else if (fetch_all)
    {
        // --all: discover all nodes via disco#items and fetch each one.
        weechat_printf(buffer, "%sDiscovering all PubSub nodes on %s…",
                       weechat_prefix("network"), service_jid.c_str());

        xmpp_string_guard uid_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
        const char *uid = uid_g.ptr;

        xmpp_stanza_t *iq = xmpp_iq_new(ptr_account->context, "get", uid);
        xmpp_stanza_set_to(iq, service_jid.c_str());

        xmpp_stanza_t *query = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(query, "query");
        xmpp_stanza_set_ns(query, "http://jabber.org/protocol/disco#items");
        xmpp_stanza_add_child(iq, query);
        xmpp_stanza_release(query);

        if (uid)
            ptr_account->pubsub_disco_queries[uid] = service_jid;

        ptr_account->connection.send(iq);
        xmpp_stanza_release(iq);
    }
    else
    {
        // Default: query subscriptions and fetch only subscribed nodes.
        weechat_printf(buffer, "%sFetching subscribed PubSub feeds from %s…",
                       weechat_prefix("network"), service_jid.c_str());

        xmpp_string_guard uid_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
        const char *uid = uid_g.ptr;

        // <iq type="get" to="service">
        //   <pubsub xmlns="http://jabber.org/protocol/pubsub">
        //     <subscriptions/>
        //   </pubsub>
        // </iq>
        xmpp_stanza_t *iq = xmpp_iq_new(ptr_account->context, "get", uid);
        xmpp_stanza_set_to(iq, service_jid.c_str());

        xmpp_stanza_t *pubsub = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(pubsub, "pubsub");
        xmpp_stanza_set_ns(pubsub, "http://jabber.org/protocol/pubsub");

        xmpp_stanza_t *subs = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(subs, "subscriptions");
        xmpp_stanza_add_child(pubsub, subs);
        xmpp_stanza_release(subs);

        xmpp_stanza_add_child(iq, pubsub);
        xmpp_stanza_release(pubsub);

        if (uid)
            ptr_account->pubsub_subscriptions_queries[uid] = service_jid;

        ptr_account->connection.send(iq);
        xmpp_stanza_release(iq);
    }

    return WEECHAT_RC_OK;
}

