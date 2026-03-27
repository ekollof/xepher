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
        // No arguments: behave as /feed discover (list known pubsub services
        // and fetch subscribed nodes from each one automatically).
        const auto &kps = ptr_account->known_pubsub_services;
        if (kps.empty())
        {
            weechat_printf(buffer,
                           _("%s%s: no PubSub services discovered yet.\n"
                             "  Your server may not have a pubsub component, or you may need to reconnect.\n"
                             "  To fetch a specific service: /feed <service-jid>"),
                           weechat_prefix("network"), WEECHAT_XMPP_PLUGIN_NAME);
            return WEECHAT_RC_OK;
        }

        weechat_printf(buffer,
                       _("%s%s: discovered %zu PubSub service(s). Fetching subscriptions…"),
                       weechat_prefix("network"), WEECHAT_XMPP_PLUGIN_NAME,
                       kps.size());
        for (const auto &svc : kps)
        {
            weechat_printf(buffer,
                           "%sFetching subscribed PubSub feeds from %s…",
                           weechat_prefix("network"), svc.c_str());
            xmpp_string_guard uid_g(ptr_account->context,
                                    xmpp_uuid_gen(ptr_account->context));
            const char *uid = uid_g.ptr;
            xmpp_stanza_t *iq = xmpp_iq_new(ptr_account->context, "get", uid);
            xmpp_stanza_set_to(iq, svc.c_str());
            xmpp_stanza_t *pubsub = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(pubsub, "pubsub");
            xmpp_stanza_set_ns(pubsub, "http://jabber.org/protocol/pubsub");
            xmpp_stanza_t *subs_el = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(subs_el, "subscriptions");
            xmpp_stanza_add_child(pubsub, subs_el);
            xmpp_stanza_release(subs_el);
            xmpp_stanza_add_child(iq, pubsub);
            xmpp_stanza_release(pubsub);
            if (uid)
                ptr_account->pubsub_subscriptions_queries[uid] = svc;
            ptr_account->connection.send(iq);
            xmpp_stanza_release(iq);
        }
        return WEECHAT_RC_OK;
    }

    // XEP-0472 write path: post / reply / retract sub-commands
    std::string_view subcmd(argv[1]);

    // ── /feed discover ───────────────────────────────────────────────────────
    // List PubSub services discovered on our server at connect time and
    // optionally fetch subscribed nodes from each one.
    if (subcmd == "discover")
    {
        const auto &kps = ptr_account->known_pubsub_services;
        if (kps.empty())
        {
            weechat_printf(buffer,
                           _("%s%s: no PubSub services discovered yet — "
                             "reconnect or check that your server has a pubsub component"),
                           weechat_prefix("network"), WEECHAT_XMPP_PLUGIN_NAME);
            return WEECHAT_RC_OK;
        }

        weechat_printf(buffer,
                       _("%s%s: discovered %zu PubSub service(s) on your server:"),
                       weechat_prefix("network"), WEECHAT_XMPP_PLUGIN_NAME,
                       kps.size());
        for (const auto &svc : kps)
            weechat_printf(buffer, "  %s  /feed %s",
                           weechat_color("chat_server"), svc.c_str());

        // If --all flag present, auto-fetch all nodes from every discovered service.
        bool fetch_all_svcs = false;
        for (int i = 2; i < argc; ++i)
            if (std::string_view(argv[i]) == "--all")
                fetch_all_svcs = true;

        if (fetch_all_svcs)
        {
            for (const auto &svc : kps)
            {
                weechat_printf(buffer,
                               "%sDiscovering all PubSub nodes on %s…",
                               weechat_prefix("network"), svc.c_str());
                xmpp_string_guard uid_g(ptr_account->context,
                                        xmpp_uuid_gen(ptr_account->context));
                const char *uid = uid_g.ptr;
                xmpp_stanza_t *iq = xmpp_iq_new(ptr_account->context, "get", uid);
                xmpp_stanza_set_to(iq, svc.c_str());
                xmpp_stanza_t *query = xmpp_stanza_new(ptr_account->context);
                xmpp_stanza_set_name(query, "query");
                xmpp_stanza_set_ns(query, "http://jabber.org/protocol/disco#items");
                xmpp_stanza_add_child(iq, query);
                xmpp_stanza_release(query);
                if (uid)
                    ptr_account->pubsub_disco_queries[uid] = svc;
                ptr_account->connection.send(iq);
                xmpp_stanza_release(iq);
            }
        }
        else
        {
            // Default: fetch subscribed nodes from each discovered service.
            for (const auto &svc : kps)
            {
                weechat_printf(buffer,
                               "%sFetching subscribed PubSub feeds from %s…",
                               weechat_prefix("network"), svc.c_str());
                xmpp_string_guard uid_g(ptr_account->context,
                                        xmpp_uuid_gen(ptr_account->context));
                const char *uid = uid_g.ptr;
                xmpp_stanza_t *iq = xmpp_iq_new(ptr_account->context, "get", uid);
                xmpp_stanza_set_to(iq, svc.c_str());
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
                    ptr_account->pubsub_subscriptions_queries[uid] = svc;
                ptr_account->connection.send(iq);
                xmpp_stanza_release(iq);
            }
        }
        return WEECHAT_RC_OK;
    }

    // ── /feed subscribe <service> <node> ────────────────────────────────────
    if (subcmd == "subscribe" || subcmd == "unsubscribe")
    {
        if (argc < 4)
        {
            weechat_printf(buffer,
                           _("%s%s: usage: /feed %s <service-jid> <node>"),
                           weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                           std::string(subcmd).c_str());
            return WEECHAT_RC_OK;
        }

        const std::string svc  = argv[2];
        const std::string node = argv[3];
        const std::string feed_key = fmt::format("{}/{}", svc, node);

        xmpp_string_guard bare_g(ptr_account->context,
                                 xmpp_jid_bare(ptr_account->context,
                                               ptr_account->jid().data()));
        const std::string my_jid = bare_g.ptr ? bare_g.ptr : ptr_account->jid().data();

        xmpp_string_guard uid_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));

        if (subcmd == "subscribe")
        {
            // <iq type='set' to='service'>
            //   <pubsub xmlns='…'><subscribe node='…' jid='…'/></pubsub>
            // </iq>
            xmpp_stanza_t *sub_el = stanza__iq_pubsub_subscribe(
                ptr_account->context, nullptr,
                with_noop(node.c_str()), with_noop(my_jid.c_str()));

            xmpp_stanza_t *sub_children[2] = {sub_el, nullptr};
            xmpp_stanza_t *ps_el = stanza__iq_pubsub(
                ptr_account->context, nullptr, sub_children,
                with_noop("http://jabber.org/protocol/pubsub"));

            xmpp_stanza_t *iq_children[2] = {ps_el, nullptr};
            xmpp_stanza_t *iq = stanza__iq(ptr_account->context, nullptr, iq_children,
                                           nullptr, uid_g.ptr,
                                           my_jid.c_str(), svc.c_str(), "set");

            if (uid_g.ptr)
                ptr_account->pubsub_subscribe_queries[uid_g.ptr] = {feed_key, buffer};

            ptr_account->connection.send(iq);
            xmpp_stanza_release(iq);

            weechat_printf(buffer, "%sSubscribing to %s/%s…",
                           weechat_prefix("network"), svc.c_str(), node.c_str());
        }
        else  // unsubscribe
        {
            // <iq type='set' to='service'>
            //   <pubsub xmlns='…'><unsubscribe node='…' jid='…'/></pubsub>
            // </iq>
            xmpp_stanza_t *unsub_el = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(unsub_el, "unsubscribe");
            xmpp_stanza_set_attribute(unsub_el, "node", node.c_str());
            xmpp_stanza_set_attribute(unsub_el, "jid", my_jid.c_str());

            xmpp_stanza_t *unsub_children[2] = {unsub_el, nullptr};
            xmpp_stanza_t *ps_el = stanza__iq_pubsub(
                ptr_account->context, nullptr, unsub_children,
                with_noop("http://jabber.org/protocol/pubsub"));

            xmpp_stanza_t *iq_children[2] = {ps_el, nullptr};
            xmpp_stanza_t *iq = stanza__iq(ptr_account->context, nullptr, iq_children,
                                           nullptr, uid_g.ptr,
                                           my_jid.c_str(), svc.c_str(), "set");

            if (uid_g.ptr)
                ptr_account->pubsub_unsubscribe_queries[uid_g.ptr] = {feed_key, buffer};

            ptr_account->connection.send(iq);
            xmpp_stanza_release(iq);

            weechat_printf(buffer, "%sUnsubscribing from %s/%s…",
                           weechat_prefix("network"), svc.c_str(), node.c_str());
        }

        return WEECHAT_RC_OK;
    }

    // ── /feed subscriptions <service> ───────────────────────────────────────
    if (subcmd == "subscriptions")
    {
        if (argc < 3)
        {
            weechat_printf(buffer,
                           _("%s%s: usage: /feed subscriptions <service-jid>"),
                           weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
            return WEECHAT_RC_OK;
        }

        const std::string svc = argv[2];
        xmpp_string_guard uid_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
        const char *uid = uid_g.ptr;

        xmpp_stanza_t *iq = xmpp_iq_new(ptr_account->context, "get", uid);
        xmpp_stanza_set_to(iq, svc.c_str());

        xmpp_stanza_t *pubsub = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(pubsub, "pubsub");
        xmpp_stanza_set_ns(pubsub, "http://jabber.org/protocol/pubsub");

        xmpp_stanza_t *subs_el = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(subs_el, "subscriptions");
        xmpp_stanza_add_child(pubsub, subs_el);
        xmpp_stanza_release(subs_el);

        xmpp_stanza_add_child(iq, pubsub);
        xmpp_stanza_release(pubsub);

        if (uid)
            ptr_account->pubsub_subscriptions_queries[uid] = svc;

        ptr_account->connection.send(iq);
        xmpp_stanza_release(iq);

        weechat_printf(buffer, "%sFetching subscriptions from %s…",
                       weechat_prefix("network"), svc.c_str());

        return WEECHAT_RC_OK;
    }

    // ── /feed repeat <service> <node> <item-id> [comment] ───────────────────
    // XEP-0472 §4.5 boost/repeat: publish a new entry with <link rel='via'>
    // pointing to the original item. The receiving client fetches the original.
    //
    // Short form: /feed repeat #N [comment]   (from a FEED buffer; resolves alias)
    // Long  form: /feed repeat <service> <node> <item-id> [comment]
    if (subcmd == "repeat")
    {
        // Short-form detection: "/feed repeat #N [comment]" or "/feed repeat N [comment]"
        bool repeat_short_form = (argc >= 3)
            && (argv[2][0] == '#' || std::isdigit((unsigned char)argv[2][0]));

        int repeat_min_argc = repeat_short_form ? 3 : 5;
        if (argc < repeat_min_argc)
        {
            weechat_printf(buffer,
                           _("%s%s: usage: /feed repeat <service-jid> <node> <item-id> [comment]\n"
                             "           or: /feed repeat #N [comment]  (from a feed buffer)"),
                           weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
            return WEECHAT_RC_OK;
        }

        std::string rep_service;
        std::string rep_node;
        std::string rep_item_id;
        std::string rep_comment;

        if (repeat_short_form)
        {
            // Infer feed from current FEED buffer.
            if (!ptr_channel || ptr_channel->type != weechat::channel::chat_type::FEED)
            {
                weechat_printf(buffer,
                               _("%s%s: /feed repeat short form requires running from a feed buffer"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                return WEECHAT_RC_OK;
            }
            const std::string &fk = ptr_channel->name;
            auto slash = fk.find('/');
            if (slash == std::string::npos)
            {
                weechat_printf(buffer, _("%s%s: cannot parse feed key '%s'"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                               fk.c_str());
                return WEECHAT_RC_OK;
            }
            rep_service = fk.substr(0, slash);
            rep_node    = fk.substr(slash + 1);

            std::string_view alias_arg(argv[2]);
            rep_item_id = ptr_account->feed_alias_resolve(fk, alias_arg);
            if (rep_item_id.empty())
            {
                weechat_printf(buffer, _("%s%s: unknown alias %s in feed %s"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                               argv[2], fk.c_str());
                return WEECHAT_RC_OK;
            }
            rep_comment = argc > 3 ? argv_eol[3] : "";
        }
        else
        {
            rep_service = argv[2];
            rep_node    = argv[3];
            rep_item_id = argv[4];
            rep_comment = argc > 5 ? argv_eol[5] : "";
        }

        const std::string via_uri = fmt::format(
            "xmpp:{}?;node={};item={}", rep_service, rep_node, rep_item_id);

        xmpp_string_guard item_uuid_g(ptr_account->context,
                                      xmpp_uuid_gen(ptr_account->context));
        const std::string item_uuid = item_uuid_g.ptr ? item_uuid_g.ptr : "repeat";

        char ts_buf[32];
        {
            std::time_t now = std::time(nullptr);
            std::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
        }

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

        auto make_text_el_r = [&](xmpp_ctx_t *ctx,
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

        // <title>Shared: item-id [— comment]</title>
        {
            std::string repeat_title = rep_comment.empty()
                ? fmt::format("Shared: {}", rep_item_id)
                : fmt::format("Shared: {} — {}", rep_item_id, rep_comment);
            xmpp_stanza_t *title_el = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(title_el, "title");
            xmpp_stanza_set_attribute(title_el, "type", "text");
            xmpp_stanza_t *t = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_text(t, repeat_title.c_str());
            xmpp_stanza_add_child(title_el, t);
            xmpp_stanza_release(t);
            xmpp_stanza_add_child(entry, title_el);
            xmpp_stanza_release(title_el);
        }

        {
            xmpp_stanza_t *id_el = make_text_el_r(ptr_account->context, "id", atom_id.c_str());
            xmpp_stanza_add_child(entry, id_el);
            xmpp_stanza_release(id_el);
            xmpp_stanza_t *pub_el = make_text_el_r(ptr_account->context, "published", ts_buf);
            xmpp_stanza_add_child(entry, pub_el);
            xmpp_stanza_release(pub_el);
            xmpp_stanza_t *upd_el = make_text_el_r(ptr_account->context, "updated", ts_buf);
            xmpp_stanza_add_child(entry, upd_el);
            xmpp_stanza_release(upd_el);
        }

        // <author>
        {
            xmpp_string_guard bare_g(ptr_account->context,
                                     xmpp_jid_bare(ptr_account->context,
                                                   ptr_account->jid().data()));
            xmpp_stanza_t *author_el = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(author_el, "author");
            xmpp_stanza_t *name_el = make_text_el_r(ptr_account->context, "name",
                bare_g.ptr ? bare_g.ptr : ptr_account->jid().data());
            xmpp_stanza_add_child(author_el, name_el);
            xmpp_stanza_release(name_el);
            std::string xmpp_uri = fmt::format("xmpp:{}", bare_g.ptr ? bare_g.ptr : "");
            xmpp_stanza_t *uri_el = make_text_el_r(ptr_account->context, "uri", xmpp_uri.c_str());
            xmpp_stanza_add_child(author_el, uri_el);
            xmpp_stanza_release(uri_el);
            xmpp_stanza_add_child(entry, author_el);
            xmpp_stanza_release(author_el);
        }

        // <link rel='via' href='xmpp:…' ref='atom-id'/>  (XEP-0472 §4.5 boost/repeat)
        // 'href' = XMPP URI of original item; 'ref' = Atom <id> of original (if known).
        {
            const std::string rep_feed_key = fmt::format("{}/{}", rep_service, rep_node);
            const std::string rep_atom_id  = ptr_account->feed_atom_id_get(rep_feed_key, rep_item_id);
            xmpp_stanza_t *via_el = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(via_el, "link");
            xmpp_stanza_set_attribute(via_el, "rel",  "via");
            xmpp_stanza_set_attribute(via_el, "href", via_uri.c_str());
            if (!rep_atom_id.empty())
                xmpp_stanza_set_attribute(via_el, "ref", rep_atom_id.c_str());
            xmpp_stanza_add_child(entry, via_el);
            xmpp_stanza_release(via_el);
        }

        // Optional <content>comment</content>
        if (!rep_comment.empty())
        {
            xmpp_stanza_t *content_el = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(content_el, "content");
            xmpp_stanza_set_attribute(content_el, "type", "text");
            xmpp_stanza_t *t = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_text(t, rep_comment.c_str());
            xmpp_stanza_add_child(content_el, t);
            xmpp_stanza_release(t);
            xmpp_stanza_add_child(entry, content_el);
            xmpp_stanza_release(content_el);
        }

        // <generator>
        {
            xmpp_stanza_t *gen_el = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(gen_el, "generator");
            xmpp_stanza_set_attribute(gen_el, "uri",
                "https://github.com/ekollof/weechat-xmpp-improved");
            xmpp_stanza_set_attribute(gen_el, "version", WEECHAT_XMPP_PLUGIN_VERSION);
            xmpp_stanza_t *gt = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_text(gt, "weechat-xmpp-improved");
            xmpp_stanza_add_child(gen_el, gt);
            xmpp_stanza_release(gt);
            xmpp_stanza_add_child(entry, gen_el);
            xmpp_stanza_release(gen_el);
        }

        xmpp_stanza_t *item_children_r[2] = {entry, nullptr};
        xmpp_stanza_t *item_el_r = stanza__iq_pubsub_publish_item(
            ptr_account->context, nullptr, item_children_r, with_noop(item_uuid.c_str()));

        xmpp_stanza_t *pub_children_r[2] = {item_el_r, nullptr};
        xmpp_stanza_t *publish_el_r = stanza__iq_pubsub_publish(
            ptr_account->context, nullptr, pub_children_r, with_noop(rep_node.c_str()));

        // publish-options (same set as /feed post)
        xmpp_stanza_t *pub_opts_r = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(pub_opts_r, "publish-options");
        {
            xmpp_stanza_t *x = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(x, "x");
            xmpp_stanza_set_ns(x, "jabber:x:data");
            xmpp_stanza_set_attribute(x, "type", "submit");
            auto add_field_r = [&](const char *var, const char *val) {
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
            add_field_r("FORM_TYPE", "http://jabber.org/protocol/pubsub#publish-options");
            add_field_r("pubsub#persist_items", "true");
            add_field_r("pubsub#max_items",     "max");
            add_field_r("pubsub#notify_retract","true");
            add_field_r("pubsub#send_last_published_item", "never");
            add_field_r("pubsub#deliver_payloads", "false");
            add_field_r("pubsub#type", "urn:xmpp:microblog:0");
            xmpp_stanza_add_child(pub_opts_r, x);
            xmpp_stanza_release(x);
        }

        xmpp_stanza_t *ps_r[3] = {publish_el_r, pub_opts_r, nullptr};
        xmpp_stanza_t *pubsub_el_r = stanza__iq_pubsub(
            ptr_account->context, nullptr, ps_r,
            with_noop("http://jabber.org/protocol/pubsub"));

        xmpp_string_guard uid_r(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
        xmpp_stanza_t *iq_r_children[2] = {pubsub_el_r, nullptr};
        xmpp_stanza_t *iq_r = stanza__iq(ptr_account->context, nullptr, iq_r_children,
                                         nullptr, uid_r.ptr,
                                         ptr_account->jid().data(),
                                         rep_service.c_str(), "set");

        if (uid_r.ptr)
            ptr_account->pubsub_publish_ids[uid_r.ptr] = {rep_service, rep_node, item_uuid, buffer};

        ptr_account->connection.send(iq_r);
        xmpp_stanza_release(iq_r);

        weechat_printf(buffer, "%sRepeated %s/%s item %s (id: %s)",
                       weechat_prefix("network"),
                       rep_service.c_str(), rep_node.c_str(),
                       rep_item_id.c_str(), item_uuid.c_str());
        return WEECHAT_RC_OK;
    }

    // ── /feed post <service> <node> <text> ──────────────────────────────────
    if (subcmd == "comments")
    {
        // Accepted forms:
        //   /feed comments <service> <node> <item-id|#N>   (explicit)
        //   /feed comments #N                              (short, from a FEED buffer)
        bool short_form = (argc == 3 && argv[2][0] == '#')
                       || (argc == 3 && std::isdigit((unsigned char)argv[2][0]));

        if (!short_form && argc < 5)
        {
            weechat_printf(buffer,
                           _("%s%s: usage: /feed comments <service-jid> <node> <item-id|#N>\n"
                             "           or: /feed comments #N  (from a feed buffer)"),
                           weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
            return WEECHAT_RC_OK;
        }

        std::string service_jid, node_name, item_id;

        if (short_form)
        {
            // Infer feed from current buffer.
            if (!ptr_channel || ptr_channel->type != weechat::channel::chat_type::FEED)
            {
                weechat_printf(buffer,
                               _("%s%s: /feed comments #N requires running from a feed buffer"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                return WEECHAT_RC_OK;
            }
            const std::string &feed_key_cur = ptr_channel->name;
            auto slash = feed_key_cur.find('/');
            if (slash == std::string::npos)
            {
                weechat_printf(buffer, _("%s%s: cannot parse feed key '%s'"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                               feed_key_cur.c_str());
                return WEECHAT_RC_OK;
            }
            service_jid = feed_key_cur.substr(0, slash);
            node_name   = feed_key_cur.substr(slash + 1);
            item_id     = ptr_account->feed_alias_resolve(feed_key_cur, argv[2]);
            if (item_id.empty())
            {
                weechat_printf(buffer, _("%s%s: unknown alias %s in feed %s"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                               argv[2], feed_key_cur.c_str());
                return WEECHAT_RC_OK;
            }
        }
        else
        {
            service_jid = argv[2];
            node_name   = argv[3];
            // argv[4] may be a raw item-id or a #N alias.
            const std::string feed_key_tmp = fmt::format("{}/{}", service_jid, node_name);
            std::string_view arg4(argv[4]);
            if (!arg4.empty() && (arg4[0] == '#' || std::isdigit((unsigned char)arg4[0])))
            {
                item_id = ptr_account->feed_alias_resolve(feed_key_tmp, arg4);
                if (item_id.empty())
                {
                    weechat_printf(buffer, _("%s%s: unknown alias %s in feed %s"),
                                   weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                                   argv[4], feed_key_tmp.c_str());
                    return WEECHAT_RC_OK;
                }
            }
            else
            {
                item_id = argv[4];
            }
        }

        const std::string feed_key    = fmt::format("{}/{}", service_jid, node_name);
        const std::string replies_uri = ptr_account->feed_replies_link_get(feed_key, item_id);
        if (replies_uri.empty())
        {
            weechat_printf(buffer,
                           _("%s%s: no cached comments link for %s/%s item %s; fetch the post first"),
                           weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                           service_jid.c_str(), node_name.c_str(), item_id.c_str());
            return WEECHAT_RC_OK;
        }

        auto percent_decode = [](std::string_view in) {
            std::string out;
            out.reserve(in.size());
            auto hexval = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                return -1;
            };
            for (size_t i = 0; i < in.size(); ++i)
            {
                if (in[i] == '%' && i + 2 < in.size())
                {
                    int hi = hexval(in[i + 1]);
                    int lo = hexval(in[i + 2]);
                    if (hi >= 0 && lo >= 0)
                    {
                        out.push_back(static_cast<char>((hi << 4) | lo));
                        i += 2;
                        continue;
                    }
                }
                out.push_back(in[i]);
            }
            return out;
        };

        std::string comments_service;
        std::string comments_node;
        if (replies_uri.rfind("xmpp:", 0) == 0)
        {
            auto qpos = replies_uri.find('?');
            comments_service = (qpos == std::string::npos)
                ? replies_uri.substr(5)
                : replies_uri.substr(5, qpos - 5);
            if (qpos != std::string::npos)
            {
                std::string_view query(replies_uri.data() + qpos + 1, replies_uri.size() - qpos - 1);
                auto npos = query.find("node=");
                if (npos != std::string_view::npos)
                {
                    auto start = npos + 5;
                    auto end = query.find(';', start);
                    comments_node = percent_decode(query.substr(start, end == std::string_view::npos ? query.size() - start : end - start));
                }
            }
        }

        if (comments_service.empty() || comments_node.empty())
        {
            weechat_printf(buffer,
                           _("%s%s: could not parse comments link: %s"),
                           weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                           replies_uri.c_str());
            return WEECHAT_RC_OK;
        }

        std::string comments_feed_key = fmt::format("{}/{}", comments_service, comments_node);
        ptr_account->channels.try_emplace(
            comments_feed_key,
            *ptr_account,
            weechat::channel::chat_type::FEED,
            comments_feed_key,
            comments_feed_key);

        std::array<xmpp_stanza_t *, 3> pub_children = {nullptr, nullptr, nullptr};
        pub_children[0] = stanza__iq_pubsub_items(ptr_account->context, nullptr,
                                                  comments_node.c_str(), 20);
        pub_children[0] = stanza__iq_pubsub(ptr_account->context, nullptr, pub_children.data(),
                                            with_noop("http://jabber.org/protocol/pubsub"));
        pub_children[1] = nullptr;

        xmpp_string_guard uid_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
        const char *uid = uid_g.ptr;
        pub_children[0] = stanza__iq(ptr_account->context, nullptr, pub_children.data(),
                                     nullptr, uid,
                                     ptr_account->jid().data(),
                                     comments_service.c_str(),
                                     "get");
        if (uid)
            ptr_account->pubsub_fetch_ids[uid] = {comments_service, comments_node, "", 20};

        ptr_account->connection.send(pub_children[0]);
        xmpp_stanza_release(pub_children[0]);

        weechat_printf(buffer, "%sFetching comments for %s/%s item %s from %s/%s…",
                       weechat_prefix("network"),
                       service_jid.c_str(), node_name.c_str(), item_id.c_str(),
                       comments_service.c_str(), comments_node.c_str());
        return WEECHAT_RC_OK;
    }

    if (subcmd == "post" || subcmd == "reply" || subcmd == "retract")
    {
        // Short-form detection for /feed reply: "/feed reply #N text"
        // (no explicit service/node — inferred from the current FEED buffer).
        bool reply_short_form = (subcmd == "reply")
            && (argc >= 4)
            && (argv[2][0] == '#' || std::isdigit((unsigned char)argv[2][0]));

        // Short-form detection for /feed post: "/feed post <text>"
        // (no explicit service/node — inferred from the current FEED buffer).
        //
        // Rules (evaluated top-to-bottom, first match wins):
        //   1. Explicit override: argv[2] == "--" → always short form; body = argv_eol[3].
        //   2. argc < 5 AND in a feed buffer → unambiguously short form (long form needs ≥5).
        //   3. argc >= 5 AND in a feed buffer AND argv[2] doesn't look like a service JID
        //      (no '.' or '@') → short form.
        //   4. Otherwise → long form.
        //
        // If the post body starts with a word that contains '.' or '@', use "--":
        //   /feed post -- hello.world@example.com is my favourite site
        bool post_force_short = (subcmd == "post") && (argc >= 3)
            && std::string_view(argv[2]) == "--";
        bool post_short_form = post_force_short
            || ((subcmd == "post") && (argc >= 3)
                && (argc < 5
                    || (ptr_channel && ptr_channel->type == weechat::channel::chat_type::FEED
                        && std::string_view(argv[2]).find('.') == std::string_view::npos
                        && std::string_view(argv[2]).find('@') == std::string_view::npos
                        && std::string_view(argv[2]) != "--open")));

        // --edit flag: delegate to feed-compose Python script.
        // Supported forms:
        //   /feed post --edit              → open blank composer (short form from feed buffer)
        //   /feed reply #N --edit          → open composer pre-wired for reply to #N
        //   /feed reply svc node id --edit → same, long form
        // The last argv must be "--edit"; we strip it and hand off.
        if (subcmd != "retract" && argc >= 3
            && std::string_view(argv[argc - 1]) == "--edit")
        {
            std::string compose_args;
            if (subcmd == "reply")
            {
                // Determine the alias/id argument regardless of short/long form.
                // short form: argv[2] = alias
                // long  form: argv[4] = alias/id  (service=argv[2], node=argv[3])
                std::string_view alias_arg = reply_short_form
                    ? std::string_view(argv[2])
                    : (argc >= 5 ? std::string_view(argv[4]) : std::string_view());
                if (!alias_arg.empty())
                    compose_args = fmt::format("--reply {}", alias_arg);
            }
            // For post, no extra args — open a blank composer in the current buffer.
            weechat_command(buffer,
                fmt::format("/feed-compose {}", compose_args).c_str());
            return WEECHAT_RC_OK;
        }

        int min_argc = subcmd == "reply"   ? (reply_short_form ? 4 : 6)
                     : subcmd == "retract" ? 5
                     : (post_short_form    ? 3 : 5); // post
        if (argc < min_argc)
        {
            if (subcmd == "post")
                weechat_printf(buffer,
                               _("%s%s: usage: /feed post <service-jid> <node> <text>\n"
                                 "           or: /feed post <text>  (from a feed buffer)\n"
                                 "           or: /feed post -- <text>  (force short form when body starts with a JID-like word)\n"
                                 "           or: /feed post --edit  (open $EDITOR, requires feed-compose.py)"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
            else if (subcmd == "reply")
                weechat_printf(buffer,
                               _("%s%s: usage: /feed reply <service-jid> <node> <item-id|#N> <text>\n"
                                 "           or: /feed reply #N <text>  (from a feed buffer)\n"
                                 "           or: /feed reply #N --edit  (open $EDITOR, requires feed-compose.py)"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
            else
                weechat_printf(buffer,
                               _("%s%s: usage: /feed retract <service-jid> <node> <item-id>"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
            return WEECHAT_RC_OK;
        }

        std::string pub_service;
        std::string pub_node;

        if (reply_short_form || post_short_form)
        {
            // Infer feed from current FEED buffer.
            if (!ptr_channel || ptr_channel->type != weechat::channel::chat_type::FEED)
            {
                weechat_printf(buffer,
                               _("%s%s: /feed %s short form requires running from a feed buffer"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                               std::string(subcmd).c_str());
                return WEECHAT_RC_OK;
            }
            const std::string &fk = ptr_channel->name;
            auto slash = fk.find('/');
            if (slash == std::string::npos)
            {
                weechat_printf(buffer, _("%s%s: cannot parse feed key '%s'"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                               fk.c_str());
                return WEECHAT_RC_OK;
            }
            pub_service = fk.substr(0, slash);
            pub_node    = fk.substr(slash + 1);
        }
        else
        {
            pub_service = argv[2];
            pub_node    = argv[3];
        }

        if (subcmd == "retract")
        {
            // ── /feed retract <service> <node> <item-id> ──────────────────
            const std::string retract_id = argv[4];

            xmpp_string_guard retract_uid_g(ptr_account->context,
                                            xmpp_uuid_gen(ptr_account->context));
            xmpp_stanza_t *iq = xmpp_iq_new(ptr_account->context, "set",
                                             retract_uid_g.ptr);
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

            // Track the IQ so the error handler can report server-side failures
            // (e.g. item-not-found, forbidden) instead of silently dropping them.
            if (retract_uid_g.ptr)
                ptr_account->pubsub_publish_ids[retract_uid_g.ptr] = {
                    pub_service, pub_node, retract_id, buffer};

            ptr_account->connection.send(iq);
            xmpp_stanza_release(iq);

            weechat_printf(buffer, "%sRetracted item %s from %s/%s",
                           weechat_prefix("network"),
                           retract_id.c_str(), pub_service.c_str(), pub_node.c_str());
            return WEECHAT_RC_OK;
        }

        // ── /feed post / /feed reply ─────────────────────────────────────
        // Short form: /feed reply #N <text>  → argv[2]=#N, argv_eol[3]=text
        // Long  form: /feed reply svc node <item-id|#N> <text>
        // Optional flag: --open  (sets pubsub#access_model=open)
        bool access_open = false;
        std::string reply_to_id;
        // For replies: the actual publish target (comments node, not the blog node)
        std::string reply_target_service;
        std::string reply_target_node;
        const char *body_raw = nullptr;
        if (subcmd == "reply")
        {
            const std::string pub_feed_key = fmt::format("{}/{}", pub_service, pub_node);
            if (reply_short_form)
            {
                // argv[2] = alias, argv_eol[3] = body (possibly with --open)
                std::string_view alias_arg(argv[2]);
                reply_to_id = ptr_account->feed_alias_resolve(pub_feed_key, alias_arg);
                if (reply_to_id.empty())
                {
                    weechat_printf(buffer, _("%s%s: unknown alias %s in feed %s"),
                                   weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                                   argv[2], pub_feed_key.c_str());
                    return WEECHAT_RC_OK;
                }
                if (argc > 3 && std::string_view(argv[3]) == "--open")
                {
                    access_open = true;
                    body_raw    = argv_eol[4];
                }
                else
                    body_raw = argv_eol[3];
            }
            else
            {
                // argv[4] = item-id or alias
                std::string_view arg4(argv[4]);
                if (!arg4.empty() && (arg4[0] == '#' || std::isdigit((unsigned char)arg4[0])))
                {
                    reply_to_id = ptr_account->feed_alias_resolve(pub_feed_key, arg4);
                    if (reply_to_id.empty())
                    {
                        weechat_printf(buffer, _("%s%s: unknown alias %s in feed %s"),
                                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                                       argv[4], pub_feed_key.c_str());
                        return WEECHAT_RC_OK;
                    }
                }
                else
                    reply_to_id = argv[4];

                if (argc > 5 && std::string_view(argv[5]) == "--open")
                {
                    access_open = true;
                    body_raw    = argv_eol[6];
                }
                else
                    body_raw    = argv_eol[5];
            }

            // Resolve the comments node URI for this reply.
            // XEP-0472 §5: replies go to the comments node, not the blog node.
            const std::string replies_uri_for_reply =
                ptr_account->feed_replies_link_get(pub_feed_key, reply_to_id);
            if (!replies_uri_for_reply.empty() && replies_uri_for_reply.rfind("xmpp:", 0) == 0)
            {
                auto qpos = replies_uri_for_reply.find('?');
                reply_target_service = (qpos == std::string::npos)
                    ? replies_uri_for_reply.substr(5)
                    : replies_uri_for_reply.substr(5, qpos - 5);
                if (qpos != std::string::npos)
                {
                    std::string_view query(replies_uri_for_reply.data() + qpos + 1,
                                           replies_uri_for_reply.size() - qpos - 1);
                    auto npos2 = query.find("node=");
                    if (npos2 != std::string_view::npos)
                    {
                        auto start = npos2 + 5;
                        auto end   = query.find(';', start);
                        // percent-decode the node name
                        std::string raw_node(query.substr(start,
                            end == std::string_view::npos ? query.size() - start : end - start));
                        auto hexval = [](char c) -> int {
                            if (c >= '0' && c <= '9') return c - '0';
                            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                            return -1;
                        };
                        for (size_t i = 0; i < raw_node.size(); ++i)
                        {
                            if (raw_node[i] == '%' && i + 2 < raw_node.size())
                            {
                                int hi = hexval(raw_node[i + 1]);
                                int lo = hexval(raw_node[i + 2]);
                                if (hi >= 0 && lo >= 0)
                                {
                                    reply_target_node.push_back(
                                        static_cast<char>((hi << 4) | lo));
                                    i += 2;
                                    continue;
                                }
                            }
                            reply_target_node.push_back(raw_node[i]);
                        }
                    }
                }
            }
            // Fallback: if the current buffer is already a comments node
            // (urn:xmpp:microblog:0:comments/…), reply into that same node —
            // this is a flat thread, not a nested one.  Otherwise construct the
            // standard comments node for this item on the same service.
            if (reply_target_service.empty() || reply_target_node.empty())
            {
                constexpr std::string_view kCommentsPfx = "urn:xmpp:microblog:0:comments/";
                if (pub_node.rfind(kCommentsPfx, 0) == 0)
                {
                    // Already in a comments buffer — post into the same node.
                    reply_target_service = pub_service;
                    reply_target_node    = pub_node;
                }
                else
                {
                    reply_target_service = pub_service;
                    reply_target_node    = fmt::format("urn:xmpp:microblog:0:comments/{}", reply_to_id);
                }
            }
        }
        else
        {
            // /feed post — check for --open flag and -- separator
            if (post_short_form)
            {
                if (post_force_short)
                {
                    // "/feed post -- <body>" — '--' is consumed, rest is body
                    body_raw = argc > 3 ? argv_eol[3] : nullptr;
                }
                else if (argc > 2 && std::string_view(argv[2]) == "--open")
                {
                    // Short form: argv[2] is either --open or the body text
                    access_open = true;
                    body_raw    = argv_eol[3];
                }
                else
                    body_raw = argv_eol[2];
            }
            else
            {
                // Long form: argv[2]=service, argv[3]=node, argv[4]=text (or --open)
                if (argc > 4 && std::string_view(argv[4]) == "--open")
                {
                    access_open = true;
                    body_raw    = argv_eol[5];
                }
                else
                    body_raw = argv_eol[4];
            }
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

        // <content type='text'>body</content> — XEP-0472 §4.2 requires at least
        // one <content> element so receiving clients have something to display.
        {
            xmpp_stanza_t *content_el = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(content_el, "content");
            xmpp_stanza_set_attribute(content_el, "type", "text");
            xmpp_stanza_t *ct = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_text(ct, body.c_str());
            xmpp_stanza_add_child(content_el, ct);
            xmpp_stanza_release(ct);
            xmpp_stanza_add_child(entry, content_el);
            xmpp_stanza_release(content_el);
        }

        // <thr:in-reply-to> for replies (XEP-0472 §4.2)
        if (!reply_to_id.empty())
        {
            const std::string reply_feed_key = fmt::format("{}/{}", pub_service, pub_node);
            const std::string reply_xmpp_uri = fmt::format(
                "xmpp:{}?;node={};item={}", pub_service, pub_node, reply_to_id);
            const std::string reply_atom_id = ptr_account->feed_atom_id_get(reply_feed_key, reply_to_id);
            xmpp_stanza_t *reply_el = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(reply_el, "thr:in-reply-to");
            xmpp_stanza_set_attribute(reply_el, "xmlns:thr",
                                      "http://purl.org/syndication/thread/1.0");
            xmpp_stanza_set_attribute(reply_el, "ref",
                                      reply_atom_id.empty() ? reply_xmpp_uri.c_str()
                                                            : reply_atom_id.c_str());
            xmpp_stanza_set_attribute(reply_el, "href", reply_xmpp_uri.c_str());
            xmpp_stanza_add_child(entry, reply_el);
            xmpp_stanza_release(reply_el);
        }

        // Determine the target service/node and whether this is a comments node.
        // These are needed both for the <link rel='replies'> and for publish-options.
        static constexpr std::string_view k_comments_pfx = "urn:xmpp:microblog:0:comments/";
        const bool is_comments_node = (pub_node.rfind(k_comments_pfx, 0) == 0);
        const std::string target_service = (!reply_target_service.empty())
            ? reply_target_service : pub_service;
        const std::string target_node    = (!reply_target_node.empty())
            ? reply_target_node    : pub_node;

        // <link rel='replies' title='comments' href='xmpp:…'/> for top-level posts (XEP-0277 §3).
        // Only for top-level posts (not replies), and not when publishing to an already-existing
        // comments node.  The comments node URI follows XEP-0277 §3: urn:xmpp:microblog:0:comments/<uuid>.
        if (reply_to_id.empty() && !is_comments_node)
        {
            const std::string comments_node_name =
                fmt::format("urn:xmpp:microblog:0:comments/{}", item_uuid);
            const std::string comments_xmpp_uri = fmt::format(
                "xmpp:{}?;node={}", target_service, comments_node_name);
            xmpp_stanza_t *replies_el = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(replies_el, "link");
            xmpp_stanza_set_attribute(replies_el, "rel",   "replies");
            xmpp_stanza_set_attribute(replies_el, "title", "comments");
            xmpp_stanza_set_attribute(replies_el, "href",  comments_xmpp_uri.c_str());
            xmpp_stanza_add_child(entry, replies_el);
            xmpp_stanza_release(replies_el);
        }

        // <generator> — identify the client (XEP-0277 / RFC 4287)
        {
            xmpp_stanza_t *gen_el = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(gen_el, "generator");
            xmpp_stanza_set_attribute(gen_el, "uri",
                "https://github.com/ekollof/weechat-xmpp-improved");
            xmpp_stanza_set_attribute(gen_el, "version", WEECHAT_XMPP_PLUGIN_VERSION);
            {
                xmpp_stanza_t *gt = xmpp_stanza_new(ptr_account->context);
                xmpp_stanza_set_text(gt, "weechat-xmpp-improved");
                xmpp_stanza_add_child(gen_el, gt);
                xmpp_stanza_release(gt);
            }
            xmpp_stanza_add_child(entry, gen_el);
            xmpp_stanza_release(gen_el);
        }

        // Wrap in <item id='uuid'><entry…/></item>
        xmpp_stanza_t *item_children[2] = {entry, nullptr};
        xmpp_stanza_t *item_el = stanza__iq_pubsub_publish_item(
            ptr_account->context, nullptr, item_children, with_noop(item_uuid.c_str()));

        // Wrap in <publish node='…'><item…/></publish>
        // For replies, publish to the comments node, not the blog node (XEP-0472 §5).
        xmpp_stanza_t *pub_children2[2] = {item_el, nullptr};
        xmpp_stanza_t *publish_el = stanza__iq_pubsub_publish(
            ptr_account->context, nullptr, pub_children2, with_noop(target_node.c_str()));

        // <publish-options>: assert node config so the server auto-creates the node
        // correctly for /feed post.  For /feed reply (or /feed post to a comments
        // node via short form) we publish to a pre-existing comments node whose
        // exact config we don't know; XEP-0060 §7.1.5 requires every asserted
        // field to match the node's current config exactly, so we omit
        // publish-options entirely to avoid precondition-not-met.
        xmpp_stanza_t *pub_opts = nullptr;
        if (subcmd != "reply" && !is_comments_node)
        {
            pub_opts = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(pub_opts, "publish-options");

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
            add_field("pubsub#deliver_payloads", "false");  // XEP-0472 §5.1.1
            add_field("pubsub#type", "urn:xmpp:microblog:0");
            if (access_open)
                add_field("pubsub#access_model", "open");

            xmpp_stanza_add_child(pub_opts, x);
            xmpp_stanza_release(x);
        }

        // Assemble: <pubsub><publish/>[<publish-options/>]</pubsub>
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
                                       target_service.c_str(),
                                       "set");

        ptr_account->connection.send(iq);
        xmpp_stanza_release(iq);

        // Track publish IQ for error reporting
        if (uid_g.ptr)
            ptr_account->pubsub_publish_ids[uid_g.ptr] = {target_service, target_node, item_uuid, buffer};

        if (subcmd == "reply")
            weechat_printf(buffer, "%sPosted reply to %s on %s/%s",
                           weechat_prefix("network"),
                           reply_to_id.c_str(), target_service.c_str(), target_node.c_str());
        else
            weechat_printf(buffer, "%sPosted to %s/%s (id: %s)",
                           weechat_prefix("network"),
                           pub_service.c_str(), pub_node.c_str(), item_uuid.c_str());

        return WEECHAT_RC_OK;
    }

    std::string service_jid = argv[1];

    // Check for --all, --limit N, --before <id>, --latest anywhere in args after service-jid
    bool fetch_all = false;
    bool fetch_latest = false;  // --latest: clear saved RSM cursor and fetch newest page
    int  max_items = 20;   // default: request 20 most-recent items
    std::string node_name;
    std::string before_cursor;  // RSM <before> cursor ("" = latest page)
    for (int i = 2; i < argc; ++i)
    {
        std::string_view arg(argv[i]);
        if (arg == "--all")
            fetch_all = true;
        else if (arg == "--latest")
            fetch_latest = true;
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

    // If no node was given and the service looks like a user JID (contains '@'),
    // default to the PEP microblog node — so "/feed user@example.org" just works.
    if (node_name.empty() && !fetch_all && service_jid.find('@') != std::string::npos)
        node_name = "urn:xmpp:microblog:0";

    if (!node_name.empty())
    {
        // Specific node requested: fetch items directly.
        std::string feed_key = fmt::format("{}/{}", service_jid, node_name);

        // If no --before was given, check LMDB for a persisted RSM cursor so
        // that successive /feed invocations automatically page forward through
        // older items without the user having to copy-paste a cursor id.
        // --latest clears the saved cursor so the newest page is fetched instead.
        std::string cursor_key = fmt::format("pubsub:{}", feed_key);
        if (fetch_latest)
        {
            ptr_account->mam_cursor_clear(cursor_key);
            weechat_printf(buffer,
                           "%sCleared saved cursor for %s/%s; fetching latest page…",
                           weechat_prefix("network"),
                           service_jid.c_str(), node_name.c_str());
        }
        else if (before_cursor.empty())
        {
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
