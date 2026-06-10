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
    // CRITICAL: Verify this is actually an SM stanza by checking xmlns
    const auto xmlns_opt = view.xmlns();
    if (!xmlns_opt || *xmlns_opt != "urn:xmpp:sm:3")
    {
        // Not an SM stanza, ignore
        return true;
    }

    const char *name = view.name().data();
    if (!name)
        return true;

    std::string element_name(name);

    if (element_name == "enabled")
    {
        // Stream management successfully enabled
        account.sm_enabled = true;
        account.sm_h_inbound = 0;
        account.sm_h_outbound = 0;
        account.sm_last_ack = 0;
        account.sm_outqueue.clear();

        const std::string id = view.attr_string("id");
        if (!id.empty())
        {
            account.sm_id = id;
            XDEBUG("Stream Management enabled (resumable, id={})", id);
        }
        else
        {
            XDEBUG("Stream Management enabled (not resumable)");
        }

        // Set up periodic ack timer (every 30 seconds)
        account.sm_ack_timer_hook = (struct t_hook *)weechat_hook_timer(30 * 1000, 0, 0,
                                   &account::sm_ack_timer_cb, &account, nullptr);
    }
    else if (element_name == "resumed")
    {
        // Stream resumed successfully
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

        // Prune stanzas the server already acknowledged
        while (!account.sm_outqueue.empty() &&
               account.sm_outqueue.front().first <= ack_h)
        {
            account.sm_outqueue.pop_front();
        }

        // Retransmit all remaining unacknowledged stanzas
        if (!account.sm_outqueue.empty())
        {
            weechat::UiPort::for_buffer(account.buffer)->printf_network(fmt::format(
                "Retransmitting {} unacknowledged stanza(s)…",
                account.sm_outqueue.size()));
            for (auto& [seq, stanza_copy] : account.sm_outqueue)
            {
                m_conn.send(stanza_copy.get());
            }
        }
    }
    else if (element_name == "failed")
    {
        // Stream management failed or resume failed
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

        // Reset SM state for this session. We deliberately do *not* poison
        // sm_available here — a transient <failed> (common after network
        // outages when the server has dropped the old SM session) should not
        // permanently disable Stream Management for future auto-reconnects.
        account.sm_enabled = false;
        account.sm_id = "";
        account.sm_h_inbound = 0;
        account.sm_h_outbound = 0;
        account.sm_last_ack = 0;
        account.sm_outqueue.clear();
        
        ui->printf_network(
            "Stream Management session ended (will retry on next connect)");
    }
    else if (element_name == "a")
    {
        // Acknowledgement from server
        const std::string h = view.attr_string("h");
        if (!h.empty())
        {
            const uint32_t ack_count = parse_uint32(h).value_or(0);
            account.sm_last_ack = ack_count;

            // Prune all stanzas the server has confirmed receiving
            while (!account.sm_outqueue.empty() &&
                   account.sm_outqueue.front().first <= ack_count)
            {
                account.sm_outqueue.pop_front();
            }
            
            // Guard against underflow
            int32_t unacked = (int32_t)account.sm_h_outbound - (int32_t)ack_count;
            if (unacked < 0) unacked = 0;
            
            // Only log if there were unacked stanzas or if it's been a while
            static time_t last_ack_log = 0;
            time_t now = time(nullptr);
            if (unacked > 0 || (now - last_ack_log) > 300)  // Log every 5 minutes if quiet
            {
                XDEBUG("Received ack: h={} (sent={}, unacked={})",
                       ack_count,
                       account.sm_h_outbound,
                       unacked);
                last_ack_log = now;
            }
        }
        else
        {
            weechat::UiPort::for_buffer(account.buffer)->printf_error(
                "SM error: 'a' stanza missing 'h' attribute");
        }
    }
    else if (element_name == "r")
    {
        // Server requests acknowledgement
        // Send answer with our current h value
        this->send(stanza::xep0198::answer(account.sm_h_inbound)
                  .build(account.context)
                  .get());
    }
