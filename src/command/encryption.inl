int command__omemo(const void *pointer, void *data,
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
    {
        weechat_printf(buffer,
                       _("%s%s: this command must be run in an XMPP account or channel buffer"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    // Helper: guard that OMEMO is initialized; prints error and returns false if not.
    auto require_omemo = [&]() -> bool {
        if (!ptr_account->omemo)
        {
            weechat_printf(buffer,
                           _("%s%s: OMEMO not initialized for this account"),
                           weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
            return false;
        }
        return true;
    };

    // Handle subcommands
    if (argc > 1)
    {
        if (weechat_strcasecmp(argv[1], "check") == 0)
        {
            if (!require_omemo()) return WEECHAT_RC_OK;

            // Show local state immediately (no async needed)
            ptr_account->omemo.show_status(
                buffer,
                ptr_account->name.data(),
                ptr_channel ? ptr_channel->name.data() : nullptr,
                ptr_channel ? ptr_channel->omemo.enabled : 0);

            // Also show own devices and peer device list if in a channel
            ptr_account->omemo.show_devices(buffer, ptr_account->jid().data());
            if (ptr_channel)
                ptr_account->omemo.show_devices(buffer, ptr_channel->name.data());

            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "republish") == 0)
        {
            if (!require_omemo()) return WEECHAT_RC_OK;

            weechat_printf(buffer,
                           _("%sRepublishing OMEMO devicelist and bundle..."),
                           weechat_prefix("network"));

            // Publish devicelist
            auto devicelist_stanza = ptr_account->get_devicelist();
            if (devicelist_stanza)
            {
                ptr_account->connection.send(devicelist_stanza.get());
                weechat_printf(buffer,
                               _("%sDevicelist published (device ID: %u)"),
                               weechat_prefix("network"), ptr_account->omemo.device_id);
            }

            // Publish bundle
            std::string from_s(ptr_account->jid());
            xmpp_stanza_t *bundle_stanza = ptr_account->omemo.get_bundle(
                ptr_account->context, from_s.data(), nullptr);
            if (bundle_stanza)
            {
                ptr_account->connection.send(bundle_stanza);
                xmpp_stanza_release(bundle_stanza);
                weechat_printf(buffer,
                               _("%sBundle published for device %u"),
                               weechat_prefix("network"), ptr_account->omemo.device_id);
            }
            else
            {
                weechat_printf(buffer,
                               _("%s%s: failed to generate OMEMO bundle"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
            }

            // Publish legacy OMEMO nodes for OMEMO:1 interoperability.
            auto legacy_devicelist_stanza = ptr_account->get_legacy_devicelist();
            if (legacy_devicelist_stanza)
            {
                ptr_account->connection.send(legacy_devicelist_stanza.get());
                weechat_printf(buffer,
                               _("%sLegacy devicelist published (device ID: %u)"),
                               weechat_prefix("network"), ptr_account->omemo.device_id);
            }

            xmpp_stanza_t *legacy_bundle_stanza = ptr_account->omemo.get_axolotl_bundle(
                ptr_account->context, from_s.data(), nullptr);
            if (legacy_bundle_stanza)
            {
                ptr_account->connection.send(legacy_bundle_stanza);
                xmpp_stanza_release(legacy_bundle_stanza);
                weechat_printf(buffer,
                               _("%sLegacy bundle published for device %u"),
                               weechat_prefix("network"), ptr_account->omemo.device_id);
            }
            else
            {
                weechat_printf(buffer,
                               _("%s%s: failed to generate legacy OMEMO bundle"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
            }

            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "reset-keys") == 0)
        {
            if (!require_omemo()) return WEECHAT_RC_OK;

            weechat_printf(buffer,
                           _("%s%s: Resetting OMEMO key database to force renegotiation"),
                           weechat_prefix("info"), WEECHAT_XMPP_PLUGIN_NAME);

            try {
                // Clear OMEMO database
                if (ptr_account->omemo.db_env)
                {
                    lmdb::txn txn = lmdb::txn::begin(ptr_account->omemo.db_env);
                    mdb_drop(txn.handle(), ptr_account->omemo.dbi.omemo, 0);
                    txn.commit();
                    weechat_printf(buffer,
                                   _("%s%s: OMEMO key database cleared. Session keys will be renegotiated."),
                                   weechat_prefix("info"), WEECHAT_XMPP_PLUGIN_NAME);
                }
            } catch (const lmdb::error& ex) {
                weechat_printf(buffer,
                               _("%s%s: Failed to reset OMEMO keys: %s"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, ex.what());
            }

            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "status") == 0)
        {
            if (!require_omemo()) return WEECHAT_RC_OK;

            ptr_account->omemo.show_status(
                buffer,
                ptr_account->name.data(),
                ptr_channel ? ptr_channel->name.data() : nullptr,
                ptr_channel ? ptr_channel->omemo.enabled : 0);

            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "fingerprint") == 0)
        {
            if (!require_omemo()) return WEECHAT_RC_OK;
            // Optional argument: jid to look up peer fingerprint; no argument = own key
            const char *jid = (argc > 2) ? argv[2] : nullptr;
            ptr_account->omemo.show_fingerprint(buffer, jid);
            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "trust") == 0)
        {
            if (!require_omemo()) return WEECHAT_RC_OK;
            if (argc < 3)
            {
                weechat_printf(buffer,
                               _("%s%s: usage: /omemo trust <jid>"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                return WEECHAT_RC_OK;
            }
            // XEP-0450 §4.2: broadcast <distrust> to own devices and peer before
            // wiping local keys so other clients learn the revocation decision.
            if (weechat::config::instance
                && weechat_config_boolean(weechat::config::instance->look.omemo_atm))
            {
                ptr_account->omemo.send_atm_distrust_pub(*ptr_account, argv[2]);
            }
            // Remove stored identity keys → next message triggers TOFU re-store
            ptr_account->omemo.distrust_jid(buffer, argv[2]);
            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "approve") == 0)
        {
            if (!require_omemo()) return WEECHAT_RC_OK;
            if (argc < 3)
            {
                weechat_printf(buffer,
                               _("%s%s: usage: /omemo approve <jid> [<fingerprint>]"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                return WEECHAT_RC_OK;
            }
            const char *fp = (argc > 3) ? argv[3] : nullptr;
            ptr_account->omemo.approve_jid(buffer, *ptr_account, argv[2], fp);
            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "distrust") == 0)
        {
            if (!require_omemo()) return WEECHAT_RC_OK;
            if (argc < 3)
            {
                weechat_printf(buffer,
                               _("%s%s: usage: /omemo distrust <jid> [<fingerprint>]"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                return WEECHAT_RC_OK;
            }
            const char *fp = (argc > 3) ? argv[3] : nullptr;
            ptr_account->omemo.distrust_fp(buffer, *ptr_account, argv[2], fp);
            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "devices") == 0)
        {
            if (!require_omemo()) return WEECHAT_RC_OK;
            // Optional argument: jid; default to channel JID or peer JID
            const char *jid = nullptr;
            if (argc > 2)
                jid = argv[2];
            else if (ptr_channel)
                jid = ptr_channel->name.data();
            if (!jid)
            {
                weechat_printf(buffer,
                               _("%s%s: usage: /omemo devices <jid>  (or run in a channel buffer)"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                return WEECHAT_RC_OK;
            }
            ptr_account->omemo.show_devices(buffer, jid);
            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "fetch") == 0)
        {
            if (!require_omemo()) return WEECHAT_RC_OK;

            const char *jid = nullptr;
            if (argc > 2)
                jid = argv[2];
            else if (ptr_channel)
                jid = ptr_channel->name.data();

            if (!jid)
            {
                weechat_printf(buffer,
                               _("%s%s: usage: /omemo fetch [<jid>] [<device-id>]"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                return WEECHAT_RC_OK;
            }

            std::optional<std::uint32_t> device_id;
            if (argc > 3)
            {
                char *end = nullptr;
                errno = 0;
                const auto parsed = strtoul(argv[3], &end, 10);
                if (errno != 0 || !end || *end != '\0' || parsed > UINT32_MAX)
                {
                    weechat_printf(buffer,
                                   _("%s%s: invalid device id '%s'"),
                                   weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, argv[3]);
                    return WEECHAT_RC_OK;
                }
                device_id = static_cast<std::uint32_t>(parsed);
            }

            ptr_account->omemo.force_fetch(*ptr_account, buffer, jid, device_id);
            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "kex") == 0)
        {
            if (!require_omemo()) return WEECHAT_RC_OK;

            const char *jid = nullptr;
            if (argc > 2)
                jid = argv[2];
            else if (ptr_channel)
                jid = ptr_channel->name.data();

            if (!jid)
            {
                weechat_printf(buffer,
                               _("%s%s: usage: /omemo kex [<jid>] [<device-id>]"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                return WEECHAT_RC_OK;
            }

            std::optional<std::uint32_t> device_id;
            if (argc > 3)
            {
                char *end = nullptr;
                errno = 0;
                const auto parsed = strtoul(argv[3], &end, 10);
                if (errno != 0 || !end || *end != '\0' || parsed > UINT32_MAX)
                {
                    weechat_printf(buffer,
                                   _("%s%s: invalid device id '%s'"),
                                   weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, argv[3]);
                    return WEECHAT_RC_OK;
                }
                device_id = static_cast<std::uint32_t>(parsed);
            }

            ptr_account->omemo.force_kex(*ptr_account, buffer, jid, device_id);
            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "optout") == 0)
        {
            if (!require_omemo()) return WEECHAT_RC_OK;
            const char *jid = nullptr;
            if (argc > 2)
                jid = argv[2];
            else if (ptr_channel)
                jid = ptr_channel->name.data();
            if (!jid)
            {
                weechat_printf(buffer,
                               _("%s%s: usage: /omemo optout <jid> [<reason>]"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                return WEECHAT_RC_OK;
            }
            const char *reason = (argc > 3) ? argv_eol[3] : nullptr;
            ptr_account->omemo.send_opt_out(*ptr_account, buffer, jid, reason);
            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "optout-ack") == 0)
        {
            if (!require_omemo()) return WEECHAT_RC_OK;
            const char *jid = nullptr;
            if (argc > 2)
                jid = argv[2];
            else if (ptr_channel)
                jid = ptr_channel->name.data();
            if (!jid)
            {
                weechat_printf(buffer,
                               _("%s%s: usage: /omemo optout-ack <jid>"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                return WEECHAT_RC_OK;
            }
            ptr_account->omemo.optout_ack(buffer, jid);
            return WEECHAT_RC_OK;
        }

        WEECHAT_COMMAND_ERROR;
    }

    // Default behavior: enable OMEMO for current channel
    if (!ptr_channel)
    {
        weechat_printf(
            buffer,
            _("%s%s: \"%s\" command requires a channel buffer or use /omemo republish on account buffer"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, "omemo");
        return WEECHAT_RC_OK;
    }

    ptr_channel->omemo.enabled = 1;
    ptr_channel->pgp.enabled = 0;

    ptr_channel->set_transport(weechat::channel::transport::OMEMO, 0);

    weechat_bar_item_update("xmpp_encryption");

    return WEECHAT_RC_OK;
}

int command__pgp(const void *pointer, void *data,
                 struct t_gui_buffer *buffer, int argc,
                 char **argv, char **argv_eol)
{
    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;
    char *keyid;

    (void) pointer;
    (void) data;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_channel)
    {
        weechat_printf(
            ptr_account->buffer,
            _("%s%s: \"%s\" command can not be executed on a account buffer"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, "pgp");
        return WEECHAT_RC_OK;
    }

    if (argc > 1)
    {
        if (weechat_strcasecmp(argv[1], "status") == 0)
        {
            weechat_printf(ptr_account->buffer,
                           _("%sPGP Status for channel %s:"),
                           weechat_prefix("info"), ptr_channel->name.data());
            weechat_printf(ptr_account->buffer,
                           _("%s  Encryption: %s"),
                           weechat_prefix("info"), ptr_channel->pgp.enabled ? "ENABLED" : "disabled");

            if (ptr_channel->pgp.ids.empty())
            {
                weechat_printf(ptr_account->buffer,
                               _("%s  No PGP keys configured"),
                               weechat_prefix("info"));
            }
            else
            {
                weechat_printf(ptr_account->buffer,
                               _("%s  Configured keys:"),
                               weechat_prefix("info"));
                for (const auto& key_id : ptr_channel->pgp.ids)
                {
                    weechat_printf(ptr_account->buffer,
                                   _("%s    - %s"),
                                   weechat_prefix("info"), key_id.data());
                }
            }

            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "reset") == 0)
        {
            if (ptr_channel->pgp.ids.empty())
            {
                weechat_printf(ptr_account->buffer,
                               _("%s%s: No PGP keys configured for this channel"),
                               weechat_prefix("info"), WEECHAT_XMPP_PLUGIN_NAME);
                return WEECHAT_RC_OK;
            }

            size_t count = ptr_channel->pgp.ids.size();
            ptr_channel->pgp.ids.clear();
            ptr_account->save_pgp_keys();
            weechat::config::write();

            weechat_printf(ptr_account->buffer,
                           _("%s%s: Removed %zu PGP key(s) from channel '%s'"),
                           weechat_prefix("info"), WEECHAT_XMPP_PLUGIN_NAME,
                           count, ptr_channel->name.data());

            return WEECHAT_RC_OK;
        }

        // Default: add key
        keyid = argv_eol[1];
        ptr_channel->pgp.ids.emplace(keyid);
        ptr_account->save_pgp_keys();
        weechat::config::write();

        weechat_printf(ptr_account->buffer,
                       _("%s%s: Added PGP key '%s' to channel '%s'"),
                       weechat_prefix("info"), WEECHAT_XMPP_PLUGIN_NAME,
                       keyid, ptr_channel->name.data());

        return WEECHAT_RC_OK;
    }

    // Default: enable PGP for current channel
    ptr_channel->omemo.enabled = 0;
    ptr_channel->pgp.enabled = 1;

    ptr_channel->set_transport(weechat::channel::transport::PGP, 0);

    weechat_bar_item_update("xmpp_encryption");

    return WEECHAT_RC_OK;
}

int command__plain(const void *pointer, void *data,
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
        weechat_printf(
            ptr_account->buffer,
            _("%s%s: \"%s\" command can not be executed on a account buffer"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, "plain");
        return WEECHAT_RC_OK;
    }

    ptr_channel->omemo.enabled = 0;
    ptr_channel->pgp.enabled = 0;

    ptr_channel->set_transport(weechat::channel::transport::PLAIN, 0);

    weechat_bar_item_update("xmpp_encryption");

    return WEECHAT_RC_OK;
}

int command__xml(const void *pointer, void *data,
                 struct t_gui_buffer *buffer, int argc,
                 char **argv, char **argv_eol)
{
    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;
    xmpp_stanza_t *stanza;

    (void) pointer;
    (void) data;
    (void) argv;

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

    if (argc > 1)
    {
        auto parse = [&](sexp::driver& sxml) {
            std::stringstream ss;
            std::string line;
            try {
                return sxml.parse(argv_eol[1], &ss);
            }
            catch (const std::invalid_argument& ex) {
                while (std::getline(ss, line))
                    weechat_printf(nullptr, "%ssxml: %s", weechat_prefix("info"), line.data());
                weechat_printf(nullptr, "%ssxml: %s", weechat_prefix("error"), ex.what());
                return false;
            }
        };
        if (sexp::driver sxml(ptr_account->context); parse(sxml))
        {
            for (auto *stanza : sxml.elements)
            {
                ptr_account->connection.send( stanza);
                xmpp_stanza_release(stanza);
            }
        }
        else
        {
            stanza = xmpp_stanza_new_from_string(ptr_account->context,
                                                argv_eol[1]);
            if (!stanza)
            {
                weechat_printf(nullptr, _("%s%s: Bad XML"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                return WEECHAT_RC_ERROR;
            }

            ptr_account->connection.send( stanza);
            xmpp_stanza_release(stanza);
        }
    }

    return WEECHAT_RC_OK;
}

int command__xmpp(const void *pointer, void *data,
                  struct t_gui_buffer *buffer, int argc,
                  char **argv, char **argv_eol)
{
    (void) pointer;
    (void) data;
    (void) buffer;
    (void) argc;
    (void) argv;
    (void) argv_eol;

    weechat_printf(nullptr,
                   _("%s%s %s [%s]"),
                   weechat_prefix("info"), WEECHAT_XMPP_PLUGIN_NAME,
                   WEECHAT_XMPP_PLUGIN_VERSION, XMPP_PLUGIN_COMMIT);

    return WEECHAT_RC_OK;
}

int command__trap(const void *pointer, void *data,
                  struct t_gui_buffer *buffer, int argc,
                  char **argv, char **argv_eol)
{
    (void) pointer;
    (void) data;
    (void) buffer;
    (void) argc;
    (void) argv;
    (void) argv_eol;

    weechat::account *account = nullptr;
    weechat::channel *channel = nullptr;

    buffer__get_account_and_channel(buffer, &account, &channel);
    weechat::user::search(account, account->jid_device().data());

    (void) channel; // trap is for debugging only

    __builtin_trap();

    return WEECHAT_RC_OK;
}



