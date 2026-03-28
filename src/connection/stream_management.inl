bool weechat::connection::sm_handler(xmpp_stanza_t *stanza)
{
    // CRITICAL: Verify this is actually an SM stanza by checking xmlns
    const char *xmlns = xmpp_stanza_get_ns(stanza);
    if (!xmlns || strcmp(xmlns, "urn:xmpp:sm:3") != 0)
    {
        // Not an SM stanza, ignore
        return true;
    }

    const char *name = xmpp_stanza_get_name(stanza);
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

        const char *id = xmpp_stanza_get_attribute(stanza, "id");
        if (id)
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
        const char *h = xmpp_stanza_get_attribute(stanza, "h");
        uint32_t ack_h = 0;
        if (h)
        {
            ack_h = std::stoul(h);
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
            weechat_printf(account.buffer, "%sRetransmitting %zu unacknowledged stanza(s)…",
                          weechat_prefix("network"), account.sm_outqueue.size());
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
        xmpp_stanza_t *error = xmpp_stanza_get_child_by_name_and_ns(stanza, "unexpected-request", xmlns_err);
        
        if (!error)
            error = xmpp_stanza_get_child_by_name_and_ns(stanza, "item-not-found", xmlns_err);
        
        if (error)
        {
            const char *error_name = xmpp_stanza_get_name(error);
            weechat_printf(account.buffer, "%sSM error: %s",
                          weechat_prefix("error"), error_name ? error_name : "unknown");
        }
        else
        {
            weechat_printf(account.buffer, "%sStream Management failed",
                          weechat_prefix("error"));
        }

        // Reset SM state (but don't try to enable again this session)
        account.sm_enabled = false;
        account.sm_id = "";
        account.sm_h_inbound = 0;
        account.sm_h_outbound = 0;
        account.sm_last_ack = 0;
        account.sm_outqueue.clear();
        
        // Mark SM as unavailable to prevent retry loops
        // (Will be reset when user manually reconnects)
        account.sm_available = false;
        
        weechat_printf(account.buffer, "%sStream Management disabled for this session",
                      weechat_prefix("network"));
    }
    else if (element_name == "a")
    {
        // Acknowledgement from server
        const char *h = xmpp_stanza_get_attribute(stanza, "h");
        if (h)
        {
            uint32_t ack_count = std::stoul(h);
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
            time_t now = time(NULL);
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
            weechat_printf(account.buffer, "%sSM error: 'a' stanza missing 'h' attribute",
                          weechat_prefix("error"));
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
