int command__block(const void *pointer, void *data,
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

    auto ui = weechat::UiPort::for_buffer(buffer);

    if (!ptr_account->connected())
    {
        ui->printf_error("xmpp: you are not connected to server");
        return WEECHAT_RC_OK;
    }

    if (argc < 2)
    {
        ui->printf_error(fmt::format("{}: missing JID argument", argv[0]));
        return WEECHAT_RC_OK;
    }

    // Block the specified JID(s)
    const char **jids = (const char **)&argv[1];
    int count = argc - 1;

    stanza::xep0191::block blk;
    for (int i = 0; i < count; i++)
    {
        blk.item(jids[i]);
        ui->printf_network(fmt::format("Blocking {}...", jids[i]));
    }

    std::string id = stanza::uuid(ptr_account->context);
    auto iq_s = stanza::iq().type("set").id(id);
    iq_s.block(blk);
    ptr_account->connection.send(iq_s.build(ptr_account->context).get());

    return WEECHAT_RC_OK;
}

int command__unblock(const void *pointer, void *data,
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

    auto ui = weechat::UiPort::for_buffer(buffer);

    if (!ptr_account->connected())
    {
        ui->printf_error("xmpp: you are not connected to server");
        return WEECHAT_RC_OK;
    }

    stanza::xep0191::unblock ublk;

    if (argc > 1)
    {
        // Unblock specific JID(s)
        const char **jids = (const char **)&argv[1];
        int count = argc - 1;

        for (int i = 0; i < count; i++)
        {
            ublk.item(jids[i]);
        ui->printf_network(fmt::format("Unblocking {}...", jids[i]));
        }
    }
    else
    {
        // Empty unblock = unblock all
        ui->printf_network("Unblocking all JIDs...");
    }

    std::string id = stanza::uuid(ptr_account->context);
    auto iq_s = stanza::iq().type("set").id(id);
    iq_s.unblock(ublk);
    ptr_account->connection.send(iq_s.build(ptr_account->context).get());

    return WEECHAT_RC_OK;
}

int command__blocklist(const void *pointer, void *data,
                      struct t_gui_buffer *buffer, int argc,
                      char **argv, char **argv_eol)
{
    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;

    (void) pointer;
    (void) data;
    (void) argv;
    (void) argv_eol;
    (void) argc;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    auto ui = weechat::UiPort::for_buffer(buffer);

    if (!ptr_account->connected())
    {
        ui->printf_error("xmpp: you are not connected to server");
        return WEECHAT_RC_OK;
    }

    // If a picker is already open for this account, don't open another.
    if (ptr_account->blocklist_picker)
    {
        ui->printf_network("xmpp: blocklist picker already open");
        return WEECHAT_RC_OK;
    }

    // Open picker with no entries yet; the IQ result handler will populate it.
    using picker_t = weechat::ui::picker<std::string>;
    weechat::account *acct = ptr_account;  // capture for lambdas

    auto p = std::make_unique<picker_t>(
        "xmpp.picker.blocklist",
        "Blocked JIDs  (XEP-0191)  — select to unblock",
        std::vector<picker_t::entry>{},  // populated async by IQ handler
        [acct](std::string_view jid) {
            // on_select: send unblock IQ for selected JID
            stanza::xep0191::unblock ublk;
            ublk.item(jid);
            std::string id = stanza::uuid(acct->context);
            auto iq_s = stanza::iq().type("set").id(id);
            iq_s.unblock(ublk);
            acct->connection.send(iq_s.build(acct->context).get());

        weechat::UiPort::for_buffer(acct->buffer)->printf_network(fmt::format("Unblocking {}…", std::string(jid).c_str()));
        },
        [acct]() {
            // on_close: clear the non-owning pointer in account
            acct->blocklist_picker = nullptr;
        },
        buffer);
    if (!*p) return WEECHAT_RC_ERROR;

    // Store non-owning pointer so the IQ handler can call add_entry().
    ptr_account->blocklist_picker = p.release();

    // Request the block list from the server.
    std::string bl_id = stanza::uuid(ptr_account->context);
    auto bl_iq = stanza::iq().type("get").id(bl_id);
    bl_iq.blocklist();
    ptr_account->connection.send(bl_iq.build(ptr_account->context).get());

    return WEECHAT_RC_OK;
}

