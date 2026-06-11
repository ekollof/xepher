void sm_start_ack_timer(weechat::account &account)
{
    if (account.sm_ack_timer_hook)
    {
        weechat_unhook(account.sm_ack_timer_hook);
        account.sm_ack_timer_hook = nullptr;
    }

    account.sm_ack_timer_hook = static_cast<struct t_hook *>(weechat_hook_timer(
        30 * 1000, 0, 0, &weechat::account::sm_ack_timer_cb, &account, nullptr));
}

namespace {

void sm_replay_pending_stanzas(weechat::connection &connection, weechat::account &account)
{
    if (!account.sm_enabled || account.sm_pending_replay.empty())
        return;

    weechat::UiPort::for_buffer(account.buffer)->printf_network(fmt::format(
        "Re-sending {} stanza(s) after fresh SM enable…",
        account.sm_pending_replay.size()));

    auto pending = std::move(account.sm_pending_replay);
    account.sm_pending_replay.clear();
    for (const auto &stanza_copy : pending)
        connection.send(stanza_copy.get());
}

} // namespace

void weechat::connection::send_sm_graceful_ack()
{
    if (!account.sm_enabled || !xmpp_conn_is_connected(*this))
        return;

    send(stanza::xep0198::answer(account.sm_h_inbound)
        .build(account.context)
        .get());
}

bool weechat::connection::sm_handler(xmpp_stanza_t *stanza)
{
    const ::xmpp::StanzaView view(stanza);
    const std::string iq_id_str = view.attr_string("id");
    const std::string iq_from_str = view.attr_string("from");
    const std::string iq_to_str = view.attr_string("to");
    const std::string iq_type_str = view.attr_string("type");
    const char *id = iq_id_str.empty() ? nullptr : iq_id_str.c_str();
    const char *from = iq_from_str.empty() ? nullptr : iq_from_str.c_str();
    const char *to = iq_to_str.empty() ? nullptr : iq_to_str.c_str();
    const char *type = iq_type_str.empty() ? nullptr : iq_type_str.c_str();
    (void)id; (void)from; (void)to; (void)type;

    const auto xmlns_opt = view.xmlns();
    if (!xmlns_opt || *xmlns_opt != "urn:xmpp:sm:3")
        return true;

    const char *name = view.name().data();
    if (!name)
        return true;

    const std::string element_name(name);

    if (element_name == "enabled")
    {
        account.sm_enabled = true;
        account.sm_awaiting_negotiation = false;
        account.sm_h_inbound = 0;
        account.sm_h_outbound = 0;
        account.sm_last_ack = 0;
        account.sm_outqueue.clear();

        const std::string session_id = view.attr_string("id");
        if (!session_id.empty())
        {
            account.sm_id = session_id;
            XDEBUG("Stream Management enabled (resumable, id={})", session_id);
        }
        else
        {
            XDEBUG("Stream Management enabled (not resumable)");
        }

        sm_start_ack_timer(account);
        sm_replay_pending_stanzas(*this, account);
        run_post_connect_setup(false);
    }
    else if (element_name == "resumed")
    {
        const std::string previd = view.attr_string("previd");
        if (!previd.empty() && !account.sm_id.empty() && previd != account.sm_id)
        {
            XDEBUG("SM resumed: previd mismatch (expected {}, got {})",
                   account.sm_id, previd);
        }

        const std::string h = view.attr_string("h");
        uint32_t ack_h = 0;
        if (!h.empty())
        {
            ack_h = parse_uint32(h).value_or(0);
            account.sm_last_ack = ack_h;
            XDEBUG("Stream resumed (h={})", ack_h);
        }
        else
        {
            XDEBUG("Stream resumed");
        }

        account.sm_enabled = true;
        account.sm_awaiting_negotiation = false;
        sm_start_ack_timer(account);

        while (!account.sm_outqueue.empty()
               && account.sm_outqueue.front().first <= ack_h)
        {
            account.sm_outqueue.pop_front();
        }

        if (!account.sm_outqueue.empty())
        {
            weechat::UiPort::for_buffer(account.buffer)->printf_network(fmt::format(
                "Retransmitting {} unacknowledged stanza(s)…",
                account.sm_outqueue.size()));
            for (auto &[_, stanza_copy] : account.sm_outqueue)
                m_conn.send(stanza_copy.get());
        }

        sm_replay_pending_stanzas(*this, account);
        run_post_connect_setup(true);
    }
    else if (element_name == "failed")
    {
        const char *xmlns_err = "urn:ietf:params:xml:ns:xmpp-stanzas";
        ::xmpp::StanzaView error = view.child("unexpected-request", xmlns_err);
        if (!error.valid())
            error = view.child("item-not-found", xmlns_err);

        auto ui = weechat::UiPort::for_buffer(account.buffer);
        if (error.valid())
        {
            const char *error_name = error.name().data();
            ui->printf_error(fmt::format(
                "SM error: {}", error_name ? error_name : "unknown"));
        }
        else
        {
            ui->printf_error("Stream Management failed");
        }

        if (account.sm_resume_attempted && account.sm_available)
        {
            account.sm_resume_attempted = false;
            account.sm_enabled = false;
            account.sm_id.clear();
            account.sm_h_inbound = 0;
            account.sm_last_ack = 0;

            while (!account.sm_outqueue.empty())
            {
                account.sm_pending_replay.push_back(account.sm_outqueue.front().second);
                account.sm_outqueue.pop_front();
            }
            account.sm_h_outbound = 0;

            ui->printf_network(
                "SM resume failed — enabling a fresh stream management session");
            account.sm_awaiting_negotiation = true;
            send(stanza::xep0198::enable(true, 300)
                .build(account.context)
                .get());
            return true;
        }

        account.sm_enabled = false;
        account.sm_awaiting_negotiation = false;
        account.sm_id.clear();
        account.sm_h_inbound = 0;
        account.sm_h_outbound = 0;
        account.sm_last_ack = 0;
        account.sm_outqueue.clear();
        account.sm_pending_replay.clear();

        ui->printf_network(
            "Stream Management session ended (will retry on next connect)");

        if (!account.sm_post_connect_done)
            run_post_connect_setup(false);
    }
    else if (element_name == "a")
    {
        const std::string h = view.attr_string("h");
        if (h.empty())
        {
            weechat::UiPort::for_buffer(account.buffer)->printf_error(
                "SM error: 'a' stanza missing 'h' attribute");
            return true;
        }

        const uint32_t ack_count = parse_uint32(h).value_or(0);
        if (ack_count > account.sm_h_outbound)
        {
            weechat::UiPort::for_buffer(account.buffer)->printf_error(fmt::format(
                "SM error: server ack h={} exceeds sent count {}",
                ack_count, account.sm_h_outbound));
            return true;
        }

        account.sm_last_ack = ack_count;

        while (!account.sm_outqueue.empty()
               && account.sm_outqueue.front().first <= ack_count)
        {
            account.sm_outqueue.pop_front();
        }

        const int32_t unacked = static_cast<int32_t>(account.sm_h_outbound)
            - static_cast<int32_t>(ack_count);
        const time_t now = time(nullptr);
        if (unacked > 0 || (now - account.sm_last_ack_log) > 300)
        {
            XDEBUG("Received ack: h={} (sent={}, unacked={})",
                   ack_count,
                   account.sm_h_outbound,
                   std::max<int32_t>(unacked, 0));
            account.sm_last_ack_log = now;
        }
    }
    else if (element_name == "r")
    {
        send(stanza::xep0198::answer(account.sm_h_inbound)
            .build(account.context)
            .get());
    }

    return true;
}