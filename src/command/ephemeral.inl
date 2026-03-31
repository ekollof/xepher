// XEP-0466: Ephemeral Messages — send side
// /ephemeral <seconds> <message>
// Sends a regular message stanza with <ephemeral xmlns='urn:xmpp:ephemeral:0' timer='N'/>
// and a <no-permanent-store xmlns='urn:xmpp:hints'/> hint.

int command__ephemeral(const void *pointer, void *data,
                       struct t_gui_buffer *buffer, int argc,
                       char **argv, char **argv_eol)
{
    (void) pointer;
    (void) data;
    (void) argv;

    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;
    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_channel)
    {
        weechat_printf(ptr_account->buffer,
                       "%s%s: /ephemeral can only be used in a chat or MUC buffer",
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        weechat_printf(buffer, "%s%s: you are not connected to server",
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    if (argc < 3)
    {
        weechat_printf(buffer,
                       "%s%s: /ephemeral <seconds> <message>",
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    // Parse timer value
    char *endp = nullptr;
    long timer_secs = std::strtol(argv[1], &endp, 10);
    if (!endp || *endp != '\0' || timer_secs <= 0)
    {
        weechat_printf(buffer, "%s%s: /ephemeral: seconds must be a positive integer",
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    const char *text = argv_eol[2];
    const char *msg_type = ptr_channel->type == weechat::channel::chat_type::MUC
                           ? "groupchat" : "chat";

    xmpp_stanza_t *message = xmpp_message_new(ptr_account->context,
                                               msg_type,
                                               ptr_channel->name.data(),
                                               nullptr);
    xmpp_message_set_body(message, text);

    // Add <ephemeral xmlns='urn:xmpp:ephemeral:0' timer='N'/>
    xmpp_stanza_t *eph = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(eph, "ephemeral");
    xmpp_stanza_set_ns(eph, "urn:xmpp:ephemeral:0");
    std::string timer_str = std::to_string(timer_secs);
    xmpp_stanza_set_attribute(eph, "timer", timer_str.c_str());
    xmpp_stanza_add_child(message, eph);
    xmpp_stanza_release(eph);

    // Add <no-permanent-store xmlns='urn:xmpp:hints'/> so servers don't archive it
    xmpp_stanza_t *hint = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(hint, "no-permanent-store");
    xmpp_stanza_set_ns(hint, "urn:xmpp:hints");
    xmpp_stanza_add_child(message, hint);
    xmpp_stanza_release(hint);

    ptr_account->connection.send(message);
    xmpp_stanza_release(message);

    // Echo locally for PM buffers (not MUC — server echoes those back)
    if (ptr_channel->type != weechat::channel::chat_type::MUC)
    {
        weechat_printf_date_tags(ptr_channel->buffer, 0,
                                 "xmpp_message,message,private,notify_none,self_msg,log1",
                                 "%s\t[⏱ %lds] %s",
                                 weechat::user::search(ptr_account,
                                     ptr_account->jid().data())->as_prefix_raw().data(),
                                 timer_secs, text);
    }

    return WEECHAT_RC_OK;
}
