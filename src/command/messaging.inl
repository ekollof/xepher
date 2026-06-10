int command__msg(const void *pointer, void *data,
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
        weechat_printf(
            ptr_account->buffer,
            _("%s%s: \"%s\" command can not be executed on a account buffer"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, "msg");
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        weechat_printf(buffer,
                        _("%s%s: you are not connected to server"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    if (argc > 1)
    {
        const char *text = argv_eol[1];
        const char *msg_type = ptr_channel->type == weechat::channel::chat_type::MUC
                               ? "groupchat" : "chat";

        auto msg_s = stanza::message().type(msg_type).to(ptr_channel->name).body(text);
        ptr_account->connection.send(msg_s.build(ptr_account->context).get());
        if (ptr_channel->type != weechat::channel::chat_type::MUC)
            weechat_printf_date_tags(ptr_channel->buffer, 0,
                                     "xmpp_message,message,private,notify_none,self_msg,log1",
                                     "%s\t%s",
                                     weechat::user::search(ptr_account, ptr_account->jid().data())->as_prefix_raw().data(),
                                     text);
    }

    return WEECHAT_RC_OK;
}

int command__me(const void *pointer, void *data,
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
        weechat_printf(
            ptr_account->buffer,
            _("%s%s: \"%s\" command can not be executed on a account buffer"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, "me");
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        weechat_printf(buffer,
                        _("%s%s: you are not connected to server"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    if (argc > 1)
    {
        std::string me_text = std::string("/me ") + argv_eol[1];
        return ptr_channel->send_message(ptr_channel->name, me_text);
    }

    return WEECHAT_RC_OK;
}

int command__invite(const void *pointer, void *data,
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
        weechat_printf(
            ptr_account->buffer,
            _("%s%s: \"%s\" command can only be executed in a MUC buffer"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, "invite");
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        weechat_printf(buffer,
                        _("%s%s: you are not connected to server"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    bool mediated = false;
    int invitee_arg = 1;
    for (int i = 1; i < argc; ++i)
    {
        if (std::string_view{argv[i]} == "--mediated")
            mediated = true;
        else
        {
            invitee_arg = i;
            break;
        }
    }

    if (argc < 2 || invitee_arg >= argc
        || std::string_view{argv[invitee_arg]} == "--mediated")
    {
        weechat_printf(buffer,
                        _("%s%s: usage: /invite [--mediated] <jid> [<reason>]"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    const char *invitee_jid = argv[invitee_arg];
    const char *reason = (invitee_arg + 1 < argc) ? argv_eol[invitee_arg + 1] : nullptr;

    if (mediated)
    {
        auto inv_msg = stanza::message().to(ptr_channel->id).mediated_invite(
            invitee_jid, reason ? std::string_view(reason) : std::string_view{});
        ptr_account->connection.send(inv_msg.build(ptr_account->context).get());

        weechat_printf(buffer, "%s%s",
                       weechat_prefix("network"),
                       fmt::format("Mediated invite sent for {} to join {}",
                                   invitee_jid, ptr_channel->name).c_str());
    }
    else
    {
        auto inv_msg = stanza::message().to(invitee_jid);
        static_cast<stanza::xep0249::message&>(inv_msg).invite(
            ptr_channel->name.data(), nullptr, reason);
        ptr_account->connection.send(inv_msg.build(ptr_account->context).get());

        weechat_printf(buffer,
                        _("%sInvited %s to %s"),
                        weechat_prefix("network"),
                        invitee_jid,
                        ptr_channel->name.data());
    }

    return WEECHAT_RC_OK;
}

int command__decline(const void *pointer, void *data,
                     struct t_gui_buffer *buffer, int argc,
                     char **argv, char **argv_eol)
{
    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;

    (void) pointer;
    (void) data;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);
    (void) ptr_channel;

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_account->connected())
    {
        weechat_printf(buffer,
                        _("%s%s: you are not connected to server"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    if (ptr_account->pending_mediated_invites.empty())
    {
        weechat_printf(buffer,
                        _("%s%s: no pending mediated MUC invitations"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    std::optional<std::size_t> match_idx;
    const char *reason = nullptr;

    auto find_pending = [&](std::string_view room_jid, std::string_view inviter)
        -> std::optional<std::size_t>
    {
        for (std::size_t i = 0; i < ptr_account->pending_mediated_invites.size(); ++i)
        {
            const auto& pending = ptr_account->pending_mediated_invites[i];
            if (pending.room_jid == room_jid && pending.inviter_bare == inviter)
                return i;
        }
        return std::nullopt;
    };

    if (argc < 2)
    {
        match_idx = ptr_account->pending_mediated_invites.size() - 1;
    }
    else if (argc == 2)
    {
        const std::string_view arg1{argv[1]};
        if (arg1.contains('@'))
        {
            weechat_printf(buffer,
                            _("%s%s: usage: /decline [<room> <inviter> [<reason>]]"),
                            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
            return WEECHAT_RC_OK;
        }
        match_idx = ptr_account->pending_mediated_invites.size() - 1;
        reason = argv_eol[1];
    }
    else if (argc == 3)
    {
        const std::string_view room_jid{argv[1]};
        const std::string_view inviter{argv[2]};
        match_idx = find_pending(room_jid, inviter);
        if (!match_idx)
        {
            weechat_printf(buffer, "%s%s",
                           weechat_prefix("error"),
                           fmt::format("{}: no pending invite from {} for {}",
                                       WEECHAT_XMPP_PLUGIN_NAME,
                                       inviter, room_jid).c_str());
            return WEECHAT_RC_OK;
        }
    }
    else
    {
        const std::string_view room_jid{argv[1]};
        const std::string_view inviter{argv[2]};
        match_idx = find_pending(room_jid, inviter);
        if (!match_idx)
        {
            weechat_printf(buffer, "%s%s",
                           weechat_prefix("error"),
                           fmt::format("{}: no pending invite from {} for {}",
                                       WEECHAT_XMPP_PLUGIN_NAME,
                                       inviter, room_jid).c_str());
            return WEECHAT_RC_OK;
        }
        reason = argv_eol[3];
    }

    const auto pending = ptr_account->pending_mediated_invites[*match_idx];
    auto dec_msg = stanza::message().to(pending.room_jid).mediated_decline(
        pending.inviter_bare,
        reason ? std::string_view(reason) : std::string_view{});
    ptr_account->connection.send(dec_msg.build(ptr_account->context).get());

    ptr_account->pending_mediated_invites.erase(
        ptr_account->pending_mediated_invites.begin()
        + static_cast<std::ptrdiff_t>(*match_idx));

    weechat_printf(buffer, "%s%s",
                   weechat_prefix("network"),
                   fmt::format("Declined invitation to {} from {}",
                               pending.room_jid, pending.inviter_bare).c_str());
    return WEECHAT_RC_OK;
}

