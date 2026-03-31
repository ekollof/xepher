int command__mam(const void *pointer, void *data,
                 struct t_gui_buffer *buffer, int argc,
                 char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;
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
        xmpp_string_guard uuid_g(ptr_account->context,
                                  xmpp_uuid_gen(ptr_account->context));
        const char *iq_id = uuid_g.ptr;

        xmpp_stanza_t *iq = xmpp_iq_new(ptr_account->context, "get", iq_id);

        // Determine action: default is "get" (display current prefs).
        // Subcommands: default <value>, always <jid>, never <jid>.
        if (argc >= 4 && (weechat_strcasecmp(argv[2], "always") == 0
                          || weechat_strcasecmp(argv[2], "never") == 0))
        {
            // Add or remove a JID from always/never list:
            // We must first fetch prefs, then merge and set.
            // For simplicity: send a set IQ with just this list populated.
            xmpp_stanza_set_type(iq, "set");
            xmpp_stanza_t *prefs = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(prefs, "prefs");
            xmpp_stanza_set_ns(prefs, "urn:xmpp:mam:2");
            xmpp_stanza_set_attribute(prefs, "default", "roster");

            xmpp_stanza_t *list = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(list, argv[2]); // "always" or "never"
            xmpp_stanza_t *jid_el = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(jid_el, "jid");
            xmpp_stanza_t *jid_text = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_text(jid_text, argv[3]);
            xmpp_stanza_add_child(jid_el, jid_text);
            xmpp_stanza_release(jid_text);
            xmpp_stanza_add_child(list, jid_el);
            xmpp_stanza_release(jid_el);

            // Add empty counterpart list
            const char *other_list = (weechat_strcasecmp(argv[2], "always") == 0)
                                     ? "never" : "always";
            xmpp_stanza_t *other = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(other, other_list);
            xmpp_stanza_add_child(prefs, list);
            xmpp_stanza_release(list);
            xmpp_stanza_add_child(prefs, other);
            xmpp_stanza_release(other);
            xmpp_stanza_add_child(iq, prefs);
            xmpp_stanza_release(prefs);
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
                xmpp_stanza_release(iq);
                return WEECHAT_RC_OK;
            }
            xmpp_stanza_set_type(iq, "set");
            xmpp_stanza_t *prefs = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(prefs, "prefs");
            xmpp_stanza_set_ns(prefs, "urn:xmpp:mam:2");
            xmpp_stanza_set_attribute(prefs, "default", defval);
            // Empty always/never lists (preserve existing — server keeps them)
            xmpp_stanza_t *always_el = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(always_el, "always");
            xmpp_stanza_t *never_el = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(never_el, "never");
            xmpp_stanza_add_child(prefs, always_el);
            xmpp_stanza_release(always_el);
            xmpp_stanza_add_child(prefs, never_el);
            xmpp_stanza_release(never_el);
            xmpp_stanza_add_child(iq, prefs);
            xmpp_stanza_release(prefs);
        }
        else
        {
            // GET: fetch current preferences
            xmpp_stanza_t *prefs = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(prefs, "prefs");
            xmpp_stanza_set_ns(prefs, "urn:xmpp:mam:2");
            xmpp_stanza_add_child(iq, prefs);
            xmpp_stanza_release(prefs);
        }

        ptr_account->mam_prefs_queries.emplace(iq_id, buffer);
        ptr_account->connection.send(iq);
        xmpp_stanza_release(iq);
        // uuid_g freed here
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
        days = strtol(argv[1], NULL, 10);

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
    end = time(NULL);
    start = end - (days * 24 * 60 * 60);
    
    xmpp_string_guard mam_uuid_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
    const char *mam_uuid = mam_uuid_g.ptr;
    ptr_channel->fetch_mam(mam_uuid, &start, &end, NULL);
    // freed by mam_uuid_g

    return WEECHAT_RC_OK;
}
