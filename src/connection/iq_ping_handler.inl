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

    auto ui = weechat::UiPort::for_buffer(account.buffer);

    if (weechat_strcasecmp(type, "result") == 0)
    {
        if (is_muc_selfping)
        {
            ui->printf_network(fmt::format(
                "MUC self-ping OK: still in {}", room_jid));
        }
        else
        {
            ui->printf_network(fmt::format(
                "Pong from {} (RTT: {} ms)", from_jid, rtt_ms));
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
            ui->printf_network(fmt::format(
                "MUC self-ping: still in {} (reflected error)", room_jid));
            break;
        case ::xmpp::MucSelfPingErrorOutcome::ambiguous:
        {
            const char *etype = err_elem
                ? xmpp_stanza_get_attribute(err_elem, "type") : "unknown";
            ui->printf_network(fmt::format(
                "MUC self-ping to {}: network error ({}), skipping rejoin",
                room_jid, etype));
            break;
        }
        case ::xmpp::MucSelfPingErrorOutcome::not_joined:
            ui->printf_error(fmt::format(
                "MUC self-ping FAILED: no longer in {} — rejoining", room_jid));
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
    ui->printf_error(fmt::format("Ping failed to {}: {}", from_jid, error_type));
    return true;
}