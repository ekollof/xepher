bool weechat::connection::handle_ping_iq_event(xmpp_stanza_t *stanza, std::string_view own_jid_str)
{
    const char *from = xmpp_stanza_get_from(stanza);
    const char *type = xmpp_stanza_get_attribute(stanza, "type");
    const ::xmpp::StanzaView view(stanza);

    if (::xmpp::is_ping_get_iq(view))
    {
        auto reply = ::xmpp::handle_ping_iq(view, own_jid_str);
        account.connection.send(reply.build(account.context).get());
        return true;
    }

    if (!type
        || (weechat_strcasecmp(type, "result") != 0
            && weechat_strcasecmp(type, "error") != 0))
        return false;

    const char *stanza_id = xmpp_stanza_get_id(stanza);
    auto ping_it = stanza_id ? account.user_ping_queries.find(stanza_id)
                             : account.user_ping_queries.end();
    if (ping_it == account.user_ping_queries.end())
        return false;

    auto& [_, start_time] = *ping_it;
    const long rtt_ms = ::xmpp::compute_ping_rtt_ms(start_time, time(nullptr));
    account.user_ping_queries.erase(ping_it);

    const char *from_jid = from ? from : own_jid_str.data();

    const auto muc_from = from ? ::xmpp::parse_muc_ping_from(from) : std::nullopt;
    const bool is_muc_selfping = muc_from && ::xmpp::is_muc_self_ping(
        *muc_from,
        account.nickname(),
        [&](const std::string_view room) {
            return account.channels.contains(std::string(room));
        });
    const std::string room_jid = muc_from ? muc_from->room_jid : std::string{};

    if (weechat_strcasecmp(type, "result") == 0)
    {
        if (is_muc_selfping)
        {
            weechat_printf(account.buffer, "%sMUC self-ping OK: still in %s",
                           weechat_prefix("network"), room_jid.c_str());
        }
        else
        {
            weechat_printf(account.buffer, "%sPong from %s (RTT: %ld ms)",
                           weechat_prefix("network"), from_jid, rtt_ms);
        }
        return true;
    }

    xmpp_stanza_t *err_elem = xmpp_stanza_get_child_by_name(stanza, "error");
    const ::xmpp::StanzaView err_view(err_elem);

    if (is_muc_selfping)
    {
        switch (::xmpp::classify_muc_self_ping_error(err_view))
        {
        case ::xmpp::MucSelfPingErrorOutcome::still_joined:
            weechat_printf(account.buffer,
                           "%sMUC self-ping: still in %s (reflected error)",
                           weechat_prefix("network"), room_jid.c_str());
            break;
        case ::xmpp::MucSelfPingErrorOutcome::ambiguous:
        {
            const char *etype = err_elem
                ? xmpp_stanza_get_attribute(err_elem, "type") : "unknown";
            weechat_printf(account.buffer,
                           "%sMUC self-ping to %s: network error (%s), skipping rejoin",
                           weechat_prefix("network"), room_jid.c_str(), etype);
            break;
        }
        case ::xmpp::MucSelfPingErrorOutcome::not_joined:
            weechat_printf(account.buffer,
                           "%sMUC self-ping FAILED: no longer in %s — rejoining",
                           weechat_prefix("error"), room_jid.c_str());
            {
                const std::string rejoin_jid = fmt::format("{}/{}",
                                                           room_jid,
                                                           account.nickname());
                auto join_pres = stanza::presence()
                    .to(rejoin_jid.c_str())
                    .from(account.jid());
                static_cast<stanza::xep0045::presence&>(join_pres).muc_join();
                account.connection.send(join_pres.build(account.context).get());
            }
            break;
        }
        return true;
    }

    const char *error_type = err_elem
        ? xmpp_stanza_get_attribute(err_elem, "type") : "unknown";
    weechat_printf(account.buffer, "%sPing failed to %s: %s",
                   weechat_prefix("error"), from_jid, error_type);
    return true;
}