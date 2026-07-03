int command__enter([[maybe_unused]] const void *pointer,
                   [[maybe_unused]] void *data,
                   struct t_gui_buffer *buffer, int argc,
                   char **argv, char **argv_eol)
{
    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;
    const char *jid, *pres_jid, *text;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    auto ui = weechat::UiPort::for_buffer(buffer);

    if (!ptr_account->connected())
    {
        ui->printf_error(fmt::format(fmt::runtime(_("{}: you are not connected to server")), WEECHAT_XMPP_PLUGIN_NAME));
        return WEECHAT_RC_OK;
    }

    // Parse flags and positionals. --no-switch: join without /buffer (bookmark autoconnect).
    // --password / -k: join password-protected MUCs (XEP-0045 §7.1.4).
    std::string room_password;
    bool no_switch = false;
    int jid_arg_index = -1;     // argv index of the room JID (may be comma-separated)
    int pos_arg_offset = -1;  // argv index of optional first message to send
    for (int i = 1; i < argc; ++i)
    {
        std::string_view a = argv[i];
        if (a == "--no-switch")
        {
            no_switch = true;
            continue;
        }
        if ((a == "--password" || a == "-k") && i + 1 < argc)
        {
            room_password = argv[++i];
            continue;
        }
        if (a.starts_with("--password="))
        {
            room_password = std::string(a.substr(std::strlen("--password=")));
            continue;
        }
        if (a.starts_with("-k="))
        {
            room_password = std::string(a.substr(3));
            continue;
        }
        if (jid_arg_index < 0)
            jid_arg_index = i;
        else if (pos_arg_offset < 0)
            pos_arg_offset = i;
    }

    if (jid_arg_index >= 0)
    {
        int n_jid = 0;
        char **jids = weechat_string_split(argv[jid_arg_index], ",", nullptr, 0, 0, &n_jid);
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
                    auto [it_ch, _ins] = ptr_account->channels.emplace(
                        std::make_pair(jid, weechat::channel {
                                *ptr_account, weechat::channel::chat_type::MUC, jid, jid
                            }));
                    auto& [_, ch] = *it_ch;
                    ptr_channel = &ch;
                    ptr_account->load_pgp_keys();
                }
                if (!ptr_channel) {
                    weechat_string_free_split(jids);
                    return WEECHAT_RC_ERROR;
                }

                xmpp::send_muc_join_presence(*ptr_account, pres_jid, room_password);
            }
            else
            {
                if (!ptr_account->channels.contains(jid))
                {
                    auto [it_ch, _ins] = ptr_account->channels.emplace(
                        std::make_pair(jid, weechat::channel {
                                *ptr_account, weechat::channel::chat_type::MUC, jid, jid
                            }));
                    auto& [_, ch] = *it_ch;
                    ptr_channel = &ch;
                    ptr_account->load_pgp_keys();
                }
                if (!ptr_channel) {
                    weechat_string_free_split(jids);
                    return WEECHAT_RC_ERROR;
                }

                xmpp::send_muc_join_presence(*ptr_account, pres_jid, room_password);
            }

            // The optional trailing positional arg is the first message to
            // send upon entering the room.
            if (pos_arg_offset > 0)
            {
                text = argv_eol[pos_arg_offset];
                ptr_channel->send_message(jid, text);
            }

            if (!no_switch)
            {
                int num = weechat_buffer_get_integer(ptr_channel->buffer, "number");
                auto buf = fmt::format("/buffer {}", num);
                weechat_command(ptr_account->buffer, buf.c_str());
            }
        }
        weechat_string_free_split(jids);
    }
    else if (ptr_channel)
    {
        const std::string &buffer_jid = ptr_channel->id;
        std::string_view nick = ptr_account->nickname();
        ::jid bjid(nullptr, buffer_jid.c_str());
        std::string pres_jid_s = fmt::format("{}@{}/{}", bjid.local, bjid.domain,
                                              nick.empty() ? "" : nick.data());
        pres_jid = pres_jid_s.c_str();

        if (!ptr_account->channels.contains(buffer_jid))
        {
            auto [it_ch, _ins] = ptr_account->channels.emplace(
                std::make_pair(buffer_jid, weechat::channel {
                        *ptr_account, weechat::channel::chat_type::MUC, buffer_jid.c_str(), buffer_jid.c_str()
                    }));
            auto& [_, ch] = *it_ch;
            ptr_channel = &ch;
        }

        xmpp::send_muc_join_presence(*ptr_account, pres_jid, room_password);
    }

    return WEECHAT_RC_OK;
}

int command__open([[maybe_unused]] const void *pointer,
                  [[maybe_unused]] void *data,
                  struct t_gui_buffer *buffer, int argc,
                  char **argv, char **argv_eol)
{
    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;
    char *jid, *text;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    auto ui = weechat::UiPort::for_buffer(buffer);

    if (!ptr_account->connected())
    {
        ui->printf_error(fmt::format(fmt::runtime(_("{}: you are not connected to server")), WEECHAT_XMPP_PLUGIN_NAME));
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
                
                auto [it_ch, _ins] = ptr_account->channels.emplace(
                    std::make_pair(std::string(jid), weechat::channel {
                            *ptr_account, weechat::channel::chat_type::PM, jid, jid
                        }));
                channel = it_ch;

                // Proactively fetch OMEMO devicelist for this contact so that
                // sessions can be established before sending the first message.
                if (ptr_account->omemo)
                    ptr_account->omemo.request_axolotl_devicelist(*ptr_account, jid);
            }

            auto& [_, ch] = *channel;

            if (argc > 2)
            {
                text = argv_eol[2];

                ch.send_message(jid, text);
            }

            int num = weechat_buffer_get_integer(ch.buffer, "number");
            auto buf = fmt::format("/buffer {}", num);
            weechat_command(ptr_account->buffer, buf.c_str());
        }
        weechat_string_free_split(jids);
    }

    return WEECHAT_RC_OK;
}

