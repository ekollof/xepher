// XEP-0224: Attention — send <attention> to get a contact's attention
int command__buzz(const void *pointer, void *data,
                  struct t_gui_buffer *buffer, int argc,
                  char **argv, char **argv_eol)
{
    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;

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
    std::string buzz_id = stanza::uuid(ptr_account->context);
    auto buzz_msg = stanza::message().type("chat").to(ptr_channel->id).id(buzz_id);
    buzz_msg.attention();
    ptr_account->connection.send(buzz_msg.build(ptr_account->context).get());

    weechat_printf(buffer, "%sxmpp: buzz sent to %s",
                  weechat_prefix("network"), ptr_channel->id.data());

    return WEECHAT_RC_OK;
}

// XEP-0382: Spoiler Messages — send a message with a spoiler warning
int command__spoiler(const void *pointer, void *data,
                     struct t_gui_buffer *buffer, int argc,
                     char **argv, char **argv_eol)
{
    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;

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
    if (argc >= 3 && std::string_view(argv[1]).back() == ':')
    {
        const char *hint = argv[1];
        // Strip trailing colon
        std::string hint_str(hint, std::string_view(hint).size()-1);
        text = argv_eol[2];

        const char *msg_type = (ptr_channel->type == weechat::channel::chat_type::MUC)
                               ? "groupchat" : "chat";
        std::string spoiler_id = stanza::uuid(ptr_account->context);
        auto spoiler_msg = stanza::message().type(msg_type).to(ptr_channel->id)
                                            .id(spoiler_id).body(text);
        spoiler_msg.spoiler(hint_str);
        spoiler_msg.store();
        ptr_account->connection.send(spoiler_msg.build(ptr_account->context).get());
    }
    else
    {
        const char *msg_type = (ptr_channel->type == weechat::channel::chat_type::MUC)
                               ? "groupchat" : "chat";
        std::string spoiler_id = stanza::uuid(ptr_account->context);
        auto spoiler_msg = stanza::message().type(msg_type).to(ptr_channel->id)
                                            .id(spoiler_id).body(text);
        spoiler_msg.spoiler();
        spoiler_msg.store();
        ptr_account->connection.send(spoiler_msg.build(ptr_account->context).get());
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

        std::string query_id = stanza::uuid(ptr_account->context);

        weechat::account::adhoc_query_info info;
        info.target_jid = target_jid;
        info.buffer = buffer;
        info.is_list = true;
        info.picker = *p_holder;
        ptr_account->adhoc_queries[query_id] = info;

        ptr_account->connection.send(
            stanza::iq().type("get").id(query_id).to(target_jid)
            .xep0030()
            .query_items(stanza::xep0030::query_items("http://jabber.org/protocol/commands"))
            .build(ptr_account->context).get());

        return WEECHAT_RC_OK;
    }

    const char *node = argv[2];

    if (argc == 3)
    {
        // Execute a command (first step)
        std::string exec_id = stanza::uuid(ptr_account->context);

        weechat::account::adhoc_query_info info;
        info.target_jid = target_jid;
        info.buffer = buffer;
        info.is_list = false;
        info.node = node;
        ptr_account->adhoc_queries[exec_id] = info;

        struct adhoc_exec_spec : stanza::spec {
            adhoc_exec_spec(const std::string &id, const char *to,
                            const char *n) : spec("iq") {
                attr("type", "set");
                attr("id", id);
                attr("to", to);
                struct command_spec : stanza::spec {
                    command_spec(const char *n) : spec("command") {
                        attr("xmlns", "http://jabber.org/protocol/commands");
                        attr("node", n);
                        attr("action", "execute");
                    }
                } cmd(n);
                child(cmd);
            }
        } adhoc_exec_iq(exec_id, target_jid, node);
        ptr_account->connection.send(adhoc_exec_iq.build(ptr_account->context).get());

        weechat_printf(buffer, "%sxmpp: executing command %s on %s…",
                      weechat_prefix("network"), node, target_jid);
        return WEECHAT_RC_OK;
    }

    // argc >= 4: submit a form step
    // argv[3] = sessionid, argv[4..] = field=value pairs
    const char *session_id = argv[3];
    std::string submit_id = stanza::uuid(ptr_account->context);

    weechat::account::adhoc_query_info info;
    info.target_jid = target_jid;
    info.buffer = buffer;
    info.is_list = false;
    info.node = node;
    info.session_id = session_id;
    ptr_account->adhoc_queries[submit_id] = info;

    // Build IQ with command + x:data form using RAII shared_ptr stanzas
    {
        xmpp_ctx_t *ctx = ptr_account->context;
        std::shared_ptr<xmpp_stanza_t> iq {xmpp_iq_new(ctx, "set", submit_id.c_str()),
                                            xmpp_stanza_release};
        xmpp_stanza_set_to(iq.get(), target_jid);
        std::shared_ptr<xmpp_stanza_t> command {xmpp_stanza_new(ctx), xmpp_stanza_release};
        xmpp_stanza_set_name(command.get(), "command");
        xmpp_stanza_set_ns(command.get(), "http://jabber.org/protocol/commands");
        xmpp_stanza_set_attribute(command.get(), "node", node);
        xmpp_stanza_set_attribute(command.get(), "sessionid", session_id);
        xmpp_stanza_set_attribute(command.get(), "action", "execute");
        std::shared_ptr<xmpp_stanza_t> x_form {xmpp_stanza_new(ctx), xmpp_stanza_release};
        xmpp_stanza_set_name(x_form.get(), "x");
        xmpp_stanza_set_ns(x_form.get(), "jabber:x:data");
        xmpp_stanza_set_attribute(x_form.get(), "type", "submit");
        for (int i = 4; i < argc; i++)
        {
            std::string_view arg_sv(argv[i]);
            auto eq_pos = arg_sv.find('=');
            if (eq_pos == std::string_view::npos) continue;
            std::string field_var(arg_sv.substr(0, eq_pos));
            const char *field_val = argv[i] + eq_pos + 1;
            std::shared_ptr<xmpp_stanza_t> field {xmpp_stanza_new(ctx), xmpp_stanza_release};
            xmpp_stanza_set_name(field.get(), "field");
            xmpp_stanza_set_attribute(field.get(), "var", field_var.c_str());
            std::shared_ptr<xmpp_stanza_t> value {xmpp_stanza_new(ctx), xmpp_stanza_release};
            xmpp_stanza_set_name(value.get(), "value");
            std::shared_ptr<xmpp_stanza_t> vtext {xmpp_stanza_new(ctx), xmpp_stanza_release};
            xmpp_stanza_set_text(vtext.get(), field_val);
            xmpp_stanza_add_child(value.get(), vtext.get());
            xmpp_stanza_add_child(field.get(), value.get());
            xmpp_stanza_add_child(x_form.get(), field.get());
        }
        xmpp_stanza_add_child(command.get(), x_form.get());
        xmpp_stanza_add_child(iq.get(), command.get());
        ptr_account->connection.send(iq.get());
    }

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
    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;

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
    const char *reason = argc > 2 ? argv_eol[2] : nullptr;

    /* IQ set to room: <query xmlns='…muc#admin'><item nick='NICK' role='none'/></query> */
    stanza::xep0045admin::item_by_nick kick_item(nick, "none");
    if (reason)
        kick_item.reason(reason);
    stanza::xep0045admin::query kick_query;
    kick_query.item(kick_item);
    std::string kick_id = stanza::uuid(ptr_account->context);
    auto kick_iq = stanza::iq().type("set").to(ptr_channel->id).id(kick_id);
    kick_iq.muc_admin(kick_query);
    ptr_account->connection.send(kick_iq.build(ptr_account->context).get());

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
    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;

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
    const char *reason = argc > 2 ? argv_eol[2] : nullptr;

    /* IQ set to room: <query xmlns='…muc#admin'><item jid='JID' affiliation='outcast'/></query> */
    stanza::xep0045admin::item_by_jid ban_item(target_jid, "outcast");
    if (reason)
        ban_item.reason(reason);
    stanza::xep0045admin::query ban_query;
    ban_query.item(ban_item);
    std::string ban_id = stanza::uuid(ptr_account->context);
    auto ban_iq = stanza::iq().type("set").to(ptr_channel->id).id(ban_id);
    ban_iq.muc_admin(ban_query);
    ptr_account->connection.send(ban_iq.build(ptr_account->context).get());

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
    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;

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

    auto topic_msg = stanza::message().type("groupchat").to(ptr_channel->id);
    if (argc > 1)
        topic_msg.subject(new_topic);
    else
        topic_msg.subject();
    ptr_account->connection.send(topic_msg.build(ptr_account->context).get());

    return WEECHAT_RC_OK;
}

