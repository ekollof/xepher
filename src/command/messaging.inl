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

    if (argc < 2)
    {
        weechat_printf(buffer,
                        _("%s%s: missing argument for \"%s\" command"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, "invite");
        return WEECHAT_RC_OK;
    }

    const char *invitee_jid = argv[1];
    const char *reason = argc > 2 ? argv_eol[2] : nullptr;

    auto inv_msg = stanza::message().to(invitee_jid);
    static_cast<stanza::xep0249::message&>(inv_msg).invite(
        ptr_channel->name.data(), nullptr, reason);
    ptr_account->connection.send(inv_msg.build(ptr_account->context).get());

    weechat_printf(buffer,
                    _("%sInvited %s to %s"),
                    weechat_prefix("network"),
                    invitee_jid,
                    ptr_channel->name.data());

    return WEECHAT_RC_OK;
}

