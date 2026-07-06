namespace {

void show_channel_omemo_devices(weechat::account *account,
                                weechat::channel *channel,
                                struct t_gui_buffer *buffer)
{
    if (!account || !account->omemo || !channel)
        return;

    auto ui = weechat::UiPort::for_buffer(buffer);
    if (channel->type == weechat::channel::chat_type::MUC)
    {
        const auto recipients = channel->omemo_recipient_list();
        if (recipients.empty())
        {
            ui->printf_info(fmt::format(
                "{}: no OMEMO recipients discovered for this MUC yet",
                WEECHAT_XMPP_PLUGIN_NAME));
            return;
        }
        for (const auto &bare : recipients)
            account->omemo.show_devices(buffer, bare.c_str());
        return;
    }

    account->omemo.show_devices(buffer, channel->name.data());
}

}  // namespace

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

    auto ui = weechat::UiPort::for_buffer(buffer);

    if (!ptr_account)
    {
        ui->printf_error(fmt::format("{}: this command must be run in an XMPP account or channel buffer",
            WEECHAT_XMPP_PLUGIN_NAME));
        return WEECHAT_RC_OK;
    }

    // Helper: guard that OMEMO is initialized; prints error and returns false if not.
    auto require_omemo = [&]() -> bool {
        if (!ptr_account->omemo)
        {
            ui->printf_error(fmt::format("{}: OMEMO not initialized for this account",
                WEECHAT_XMPP_PLUGIN_NAME));
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

            // Also show own devices and per-occupant lists in MUC (not the room JID)
            ptr_account->omemo.show_devices(buffer, ptr_account->jid().data());
            if (ptr_channel)
                show_channel_omemo_devices(ptr_account, ptr_channel, buffer);

            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "republish") == 0)
        {
            if (!require_omemo()) return WEECHAT_RC_OK;

            ui->printf_network(_("Republishing OMEMO devicelist and bundle..."));

            // Publish axolotl devicelist
            auto devicelist_stanza = ptr_account->get_devicelist();
            if (devicelist_stanza)
            {
                ptr_account->connection.send(devicelist_stanza.get());
        ui->printf_network(fmt::format("Devicelist published (device ID: {})",
                    ptr_account->omemo.device_id));
            }

            // Publish axolotl bundle
            std::string from_s(ptr_account->jid());
            xmpp_stanza_t *bundle_stanza = ptr_account->omemo.get_axolotl_bundle(
                ptr_account->context, from_s.data(), nullptr);
            if (bundle_stanza)
            {
                ptr_account->connection.send(bundle_stanza);
                xmpp_stanza_release(bundle_stanza);
        ui->printf_network(fmt::format("Bundle published for device {}",
                    ptr_account->omemo.device_id));
            }
            else
            {
        ui->printf_error(fmt::format("{}: failed to generate OMEMO bundle",
                    WEECHAT_XMPP_PLUGIN_NAME));
            }

            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "reset-keys") == 0)
        {
            if (!require_omemo()) return WEECHAT_RC_OK;

            ui->printf_info(fmt::format("{}: Resetting OMEMO key database to force renegotiation",
                WEECHAT_XMPP_PLUGIN_NAME));

            try {
                // Clear OMEMO database
                if (ptr_account->omemo.db_env)
                {
                    lmdb::txn txn = lmdb::txn::begin(ptr_account->omemo.db_env);
                    mdb_drop(txn.handle(), ptr_account->omemo.dbi.omemo, 0);
                    txn.commit();
        ui->printf_info(fmt::format("{}: OMEMO key database cleared. Session keys will be renegotiated.",
                        WEECHAT_XMPP_PLUGIN_NAME));
                }
            } catch (const lmdb::error& ex) {
        ui->printf_error(fmt::format("{}: Failed to reset OMEMO keys: {}",
                    WEECHAT_XMPP_PLUGIN_NAME, ex.what()));
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
        ui->printf_error(fmt::format("{}: usage: /omemo trust <jid> [<device-id>]",
                    WEECHAT_XMPP_PLUGIN_NAME));
                return WEECHAT_RC_OK;
            }
            std::optional<std::uint32_t> device_id;
            if (argc > 3)
            {
                if (auto v = parse_uint32(argv[3]); v && *v > 0 && *v <= 0x7fffffffU)
                    device_id = *v;
                else
                {
        ui->printf_error(fmt::format("{}: /omemo trust: device id must be a positive integer",
                        WEECHAT_XMPP_PLUGIN_NAME));
                    return WEECHAT_RC_OK;
                }
            }
            ptr_account->omemo.trust_jid(buffer, *ptr_account, argv[2], device_id);
            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "distrust") == 0)
        {
            if (!require_omemo()) return WEECHAT_RC_OK;
            if (argc < 3)
            {
        ui->printf_error(fmt::format("{}: usage: /omemo distrust <jid> [<fingerprint>]",
                    WEECHAT_XMPP_PLUGIN_NAME));
                return WEECHAT_RC_OK;
            }
            std::optional<std::uint32_t> device_id;
            if (argc > 3)
            {
                if (auto v = parse_uint32(argv[3]); v && *v > 0 && *v <= 0x7fffffffU)
                    device_id = *v;
                else
                {
        ui->printf_error(fmt::format("{}: /omemo distrust: device id must be a positive integer",
                        WEECHAT_XMPP_PLUGIN_NAME));
                    return WEECHAT_RC_OK;
                }
            }
            ptr_account->omemo.distrust_fp(buffer, *ptr_account, argv[2], device_id);
            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "devices") == 0)
        {
            if (!require_omemo()) return WEECHAT_RC_OK;
            // Optional argument: jid; default to channel JID or peer JID
            const char *jid = nullptr;
            if (argc > 2)
                jid = argv[2];
            else if (ptr_channel
                     && ptr_channel->type != weechat::channel::chat_type::MUC)
                jid = ptr_channel->name.data();
            if (!jid)
            {
                if (ptr_channel
                    && ptr_channel->type == weechat::channel::chat_type::MUC)
                {
                    show_channel_omemo_devices(ptr_account, ptr_channel, buffer);
                    return WEECHAT_RC_OK;
                }
        ui->printf_error(fmt::format("{}: usage: /omemo devices <jid>  (or run in a channel buffer)",
                    WEECHAT_XMPP_PLUGIN_NAME));
                return WEECHAT_RC_OK;
            }
            ptr_account->omemo.show_devices(buffer, jid);
            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "fetch") == 0)
        {
            if (!require_omemo()) return WEECHAT_RC_OK;

            std::optional<std::uint32_t> device_id;
            if (argc > 3)
            {
                if (auto parsed = parse_uint32(argv[3]); parsed)
                    device_id = *parsed;
                else
                {
        ui->printf_error(fmt::format("{}: invalid device id '{}'",
                        WEECHAT_XMPP_PLUGIN_NAME, argv[3]));
                    return WEECHAT_RC_OK;
                }
            }

            if (argc > 2)
            {
                ptr_account->omemo.force_fetch(*ptr_account, buffer, argv[2], device_id);
                return WEECHAT_RC_OK;
            }

            if (ptr_channel && ptr_channel->type == weechat::channel::chat_type::MUC)
            {
                const auto recipients = ptr_channel->omemo_recipient_list();
                if (recipients.empty())
                {
        ui->printf_error(fmt::format(
                        "{}: no OMEMO recipients discovered for this MUC yet",
                        WEECHAT_XMPP_PLUGIN_NAME));
                    return WEECHAT_RC_OK;
                }
                for (const auto &bare : recipients)
                    ptr_account->omemo.force_fetch(*ptr_account, buffer, bare.c_str(), device_id);
                return WEECHAT_RC_OK;
            }

            if (ptr_channel)
            {
                ptr_account->omemo.force_fetch(*ptr_account, buffer,
                                               ptr_channel->name.data(), device_id);
                return WEECHAT_RC_OK;
            }

        ui->printf_error(fmt::format("{}: usage: /omemo fetch [<jid>] [<device-id>]",
                    WEECHAT_XMPP_PLUGIN_NAME));
            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "kex") == 0)
        {
            if (!require_omemo()) return WEECHAT_RC_OK;

            if (argc < 3
                && ptr_channel
                && ptr_channel->type == weechat::channel::chat_type::MUC)
            {
        ui->printf_error(fmt::format(
                    "{}: usage: /omemo kex <occupant-jid> [<device-id>] in MUC buffers",
                    WEECHAT_XMPP_PLUGIN_NAME));
                return WEECHAT_RC_OK;
            }

            const char *jid = nullptr;
            if (argc > 2)
                jid = argv[2];
            else if (ptr_channel)
                jid = ptr_channel->name.data();

            if (!jid)
            {
        ui->printf_error(fmt::format("{}: usage: /omemo kex [<jid>] [<device-id>]",
                    WEECHAT_XMPP_PLUGIN_NAME));
                return WEECHAT_RC_OK;
            }

            std::optional<std::uint32_t> device_id;
            if (argc > 3)
            {
                if (auto parsed = parse_uint32(argv[3]); parsed)
                    device_id = *parsed;
                else
                {
        ui->printf_error(fmt::format("{}: invalid device id '{}'",
                        WEECHAT_XMPP_PLUGIN_NAME, argv[3]));
                    return WEECHAT_RC_OK;
                }
            }

            ptr_account->omemo.force_kex(*ptr_account, buffer, jid, device_id);
            return WEECHAT_RC_OK;
        }

        WEECHAT_COMMAND_ERROR;
    }

    // Default behavior: enable OMEMO for current channel
    if (!ptr_channel)
    {
        ui->printf_error(fmt::format(
            "{}: \"{}\" command requires a channel buffer or use /omemo republish on account buffer",
            WEECHAT_XMPP_PLUGIN_NAME, "omemo"));
        return WEECHAT_RC_OK;
    }

    if (ptr_channel->type == weechat::channel::chat_type::MUC)
    {
        if (!ptr_channel->muc_supports_omemo())
        {
            const auto &info = ptr_channel->get_muc_info();
            const char *anon = (info.anon == weechat::channel::muc_info::anonymity::semianonymous)
                ? "semi-anonymous"
                : (info.anon == weechat::channel::muc_info::anonymity::anonymous)
                    ? "fully anonymous"
                    : "not yet known as non-anonymous";
            ui->printf_error(fmt::format(
                "{}: OMEMO group chat requires a non-anonymous room (XEP-0384 §5.8); this room is {}",
                WEECHAT_XMPP_PLUGIN_NAME, anon));
            return WEECHAT_RC_OK;
        }
        if (ptr_channel->omemo_recipient_jids.empty())
        {
            ui->printf_error(fmt::format(
                "{}: OMEMO not ready yet — waiting for member/admin/owner affiliation lists",
                WEECHAT_XMPP_PLUGIN_NAME));
            return WEECHAT_RC_OK;
        }
        if (!ptr_channel->all_occupants_have_real_jid())
        {
            ptr_channel->notify_omemo_missing_real_jids(*ui);
            return WEECHAT_RC_OK;
        }
    }

    ptr_channel->omemo.enabled = 1;
    ptr_channel->pgp.enabled = 0;

    ptr_channel->set_transport(weechat::channel::transport::OMEMO, 0);

    weechat_bar_item_update("xmpp_encryption");

    // docs/planning-muc-omemo.md §6: Surface the blind-trust concern for MUCs.
    if (ptr_channel->type == weechat::channel::chat_type::MUC)
    {
        if (!ptr_channel->get_muc_info().members_only)
        {
            ui->printf_network(fmt::format(
                "{}: Note: XEP-0384 §5.8 recommends members-only rooms for OMEMO group chat; "
                "this room is open.",
                WEECHAT_XMPP_PLUGIN_NAME));
        }
        ui->printf_network(fmt::format(
            "{}: Note: OMEMO in this MUC uses blind trust (BTBV) for all occupants by default. "
            "Manually verify important devices with /omemo trust if desired.",
            WEECHAT_XMPP_PLUGIN_NAME));
    }

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
        weechat::UiPort::for_buffer(ptr_account->buffer)->printf_error(fmt::format(
            "{}: \"{}\" command can not be executed on a account buffer",
            WEECHAT_XMPP_PLUGIN_NAME, "pgp"));
        return WEECHAT_RC_OK;
    }

    if (argc > 1)
    {
        if (weechat_strcasecmp(argv[1], "status") == 0)
        {
        weechat::UiPort::for_buffer(ptr_account->buffer)->printf_info(fmt::format(fmt::runtime(_("PGP Status for channel {}:")), ptr_channel->name.data()));
        weechat::UiPort::for_buffer(ptr_account->buffer)->printf_info(fmt::format(fmt::runtime(_("  Encryption: {}")), ptr_channel->pgp.enabled ? "ENABLED" : "disabled"));

            if (ptr_channel->pgp.ids.empty())
            {
        weechat::UiPort::for_buffer(ptr_account->buffer)->printf_info(_("  No PGP keys configured"));
            }
            else
            {
        weechat::UiPort::for_buffer(ptr_account->buffer)->printf_info(_("  Configured keys:"));
                for (const auto& key_id : ptr_channel->pgp.ids)
                {
        weechat::UiPort::for_buffer(ptr_account->buffer)->printf_info(fmt::format(fmt::runtime(_("    - {}")), key_id.data()));
                }
            }

            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "reset") == 0)
        {
            if (ptr_channel->pgp.ids.empty())
            {
        weechat::UiPort::for_buffer(ptr_account->buffer)->printf_info(fmt::format(fmt::runtime(_("{}: No PGP keys configured for this channel")), WEECHAT_XMPP_PLUGIN_NAME));
                return WEECHAT_RC_OK;
            }

            size_t count = ptr_channel->pgp.ids.size();
            ptr_channel->pgp.ids.clear();
            ptr_account->save_pgp_keys();
            weechat::config::write();

        weechat::UiPort::for_buffer(ptr_account->buffer)->printf_info(fmt::format(fmt::runtime(_("{}: Removed {} PGP key(s) from channel '{}'")), WEECHAT_XMPP_PLUGIN_NAME, count, ptr_channel->name.data()));

            return WEECHAT_RC_OK;
        }

        // Default: add key
        keyid = argv_eol[1];
        ptr_channel->pgp.ids.emplace(keyid);
        ptr_account->save_pgp_keys();
        weechat::config::write();

        weechat::UiPort::for_buffer(ptr_account->buffer)->printf_info(fmt::format(fmt::runtime(_("{}: Added PGP key '{}' to channel '{}'")), WEECHAT_XMPP_PLUGIN_NAME, keyid, ptr_channel->name.data()));

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
        weechat::UiPort::for_buffer(ptr_account->buffer)->printf_error(fmt::format(
            "{}: \"{}\" command can not be executed on a account buffer",
            WEECHAT_XMPP_PLUGIN_NAME, "plain"));
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

    (void) pointer;
    (void) data;
    (void) argv;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_account->connected())
    {
        weechat::UiPort::for_buffer(buffer)->printf_error(fmt::format("{}: you are not connected to server",
            WEECHAT_XMPP_PLUGIN_NAME));
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
        weechat::UiPort::for_buffer(nullptr)->printf_info("");
        weechat::UiPort::for_buffer(nullptr)->printf_error("");
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
            auto stanza = stanza_from_string(ptr_account->context, argv_eol[1]);
            if (!stanza)
            {
        weechat::UiPort::for_buffer(nullptr)->printf_error(fmt::format(fmt::runtime(_("{}: Bad XML")), WEECHAT_XMPP_PLUGIN_NAME));
                return WEECHAT_RC_ERROR;
            }

            ptr_account->connection.send(stanza.get());
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

        weechat::UiPort::for_buffer(nullptr)->printf_info(fmt::format(fmt::runtime(_(" {} [{}]")), WEECHAT_XMPP_PLUGIN_NAME, WEECHAT_XMPP_PLUGIN_VERSION, XMPP_PLUGIN_COMMIT));

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



