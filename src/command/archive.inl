int command__mam(const void *pointer, void *data,
                 struct t_gui_buffer *buffer, int argc,
                 char **argv, char **argv_eol)
{
    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;
    int days;

    (void) pointer;
    (void) data;
    (void) argv_eol;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    // XEP-0441: /mam prefs [get | default <always|never|roster> |
    //                        always <jid> | never <jid>]
    if (argc >= 2 && weechat_strcasecmp(argv[1], "prefs") == 0)
    {
        std::string iq_id = stanza::uuid(ptr_account->context);

        // Determine action: default is "get" (display current prefs).
        // Subcommands: default <value>, always <jid>, never <jid>.
        if (argc >= 4 && (weechat_strcasecmp(argv[2], "always") == 0
                          || weechat_strcasecmp(argv[2], "never") == 0))
        {
            // Send a set IQ with the specified JID in the named list.
            bool is_always = weechat_strcasecmp(argv[2], "always") == 0;
            stanza::xep0313::prefs::jid_list filled_list(argv[2]);
            filled_list.jid(argv[3]);
            stanza::xep0313::prefs::jid_list empty_list(is_always ? "never" : "always");
            stanza::xep0313::prefs p;
            p.default_("roster");
            p.always(is_always ? filled_list : empty_list);
            p.never_(is_always ? empty_list : filled_list);
            auto iq_s = stanza::iq().type("set").id(iq_id);
            static_cast<stanza::xep0313::iq&>(iq_s).prefs(p);
            ptr_account->mam_prefs_queries.emplace(iq_id, buffer);
            ptr_account->connection.send(iq_s.build(ptr_account->context).get());
        }
        else if (argc >= 4 && weechat_strcasecmp(argv[2], "default") == 0)
        {
            const char *defval = argv[3];
            if (weechat_strcasecmp(defval, "always") != 0
                && weechat_strcasecmp(defval, "never") != 0
                && weechat_strcasecmp(defval, "roster") != 0)
            {
                weechat_printf(buffer,
                    "%s/mam prefs default: value must be always, never, or roster",
                    weechat_prefix("error"));
                return WEECHAT_RC_OK;
            }
            // Empty always/never lists (preserve existing — server keeps them)
            stanza::xep0313::prefs p;
            p.default_(defval);
            p.always();
            p.never_();
            auto iq_s = stanza::iq().type("set").id(iq_id);
            static_cast<stanza::xep0313::iq&>(iq_s).prefs(p);
            ptr_account->mam_prefs_queries.emplace(iq_id, buffer);
            ptr_account->connection.send(iq_s.build(ptr_account->context).get());
        }
        else
        {
            // GET: fetch current preferences
            stanza::xep0313::prefs p;
            auto iq_s = stanza::iq().type("get").id(iq_id);
            static_cast<stanza::xep0313::iq&>(iq_s).prefs(p);
            ptr_account->mam_prefs_queries.emplace(iq_id, buffer);
            ptr_account->connection.send(iq_s.build(ptr_account->context).get());
        }

        return WEECHAT_RC_OK;
    }

    if (!ptr_channel)
    {
        weechat_printf(
            ptr_account->buffer,
            _("%s%s: \"%s\" command can not be executed on a account buffer"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, "mam");
        return WEECHAT_RC_OK;
    }

    time_t start, end;
    if (argc > 1)
    {
        errno = 0;
        days = strtol(argv[1], nullptr, 10);

        if (errno != 0)
        {
            weechat_printf(
                ptr_channel->buffer,
                _("%s%s: \"%s\" is not a valid number of %s for %s"),
                weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, argv[1], "days", "mam");
            days = MAM_DEFAULT_DAYS;
        }
    }
    else
        days = MAM_DEFAULT_DAYS;
    
    // Calculate time range: (now - days) to now
    end = time(nullptr);
    start = end - (days * 24 * 60 * 60);
    
    const std::string mam_uuid = stanza::uuid(ptr_account->context);
    ptr_channel->fetch_mam(mam_uuid.c_str(), &start, &end, nullptr);

    return WEECHAT_RC_OK;
}