int command__muc_nick(const void *pointer, void *data,
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
    ::jid ch_jid(nullptr, ptr_channel->id);
    std::string new_full_jid = fmt::format("{}@{}/{}", ch_jid.local, ch_jid.domain, new_nick);

    auto nick_pres = stanza::presence().from(ptr_account->jid()).to(new_full_jid);
    ptr_account->connection.send(nick_pres.build(ptr_account->context).get());

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
            std::string uid = stanza::uuid(ptr_account->context);
            ptr_account->pubsub_subscriptions_queries[uid] = svc;
            ptr_account->connection.send(
                stanza::iq().type("get").id(uid).to(svc)
                .xep0060().pubsub(stanza::xep0060::pubsub()
                    .subscriptions(stanza::xep0060::subscriptions()))
                .build(ptr_account->context).get());
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
                std::string uid = stanza::uuid(ptr_account->context);
                ptr_account->pubsub_disco_queries[uid] = svc;
                ptr_account->connection.send(
                    stanza::iq().type("get").id(uid).to(svc)
                    .xep0030().query_items()
                    .build(ptr_account->context).get());
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
                std::string uid = stanza::uuid(ptr_account->context);
                ptr_account->pubsub_subscriptions_queries[uid] = svc;
                ptr_account->connection.send(
                    stanza::iq().type("get").id(uid).to(svc)
                    .xep0060().pubsub(stanza::xep0060::pubsub()
                        .subscriptions(stanza::xep0060::subscriptions()))
                    .build(ptr_account->context).get());
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

        const std::string my_jid = ptr_account->jid();

        const std::string uid = stanza::uuid(ptr_account->context);

        if (subcmd == "subscribe")
        {
            // <iq type='set' to='service'>
            //   <pubsub xmlns='…'><subscribe node='…' jid='…'/></pubsub>
            // </iq>
            stanza::xep0060::subscribe sub_el(node, my_jid);
            stanza::xep0060::pubsub ps;
            ps.subscribe(sub_el);
            stanza::iq iq_s;
            iq_s.id(uid).from(my_jid).to(svc).type("set");
            iq_s.pubsub(ps);

            ptr_account->pubsub_subscribe_queries[uid] = {feed_key, buffer};
            ptr_account->connection.send(iq_s.build(ptr_account->context).get());

            weechat_printf(buffer, "%sSubscribing to %s/%s…",
                           weechat_prefix("network"), svc.c_str(), node.c_str());
        }
        else  // unsubscribe
        {
            // <iq type='set' to='service'>
            //   <pubsub xmlns='…'><unsubscribe node='…' jid='…'/></pubsub>
            // </iq>
            stanza::xep0060::unsubscribe unsub_el(node, my_jid);
            stanza::xep0060::pubsub ps;
            ps.unsubscribe(unsub_el);
            stanza::iq iq_s;
            iq_s.id(uid).from(my_jid).to(svc).type("set");
            iq_s.pubsub(ps);

            ptr_account->pubsub_unsubscribe_queries[uid] = {feed_key, buffer};
            ptr_account->connection.send(iq_s.build(ptr_account->context).get());

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
        std::string uid = stanza::uuid(ptr_account->context);
        ptr_account->pubsub_subscriptions_queries[uid] = svc;
        ptr_account->connection.send(
            stanza::iq().type("get").id(uid).to(svc)
            .xep0060().pubsub(stanza::xep0060::pubsub()
                .subscriptions(stanza::xep0060::subscriptions()))
            .build(ptr_account->context).get());

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

        const std::string item_uuid = stanza::uuid(ptr_account->context);

        std::time_t now_ts = std::time(nullptr);
        const std::string ts_buf = fmt::format("{:%Y-%m-%dT%H:%M:%SZ}", fmt::gmtime(now_ts));

        const std::string acct_domain = ::jid(nullptr, ptr_account->jid()).domain;
        const std::string date_buf = fmt::format("{:%Y-%m-%d}", fmt::gmtime(now_ts));
        const std::string atom_id = fmt::format("tag:{},{};posts/{}",
                                                acct_domain.empty() ? "xmpp" : acct_domain,
                                                date_buf, item_uuid);

        // Helper: build <tag>text</tag> with RAII
        auto make_text_el = [&](const char *tag, const char *text)
            -> std::shared_ptr<xmpp_stanza_t> {
            std::shared_ptr<xmpp_stanza_t> el {xmpp_stanza_new(ptr_account->context),
                                                xmpp_stanza_release};
            xmpp_stanza_set_name(el.get(), tag);
            std::shared_ptr<xmpp_stanza_t> t {xmpp_stanza_new(ptr_account->context),
                                               xmpp_stanza_release};
            xmpp_stanza_set_text(t.get(), text);
            xmpp_stanza_add_child(el.get(), t.get());
            return el;
        };

        std::shared_ptr<xmpp_stanza_t> entry {xmpp_stanza_new(ptr_account->context),
                                               xmpp_stanza_release};
        xmpp_stanza_set_name(entry.get(), "entry");
        xmpp_stanza_set_ns(entry.get(), "http://www.w3.org/2005/Atom");

        // <title>Shared: item-id [— comment]</title>
        {
            std::string repeat_title = rep_comment.empty()
                ? fmt::format("Shared: {}", rep_item_id)
                : fmt::format("Shared: {} — {}", rep_item_id, rep_comment);
            std::shared_ptr<xmpp_stanza_t> title_el {xmpp_stanza_new(ptr_account->context),
                                                       xmpp_stanza_release};
            xmpp_stanza_set_name(title_el.get(), "title");
            xmpp_stanza_set_attribute(title_el.get(), "type", "text");
            std::shared_ptr<xmpp_stanza_t> t {xmpp_stanza_new(ptr_account->context),
                                               xmpp_stanza_release};
            xmpp_stanza_set_text(t.get(), repeat_title.c_str());
            xmpp_stanza_add_child(title_el.get(), t.get());
            xmpp_stanza_add_child(entry.get(), title_el.get());
        }

        xmpp_stanza_add_child(entry.get(), make_text_el("id", atom_id.c_str()).get());
        xmpp_stanza_add_child(entry.get(), make_text_el("published", ts_buf.c_str()).get());
        xmpp_stanza_add_child(entry.get(), make_text_el("updated", ts_buf.c_str()).get());

        // <author>
        {
            const std::string my_bare = ptr_account->jid();
            const std::string xmpp_uri = fmt::format("xmpp:{}", my_bare);
            std::shared_ptr<xmpp_stanza_t> author_el {xmpp_stanza_new(ptr_account->context),
                                                        xmpp_stanza_release};
            xmpp_stanza_set_name(author_el.get(), "author");
            xmpp_stanza_add_child(author_el.get(), make_text_el("name", my_bare.c_str()).get());
            xmpp_stanza_add_child(author_el.get(), make_text_el("uri", xmpp_uri.c_str()).get());
            xmpp_stanza_add_child(entry.get(), author_el.get());
        }

        // <link rel='via' href='xmpp:…' ref='atom-id'/>  (XEP-0472 §4.5 boost/repeat)
        {
            const std::string rep_feed_key = fmt::format("{}/{}", rep_service, rep_node);
            const std::string rep_atom_id  = ptr_account->feed_atom_id_get(rep_feed_key, rep_item_id);
            std::shared_ptr<xmpp_stanza_t> via_el {xmpp_stanza_new(ptr_account->context),
                                                    xmpp_stanza_release};
            xmpp_stanza_set_name(via_el.get(), "link");
            xmpp_stanza_set_attribute(via_el.get(), "rel",  "via");
            xmpp_stanza_set_attribute(via_el.get(), "href", via_uri.c_str());
            if (!rep_atom_id.empty())
                xmpp_stanza_set_attribute(via_el.get(), "ref", rep_atom_id.c_str());
            xmpp_stanza_add_child(entry.get(), via_el.get());
        }

        // Optional <content>comment</content>
        if (!rep_comment.empty())
        {
            std::shared_ptr<xmpp_stanza_t> content_el {xmpp_stanza_new(ptr_account->context),
                                                         xmpp_stanza_release};
            xmpp_stanza_set_name(content_el.get(), "content");
            xmpp_stanza_set_attribute(content_el.get(), "type", "text");
            std::shared_ptr<xmpp_stanza_t> t {xmpp_stanza_new(ptr_account->context),
                                               xmpp_stanza_release};
            xmpp_stanza_set_text(t.get(), rep_comment.c_str());
            xmpp_stanza_add_child(content_el.get(), t.get());
            xmpp_stanza_add_child(entry.get(), content_el.get());
        }

        // <generator>
        {
            std::shared_ptr<xmpp_stanza_t> gen_el {xmpp_stanza_new(ptr_account->context),
                                                     xmpp_stanza_release};
            xmpp_stanza_set_name(gen_el.get(), "generator");
            xmpp_stanza_set_attribute(gen_el.get(), "uri",
                "https://github.com/ekollof/xepher");
            xmpp_stanza_set_attribute(gen_el.get(), "version", WEECHAT_XMPP_PLUGIN_VERSION);
            xmpp_stanza_add_child(gen_el.get(), make_text_el("", "Xepher").get());
            xmpp_stanza_add_child(entry.get(), gen_el.get());
        }

        // Publish the repeat entry via PubSub
        std::string uid_r = stanza::uuid(ptr_account->context);
        ptr_account->pubsub_publish_ids[uid_r] = {rep_service, rep_node, item_uuid, buffer};

        auto entry_item = stanza::xep0060::item().id(item_uuid);
        // entry_item.payload() needs the built entry stanza — use raw approach for payload
        auto repeat_iq_built = stanza::iq().type("set").id(uid_r)
            .from(ptr_account->jid()).to(rep_service)
            .xep0060().pubsub(stanza::xep0060::pubsub()
                .publish(stanza::xep0060::publish(rep_node)))
            .build(ptr_account->context);
        // Attach the entry to the item inside the publish
        {
            xmpp_stanza_t *ps = xmpp_stanza_get_child_by_name(repeat_iq_built.get(), "pubsub");
            xmpp_stanza_t *pub = ps ? xmpp_stanza_get_child_by_name(ps, "publish") : nullptr;
            if (pub)
            {
                std::shared_ptr<xmpp_stanza_t> item_s {xmpp_stanza_new(ptr_account->context),
                                                         xmpp_stanza_release};
                xmpp_stanza_set_name(item_s.get(), "item");
                xmpp_stanza_set_id(item_s.get(), item_uuid.c_str());
                xmpp_stanza_add_child(item_s.get(), entry.get());
                xmpp_stanza_add_child(pub, item_s.get());
            }
        }
        ptr_account->connection.send(repeat_iq_built.get());

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
        ptr_account->feed_open_register(comments_feed_key);

        {
            std::string uid = stanza::uuid(ptr_account->context);
            stanza::xep0060::items its(comments_node);
            its.max_items(20);
            stanza::xep0060::pubsub ps;
            ps.items(its);
            stanza::iq iq_s;
            iq_s.id(uid).from(ptr_account->jid()).to(comments_service).type("get");
            iq_s.pubsub(ps);
            ptr_account->pubsub_fetch_ids[uid] = {comments_service, comments_node, "", 20};
            ptr_account->connection.send(iq_s.build(ptr_account->context).get());
        }

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

        // Short-form detection for /feed retract: "/feed retract #N"
        // (no explicit service/node — inferred from the current FEED buffer).
        bool retract_short_form = (subcmd == "retract")
            && (argc >= 3)
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
                     : subcmd == "retract" ? (retract_short_form ? 3 : 5)
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
                               _("%s%s: usage: /feed retract <service-jid> <node> <item-id>\n"
                                 "           or: /feed retract #N  (from a feed buffer)"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
            return WEECHAT_RC_OK;
        }

        std::string pub_service;
        std::string pub_node;

        if (reply_short_form || post_short_form || retract_short_form)
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
            // ── /feed retract #N  (short form from a feed buffer)  ─────────
            const std::string pub_feed_key = fmt::format("{}/{}", pub_service, pub_node);
            std::string retract_id;
            if (retract_short_form)
            {
                retract_id = ptr_account->feed_alias_resolve(pub_feed_key, argv[2]);
                if (retract_id.empty())
                {
                    weechat_printf(buffer, _("%s%s: unknown alias %s in feed %s"),
                                   weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                                   argv[2], pub_feed_key.c_str());
                    return WEECHAT_RC_OK;
                }
            }
            else
            {
                // Long form: argv[4] may be a raw UUID or a #N alias.
                std::string_view arg4(argv[4]);
                if (!arg4.empty() && (arg4[0] == '#' || std::isdigit((unsigned char)arg4[0])))
                    retract_id = ptr_account->feed_alias_resolve(pub_feed_key, arg4);
                if (retract_id.empty())
                    retract_id = std::string(arg4);
            }

            std::string retract_uid = stanza::uuid(ptr_account->context);
            // Track the IQ so the error handler can report server-side failures.
            // is_retract=true makes trigger_publish_refetch do a full node re-fetch
            // on success (the retracted item is gone; fetching it by ID returns nothing).
            ptr_account->pubsub_publish_ids[retract_uid] = {
                pub_service, pub_node, retract_id, buffer, /*is_retract=*/true};

            ptr_account->connection.send(
                stanza::iq().type("set").id(retract_uid)
                .to(pub_service).from(ptr_account->jid())
                .xep0060().pubsub(stanza::xep0060::pubsub()
                    .retract(stanza::xep0060::retract(pub_node)
                        .notify(true)
                        .item(stanza::xep0060::item().id(retract_id))))
                .build(ptr_account->context).get());

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

            // When posting from a comments buffer, treat it as a reply to the
            // parent item so that <thr:in-reply-to> threading is emitted and the
            // post is routed back into the same comments node.
            // The comments node name is "urn:xmpp:microblog:0:comments/<parent-id>".
            constexpr std::string_view kCommentsPfx = "urn:xmpp:microblog:0:comments/";
            if (reply_to_id.empty() && pub_node.rfind(kCommentsPfx, 0) == 0)
            {
                reply_to_id          = pub_node.substr(kCommentsPfx.size());
                reply_target_service = pub_service;
                reply_target_node    = pub_node;
            }
        }

         if (!body_raw || !*body_raw)
         {
             weechat_printf(buffer, _("%s%s: post text must not be empty"),
                            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
             return WEECHAT_RC_OK;
         }

         // Parse optional --title <value> -- prefix from body_raw.
         // feed_compose.py emits: --title My Title -- Body text
         // The title is everything between "--title " and the first " -- ".
         // Falls back to the first line of the body when absent.
         std::string post_title;
         std::string_view body_sv(body_raw);
         constexpr std::string_view kTitleFlag = "--title ";
         if (body_sv.substr(0, kTitleFlag.size()) == kTitleFlag)
         {
             auto rest      = body_sv.substr(kTitleFlag.size());
             auto sep_pos   = rest.find(" -- ");
             if (sep_pos != std::string_view::npos)
             {
                 post_title = std::string(rest.substr(0, sep_pos));
                 body_raw   = body_raw
                              + kTitleFlag.size()  // skip "--title "
                              + sep_pos            // skip title value
                              + 4;                 // skip " -- "
             }
         }
         const std::string body(body_raw);

         // Parse template tags ({{ embed|attach|video "file" [alt="..."] }})
         // Embed tags are only supported for top-level microblog posts, not
         // comments (urn:xmpp:microblog:0:comments/<uuid> nodes).
         constexpr std::string_view kCommentsPfx = "urn:xmpp:microblog:0:comments/";
         const bool is_comments_post = (pub_node.rfind(kCommentsPfx, 0) == 0);
         auto embed_tags = xepher::parse_embed_tags(body);
         if (is_comments_post && !embed_tags.empty())
         {
             weechat_printf(buffer,
                 "%s%s: embed tags ({{ embed/attach/video }}) are not supported in comments",
                 weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
             return WEECHAT_RC_OK;
         }

          // post_title is only set when the user explicitly provided --title (or
          // YAML frontmatter via feed_compose.py).  We intentionally do NOT fall
          // back to the first line of the body here: Movim and other microblog
          // clients treat <title> as a headline separate from the post body, so
          // synthesising one from the body causes the full text to appear as a
          // title in the Movim web UI.  When post_title is empty we simply omit
          // <title> from the Atom entry, letting <content> carry everything.

         // Generate a stable UUID for the item
        const std::string item_uuid = stanza::uuid(ptr_account->context);

        // ISO-8601 timestamp (UTC)
        std::time_t now_ts = std::time(nullptr);
        const std::string ts_buf = fmt::format("{:%Y-%m-%dT%H:%M:%SZ}", fmt::gmtime(now_ts));

        // Atom tag URI: tag:<domain>,<date>:posts/<uuid>
        const std::string acct_domain = ::jid(nullptr, ptr_account->jid()).domain;
        const std::string date_buf = fmt::format("{:%Y-%m-%d}", fmt::gmtime(now_ts));
        const std::string atom_id = fmt::format("tag:{},{};posts/{}",
                                                acct_domain.empty() ? "xmpp" : acct_domain,
                                                date_buf, item_uuid);

        // Build a pending_feed_post struct with all needed metadata.
        // This is used both for the embed-upload path (returned early, async)
        // and for the immediate publish path (no embed tags).
        xepher::pending_feed_post pfp;
        pfp.account_name       = ptr_account->name;
        pfp.service            = pub_service;
        pfp.node               = pub_node;
        pfp.is_reply           = !reply_to_id.empty();
        pfp.reply_to_id        = reply_to_id;
        pfp.reply_target_service = reply_target_service;
        pfp.reply_target_node  = reply_target_node;
        pfp.title              = post_title;
        pfp.access_open        = access_open;
        pfp.item_uuid          = item_uuid;
        pfp.atom_id            = atom_id;
        pfp.timestamp          = ts_buf;
        pfp.raw_body_template  = body;
        pfp.embeds             = std::move(embed_tags);
        pfp.buffer             = buffer;

        if (!pfp.embeds.empty())
        {
            // Phase 1: validate files, check upload service, kick off first upload.
            if (ptr_account->upload_service.empty())
            {
                weechat_printf(buffer,
                    "%s%s: embed tags require an upload service — none discovered yet (try reconnecting)",
                    weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                return WEECHAT_RC_OK;
            }

            // Resolve filepaths (relative paths resolved against $HOME)
            for (auto &emb : pfp.embeds)
            {
                if (emb.filepath.empty())
                {
                    // Try the filename as-is first
                    FILE *f = fopen(emb.filename.c_str(), "rb");
                    if (f) { fclose(f); emb.filepath = emb.filename; }
                    else
                    {
                        // Try relative to $HOME
                        const char *home = getenv("HOME");
                        if (home)
                        {
                            std::string candidate = fmt::format("{}/{}", home, emb.filename);
                            f = fopen(candidate.c_str(), "rb");
                            if (f) { fclose(f); emb.filepath = std::move(candidate); }
                        }
                    }
                }
                if (emb.filepath.empty())
                {
                    weechat_printf(buffer,
                        "%s%s: embed file not found: %s",
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                        emb.filename.c_str());
                    // Save draft so user doesn't lose the post
                    std::string draft_path = ptr_account->save_feed_draft(pfp);
                    if (!draft_path.empty())
                        weechat_printf(buffer,
                            "%sDraft saved: %s",
                            weechat_prefix("network"), draft_path.c_str());
                    return WEECHAT_RC_OK;
                }
            }

            // Kick off the first upload slot request (Phase 1 → Phase 2 chain)
            auto &first_emb = pfp.embeds[0];

            // Determine content-type and file size for first embed
            std::string ct = "application/octet-stream";
            {
                size_t dp = first_emb.filename.find_last_of('.');
                if (dp != std::string::npos)
                {
                    std::string ext = first_emb.filename.substr(dp + 1);
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == "jpg" || ext == "jpeg") ct = "image/jpeg";
                    else if (ext == "png")  ct = "image/png";
                    else if (ext == "gif")  ct = "image/gif";
                    else if (ext == "webp") ct = "image/webp";
                    else if (ext == "mp4")  ct = "video/mp4";
                    else if (ext == "webm") ct = "video/webm";
                    else if (ext == "pdf")  ct = "application/pdf";
                    else if (ext == "txt")  ct = "text/plain";
                }
            }
            first_emb.mime = ct;

            FILE *ff = fopen(first_emb.filepath.c_str(), "rb");
            if (!ff)
            {
                weechat_printf(buffer,
                    "%s%s: cannot open embed file: %s",
                    weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                    first_emb.filepath.c_str());
                std::string draft_path = ptr_account->save_feed_draft(pfp);
                if (!draft_path.empty())
                    weechat_printf(buffer, "%sDraft saved: %s",
                        weechat_prefix("network"), draft_path.c_str());
                return WEECHAT_RC_OK;
            }
            fseek(ff, 0, SEEK_END);
            long fsz = ftell(ff);
            fclose(ff);
            if (fsz < 0) fsz = 0;
            first_emb.size = static_cast<uint64_t>(fsz);

            // Sanitize filename for the upload server
            std::string san_name;
            for (char c : first_emb.filename)
            {
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')
                    san_name += c;
                else
                    san_name += '-';
            }
            // Collapse repeated dashes
            {
                size_t p2 = 0;
                while ((p2 = san_name.find("--", p2)) != std::string::npos)
                    san_name.erase(p2, 1);
            }
            while (!san_name.empty() && san_name.front() == '-') san_name.erase(0, 1);
            while (!san_name.empty() && san_name.back() == '-') san_name.pop_back();
            if (san_name.empty()) san_name = "file";

            const std::string slot_id = stanza::uuid(ptr_account->context);

            // Store the upload request (keyed by slot IQ id)
            ptr_account->upload_requests[slot_id] = {
                slot_id,
                first_emb.filepath,
                san_name,
                "",   // channel_id unused for feed posts
                ct,
                static_cast<size_t>(fsz),
                ""    // sha256 filled during upload
            };

            // Store pending post (keyed by same slot_id)
            ptr_account->pending_feed_posts[slot_id] = std::move(pfp);

            // Build and send XEP-0363 slot request IQ
            {
                std::size_t sz_val = static_cast<std::size_t>(fsz);
                struct slot_req_spec : stanza::spec {
                    slot_req_spec(const std::string &id, const std::string &to,
                                  const std::string &fname, std::size_t sz,
                                  const std::string &ct) : spec("iq") {
                        attr("type", "get");
                        attr("id", id);
                        attr("to", to);
                        struct request_spec : stanza::spec {
                            request_spec(const std::string &fn, std::size_t sz,
                                         const std::string &ct) : spec("request") {
                                attr("xmlns", "urn:xmpp:http:upload:0");
                                attr("filename", fn);
                                attr("size", fmt::format("{}", sz));
                                if (!ct.empty()) attr("content-type", ct);
                            }
                        } req(fname, sz, ct);
                        child(req);
                    }
                } slot_iq_spec(slot_id, ptr_account->upload_service, san_name, sz_val, ct);
                ptr_account->connection.send(slot_iq_spec.build(ptr_account->context).get());
            }

            weechat_printf(buffer,
                "%sUploading %zu embed file(s) before posting…",
                weechat_prefix("network"), ptr_account->pending_feed_posts[slot_id].embeds.size());
            return WEECHAT_RC_OK;
        }

        // No embed tags — publish immediately.
        ptr_account->build_and_publish_post(pfp);

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
        ptr_account->feed_open_register(feed_key);

        weechat_printf(buffer, "%sFetching PubSub feed %s from %s (XEP-0060)…",
                       weechat_prefix("network"), node_name.c_str(), service_jid.c_str());

        // Build: <pubsub><items node=".." max_items="N"/><set xmlns=RSM><max>N</max><before>cursor</before></set></pubsub>
        {
            std::string uid = stanza::uuid(ptr_account->context);
            stanza::xep0060::items its(node_name);
            its.max_items(max_items);
            // RSM <set>: <before/> = latest page, <before>cursor</before> = specific page
            stanza::xep0059::set rsm;
            rsm.max(max_items)
               .before(before_cursor.empty()
                       ? std::nullopt
                       : std::optional<std::string>(before_cursor));
            stanza::xep0060::pubsub ps;
            ps.items(its).rsm(rsm);
            stanza::iq iq_s;
            iq_s.id(uid).from(ptr_account->jid()).to(service_jid).type("get");
            iq_s.pubsub(ps);
            ptr_account->pubsub_fetch_ids[uid] = {service_jid, node_name, before_cursor, max_items};
            ptr_account->connection.send(iq_s.build(ptr_account->context).get());
        }
    }
    else if (fetch_all)
    {
        // --all: discover all nodes via disco#items and fetch each one.
        weechat_printf(buffer, "%sDiscovering all PubSub nodes on %s…",
                       weechat_prefix("network"), service_jid.c_str());

        std::string uid = stanza::uuid(ptr_account->context);
        ptr_account->pubsub_disco_queries[uid] = service_jid;
        ptr_account->connection.send(
            stanza::iq().type("get").id(uid).to(service_jid)
            .xep0030().query_items()
            .build(ptr_account->context).get());
    }
    else
    {
        // Default: query subscriptions and fetch only subscribed nodes.
        weechat_printf(buffer, "%sFetching subscribed PubSub feeds from %s…",
                       weechat_prefix("network"), service_jid.c_str());

        std::string uid = stanza::uuid(ptr_account->context);
        ptr_account->pubsub_subscriptions_queries[uid] = service_jid;
        ptr_account->connection.send(
            stanza::iq().type("get").id(uid).to(service_jid)
            .xep0060().pubsub(stanza::xep0060::pubsub()
                .subscriptions(stanza::xep0060::subscriptions()))
            .build(ptr_account->context).get());
    }

    return WEECHAT_RC_OK;
}
