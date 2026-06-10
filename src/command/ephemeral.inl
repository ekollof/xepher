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

    auto ui = weechat::UiPort::for_buffer(buffer);

    if (!ptr_channel)
    {
        ui->printf_error(fmt::format(
            "{}: /ephemeral can only be used in a chat or MUC buffer",
            WEECHAT_XMPP_PLUGIN_NAME));
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        ui->printf_error(fmt::format(
            "{}: you are not connected to server", WEECHAT_XMPP_PLUGIN_NAME));
        return WEECHAT_RC_OK;
    }

    if (argc < 3)
    {
        ui->printf_error(fmt::format(
            "{}: /ephemeral <seconds> <message>", WEECHAT_XMPP_PLUGIN_NAME));
        return WEECHAT_RC_OK;
    }

    // Parse timer value
    const auto timer_parsed = parse_int64(argv[1]);
    if (!timer_parsed || *timer_parsed <= 0)
    {
        ui->printf_error(fmt::format(
            "{}: /ephemeral: seconds must be a positive integer",
            WEECHAT_XMPP_PLUGIN_NAME));
        return WEECHAT_RC_OK;
    }
    const long timer_secs = *timer_parsed;

    const char *text = argv_eol[2];
    const char *msg_type = ptr_channel->type == weechat::channel::chat_type::MUC
                           ? "groupchat" : "chat";

    auto eph_msg = stanza::message().type(msg_type).to(ptr_channel->name).body(text);
    static_cast<stanza::xep0466::message&>(eph_msg).ephemeral(timer_secs);
    static_cast<stanza::xep0333::message&>(eph_msg).no_permanent_store();
    ptr_account->connection.send(eph_msg.build(ptr_account->context).get());

    // Echo locally for PM buffers (not MUC — server echoes those back)
    if (ptr_channel->type != weechat::channel::chat_type::MUC)
    {
        weechat::UiPort::for_buffer(ptr_channel->buffer)->printf_date_tags(
            0, "xmpp_message,message,private,notify_none,self_msg,log1",
            fmt::format("{}\t[⏱ {}s] {}",
                        weechat::user::search(ptr_account,
                            ptr_account->jid().data())->as_prefix_raw().data(),
                        timer_secs, text));
    }

    return WEECHAT_RC_OK;
}
