// Send a MUC join presence stanza to `to_jid` (room@domain/nick) from the account.
static void send_muc_join_presence(weechat::account *account, const char *to_jid)
{
    auto join_pres = stanza::presence().to(to_jid).from(account->jid());
    static_cast<stanza::xep0045::presence&>(join_pres).muc_join();
    account->connection.send(join_pres.build(account->context).get());
}

int command__enter(const void *pointer, void *data,
                   struct t_gui_buffer *buffer, int argc,
                   char **argv, char **argv_eol)
{
    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;
    const char *jid, *pres_jid, *text;

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
        int n_jid = 0;
        char **jids = weechat_string_split(argv[1], ",", nullptr, 0, 0, &n_jid);
        for (int i = 0; i < n_jid; i++)
        {
            ::jid jid_parsed(nullptr, jids[i]);
            std::string jid_bare_s = jid_parsed.bare;
            jid = jid_bare_s.c_str();
            pres_jid = jids[i];

            std::string pres_jid_s;
            if (jid_parsed.resource.empty())
            {
                std::string_view nick = ptr_account->nickname();
                std::string fallback_nick;
                if (nick.empty())
                    fallback_nick = ::jid(nullptr, ptr_account->jid()).local;
                const std::string &effective_nick = nick.empty() ? fallback_nick
                                                                  : std::string(nick);
                pres_jid_s = fmt::format("{}@{}/{}", jid_parsed.local,
                                         jid_parsed.domain, effective_nick);
                pres_jid = pres_jid_s.c_str();

                if (!ptr_account->channels.contains(jid))
                {
                    ptr_channel = &ptr_account->channels.emplace(
                        std::make_pair(jid, weechat::channel {
                                *ptr_account, weechat::channel::chat_type::MUC, jid, jid
                            })).first->second;
                    ptr_account->load_pgp_keys();
                }
                if (!ptr_channel) {
                    weechat_string_free_split(jids);
                    return WEECHAT_RC_ERROR;
                }

                send_muc_join_presence(ptr_account, pres_jid);
            }
            else
            {
                if (!ptr_account->channels.contains(jid))
                {
                    ptr_channel = &ptr_account->channels.emplace(
                        std::make_pair(jid, weechat::channel {
                                *ptr_account, weechat::channel::chat_type::MUC, jid, jid
                            })).first->second;
                    ptr_account->load_pgp_keys();
                }
                if (!ptr_channel) {
                    weechat_string_free_split(jids);
                    return WEECHAT_RC_ERROR;
                }

                send_muc_join_presence(ptr_account, pres_jid);
            }

            if (argc > 2)
            {
                text = argv_eol[2];

                ptr_channel->send_message(jid, text);
            }

            int num = weechat_buffer_get_integer(ptr_channel->buffer, "number");
            auto buf = fmt::format("/buffer {}", num);
            weechat_command(ptr_account->buffer, buf.c_str());
        }
        weechat_string_free_split(jids);
    }
    else
    {
        const char *buffer_jid = weechat_buffer_get_string(buffer, "localvar_remote_jid");

        {
            ::jid bjid(nullptr, buffer_jid ? buffer_jid : "");
            const char *nick = weechat_buffer_get_string(buffer, "localvar_nick");
            std::string pres_jid_s = fmt::format("{}@{}/{}", bjid.local, bjid.domain,
                                                  nick ? nick : "");
            pres_jid = pres_jid_s.c_str();

            if (!ptr_account->channels.contains(buffer_jid))
                ptr_channel = &ptr_account->channels.emplace(
                    std::make_pair(std::string(buffer_jid), weechat::channel {
                            *ptr_account, weechat::channel::chat_type::MUC, buffer_jid, buffer_jid
                        })).first->second;

            send_muc_join_presence(ptr_account, pres_jid);
        }
    }

    return WEECHAT_RC_OK;
}

int command__open(const void *pointer, void *data,
                  struct t_gui_buffer *buffer, int argc,
                  char **argv, char **argv_eol)
{
    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;
    char *jid, *text;

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
        int n_jid = 0;
        char **jids = weechat_string_split(argv[1], ",", nullptr, 0, 0, &n_jid);
        for (int i = 0; i < n_jid; i++)
        {
            const std::string bare_jid = ::jid(nullptr, jids[i]).bare;
            const char *effective_jid = bare_jid.empty() ? jids[i] : bare_jid.c_str();

            // When in a MUC and given a bare nick (no '@'), build the full JID.
            std::string full_jid_s;
            bool is_muc_occupant_jid = false;
            if (ptr_channel && !std::string_view(effective_jid).contains('@'))
            {
                ::jid ch_jid(nullptr, ptr_channel->name);
                full_jid_s = fmt::format("{}@{}/{}", ch_jid.local, ch_jid.domain,
                                         effective_jid);
                effective_jid = full_jid_s.c_str();
                is_muc_occupant_jid = (ptr_channel->type == weechat::channel::chat_type::MUC);
            }

            jid = const_cast<char*>(effective_jid);

            // Only send a directed presence when opening a plain JID PM.
            // Do NOT send presence to a MUC occupant full JID (room@service/nick):
            // the MUC server treats any presence to a full MUC JID as a join
            // request and responds with <conflict/> if you are already in the room.
            if (!is_muc_occupant_jid)
            {
                auto open_pres = stanza::presence().to(jid).from(ptr_account->jid());
                ptr_account->connection.send(open_pres.build(ptr_account->context).get());
            }

            auto channel = ptr_account->channels.find(jid);
            if (channel == ptr_account->channels.end())
            {
                // Reset MAM timestamp for this channel so history will be fetched
                // (it might be -1 if previously closed)
                ptr_account->mam_cache_set_last_timestamp(jid, 0);
                
                channel = ptr_account->channels.emplace(
                    std::make_pair(std::string(jid), weechat::channel {
                            *ptr_account, weechat::channel::chat_type::PM, jid, jid
                        })).first;

                // Proactively fetch OMEMO devicelist for this contact so that
                // sessions can be established before sending the first message.
                if (ptr_account->omemo)
                    ptr_account->omemo.request_devicelist(*ptr_account, jid);
            }

            if (argc > 2)
            {
                text = argv_eol[2];

                channel->second.send_message(jid, text);
            }

            int num = weechat_buffer_get_integer(channel->second.buffer, "number");
            auto buf = fmt::format("/buffer {}", num);
            weechat_command(ptr_account->buffer, buf.c_str());
        }
        weechat_string_free_split(jids);
    }

    return WEECHAT_RC_OK;
}