int command__disco(const void *pointer, void *data,
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

    auto ui = weechat::UiPort::for_buffer(buffer);

    if (!ptr_account->connected())
    {
        ui->printf_error(fmt::format(fmt::runtime(_("{}: you are not connected to server")), WEECHAT_XMPP_PLUGIN_NAME));
        return WEECHAT_RC_OK;
    }

    if (argc > 1 && weechat_strcasecmp(argv[1], "summary") == 0)
    {
        const bool refresh = argc > 2 && weechat_strcasecmp(argv[2], "refresh") == 0;
        if (refresh)
        {
            ptr_account->send_server_disco_summary_refresh();
            ui->printf_network("Refreshing server discovery (domain disco#info + disco#items)...");
        }

        const auto caps = ptr_account->gather_server_capabilities();
        for (const std::string &line : ::xmpp::format_disco_summary(caps))
            ui->printf(line);
        return WEECHAT_RC_OK;
    }

    // Subcommand: /disco items [jid]
    bool do_items = argc > 1 && weechat_strcasecmp(argv[1], "items") == 0;
    int jid_arg = do_items ? 2 : 1;  // argv index of optional jid

    std::string target_domain;
    const char *target = nullptr;
    if (argc > jid_arg)
        target = argv[jid_arg];
    else
    {
        target_domain = jid(nullptr, ptr_account->jid()).domain;
        target = target_domain.c_str();
    }

    std::string id = stanza::uuid(ptr_account->context);

    auto iq_s = stanza::iq().type("get").id(id).to(target);
    if (do_items)
    {
        static_cast<stanza::xep0030::iq&>(iq_s).query_items();
        ptr_account->user_disco_items_queries.insert(id);
    }
    else
    {
        static_cast<stanza::xep0030::iq&>(iq_s).query();
        ptr_account->user_disco_queries.insert(id);
    }
    ptr_account->connection.send(iq_s.build(ptr_account->context).get());

        ui->printf(fmt::format("Querying service discovery ({}) for {}...", do_items ? "items" : "info", target));

    return WEECHAT_RC_OK;
}

int command__roster(const void *pointer, void *data,
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

    auto ui = weechat::UiPort::for_buffer(buffer);

    if (!ptr_account->connected())
    {
        ui->printf_error(fmt::format(fmt::runtime(_("{}: you are not connected to server")), WEECHAT_XMPP_PLUGIN_NAME));
        return WEECHAT_RC_OK;
    }

    // /roster - display roster as interactive picker
    if (argc == 1)
    {
        if (ptr_account->roster.empty())
        {
        ui->printf_network("Roster is empty");
            return WEECHAT_RC_OK;
        }

        using picker_t = weechat::ui::picker<std::string>;
        std::vector<picker_t::entry> entries;
        for (const auto& [jid, item] : ptr_account->roster)
        {
            std::string label = jid;
            if (!item.name.empty())
                label = item.name + " <" + jid + ">";
            std::string sublabel = item.subscription;
            if (!item.groups.empty()) {
                sublabel += "  [";
                bool first = true;
                std::ranges::for_each(item.groups, [&](const auto &g) {
                    if (!first) sublabel += ", ";
                    first = false;
                    sublabel += g;
                });
                sublabel += "]";
            }
            entries.push_back({ jid, label, sublabel, true });
        }

        auto p = std::make_unique<picker_t>(
            "xmpp.picker.roster",
            "Open chat with contact  (roster)",
            std::move(entries),
            [buf = buffer](std::string_view selected) {
                auto cmd = fmt::format("/open {}", selected);
                weechat_command(buf, cmd.c_str());
            },
            picker_t::close_cb{},
            buffer,
            true /* sort_entries */);
        if (!*p) return WEECHAT_RC_ERROR;
        p.release();
        return WEECHAT_RC_OK;
    }

    // /roster add <jid> [name]
    if (argc >= 3 && weechat_strcasecmp(argv[1], "add") == 0)
    {
        const char *jid = argv[2];
        const char *name = (argc >= 4) ? argv_eol[3] : nullptr;

        std::string id = stanza::uuid(ptr_account->context);
        stanza::rfc6121::item it(jid);
        if (name) it.name(name);
        stanza::rfc6121::query q;
        q.item(it);
        auto iq_s = stanza::iq().type("set").id(id);
        static_cast<stanza::rfc6121::iq&>(iq_s).query(q);
        ptr_account->connection.send(iq_s.build(ptr_account->context).get());

        ui->printf_network(fmt::format("Adding {} to roster...", jid));

        // Also send presence subscription request
        auto sub = stanza::presence().type("subscribe").to(jid);
        ptr_account->connection.send(sub.build(ptr_account->context).get());

        return WEECHAT_RC_OK;
    }

    // /roster del <jid>
    if (argc >= 3 && (weechat_strcasecmp(argv[1], "del") == 0 ||
                       weechat_strcasecmp(argv[1], "delete") == 0 ||
                       weechat_strcasecmp(argv[1], "remove") == 0))
    {
        const char *jid = argv[2];

        std::string id = stanza::uuid(ptr_account->context);
        stanza::rfc6121::item it(jid);
        it.subscription("remove");
        stanza::rfc6121::query q;
        q.item(it);
        auto iq_s = stanza::iq().type("set").id(id);
        static_cast<stanza::rfc6121::iq&>(iq_s).query(q);
        ptr_account->connection.send(iq_s.build(ptr_account->context).get());

        ui->printf_network(fmt::format("Removing {} from roster...", jid));

        return WEECHAT_RC_OK;
    }

        ui->printf_error(fmt::format(fmt::runtime(_("{}: unknown roster command (try /help roster)")), WEECHAT_XMPP_PLUGIN_NAME));

    return WEECHAT_RC_OK;
}

int command__bookmark(const void *pointer, void *data,
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

    auto ui = weechat::UiPort::for_buffer(buffer);

    if (!ptr_account->connected())
    {
        ui->printf_error(fmt::format(fmt::runtime(_("{}: you are not connected to server")), WEECHAT_XMPP_PLUGIN_NAME));
        return WEECHAT_RC_OK;
    }

    // /bookmark - list bookmarks via interactive picker
    if (argc == 1)
    {
        if (ptr_account->bookmarks.empty())
        {
        ui->printf_network("No bookmarks");
            return WEECHAT_RC_OK;
        }

        using picker_t = weechat::ui::picker<std::string>;
        std::vector<picker_t::entry> entries;
        for (const auto& [jid, bookmark] : ptr_account->bookmarks)
        {
            std::string label = bookmark.name.empty() ? jid : bookmark.name + " <" + jid + ">";
            std::string sublabel;
            if (!bookmark.nick.empty())
                sublabel += "nick: " + bookmark.nick;
            if (bookmark.autojoin)
                sublabel += sublabel.empty() ? "autojoin" : "  autojoin";
            entries.push_back({jid, label, sublabel});
        }
        auto p = std::make_unique<picker_t>(
            "xmpp.picker.bookmark",
            "Open bookmark  (XEP-0048)",
            std::move(entries),
            [buf = buffer](std::string_view selected) {
                auto cmd = fmt::format("/enter {}", selected);
                weechat_command(buf, cmd.c_str());
            },
            picker_t::close_cb{},
            buffer,
            true /* sort_entries */);
        if (!*p) return WEECHAT_RC_ERROR;
        p.release();
        return WEECHAT_RC_OK;
    }

    // /bookmark add [jid] [name]
    if (argc >= 2 && weechat_strcasecmp(argv[1], "add") == 0)
    {
        const char *jid = nullptr;
        const char *name = nullptr;
        
        if (argc >= 3)
        {
            jid = argv[2];
            name = (argc >= 4) ? argv_eol[3] : nullptr;
        }
        else if (ptr_channel && ptr_channel->type == weechat::channel::chat_type::MUC)
        {
            jid = ptr_channel->id.data();
            name = ptr_channel->name.data();
        }
        else
        {
        ui->printf_error("You must specify a JID or be in a MUC buffer");
            return WEECHAT_RC_OK;
        }

        // Add or update bookmark
        ptr_account->bookmarks[jid].jid = jid;
        ptr_account->bookmarks[jid].name = name ? name : "";
        ptr_account->bookmarks[jid].nick = ptr_account->nickname().data();
        ptr_account->bookmarks[jid].autojoin = false;
        
        ptr_account->send_bookmarks();
        
        ui->printf_network(fmt::format("Bookmark added: {}", jid));

        return WEECHAT_RC_OK;
    }

    // /bookmark del <jid>
    if (argc >= 3 && (weechat_strcasecmp(argv[1], "del") == 0 || 
                       weechat_strcasecmp(argv[1], "delete") == 0 ||
                       weechat_strcasecmp(argv[1], "remove") == 0))
    {
        const char *jid = argv[2];
        
        if (!ptr_account->bookmarks.contains(jid))
        {
        ui->printf_error(fmt::format("Bookmark not found: {}", jid));
            return WEECHAT_RC_OK;
        }
        
        ptr_account->bookmarks.erase(jid);
        ptr_account->retract_bookmark(jid);
        
        ui->printf_network(fmt::format("Bookmark removed: {}", jid));

        return WEECHAT_RC_OK;
    }

    // /bookmark autojoin <jid> <on|off>
    if (argc >= 4 && weechat_strcasecmp(argv[1], "autojoin") == 0)
    {
        const char *jid = argv[2];
        const char *value = argv[3];
        
        if (!ptr_account->bookmarks.contains(jid))
        {
        ui->printf_error(fmt::format("Bookmark not found: {}", jid));
            return WEECHAT_RC_OK;
        }
        
        bool autojoin = weechat_strcasecmp(value, "on") == 0 || 
                       weechat_strcasecmp(value, "true") == 0 ||
                       weechat_strcasecmp(value, "1") == 0;
        
        ptr_account->bookmarks[jid].autojoin = autojoin;
        ptr_account->send_bookmarks();
        
        ui->printf_network(fmt::format("Autojoin {} for {}", autojoin ? "enabled" : "disabled", jid));

        return WEECHAT_RC_OK;
    }

        ui->printf_error(fmt::format(fmt::runtime(_("{}: unknown bookmark command (try /help bookmark)")), WEECHAT_XMPP_PLUGIN_NAME));

    return WEECHAT_RC_OK;
}

// XEP-0433: Extended Channel Search
// Send a search IQ to the given service JID with optional keywords.
// Two steps: first request the form (type=get <search/>)), then submit it.
// picker_ptr: non-owning pointer to a picker<std::string> to populate with
//             results; nullptr to print inline.
