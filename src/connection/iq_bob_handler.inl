bool weechat::connection::handle_bob_iq_event(xmpp_stanza_t *stanza,
                                                std::string_view own_jid_str)
{
    const ::xmpp::StanzaView view(stanza);
    const std::string iq_id_str = view.attr_string("id");
    const std::string iq_type_str = view.attr_string("type");
    const char *id = iq_id_str.empty() ? nullptr : iq_id_str.c_str();
    const char *type = iq_type_str.empty() ? nullptr : iq_type_str.c_str();

    if (!id || !type)
        return false;

    if (weechat_strcasecmp(type, "get") == 0 && ::xmpp::is_bob_iq_get(view))
    {
        const std::string cid = view.child("data", ::xmpp::k_bob_ns).attr_string("cid");
        const ::xmpp::BobHostedPayload *hosted = nullptr;
        if (auto entry = ::xmpp::bob_host_lookup(account, cid))
            hosted = &*entry;
        if (auto reply = ::xmpp::handle_bob_iq_get(view, own_jid_str, hosted))
        {
            account.connection.send(reply->build(account.context).get());
            XDEBUG("BoB IQ-get served for cid {}", cid);
            return true;
        }
        return false;
    }

    if (weechat_strcasecmp(type, "error") == 0)
    {
        if (!account.bob_fetch_queries.contains(id))
            return false;
        const auto ctx = account.bob_fetch_queries[id];
        account.bob_fetch_queries.erase(id);
        account.bob_inflight_cids.erase(ctx.cid);
        account.bob_deferred_icat.erase(ctx.cid);
        XDEBUG("BoB fetch failed for cid {} (iq {})", ctx.cid, id);
        return true;
    }

    if (weechat_strcasecmp(type, "result") != 0)
        return false;
    if (!account.bob_fetch_queries.contains(id))
        return false;

    const ::xmpp::StanzaView data_elem = view.child("data", ::xmpp::k_bob_ns);
    if (!data_elem.valid())
        return false;
    const std::string b64_data = data_elem.text();
    if (b64_data.empty())
        return false;
    const std::string mime = data_elem.attr_string("type");
    const auto bytes = ::xmpp::bob_decode_base64(b64_data);
    if (bytes.empty())
        return false;
    ::xmpp::bob_complete_fetch_iq(account, id, mime.empty() ? "application/octet-stream" : mime, bytes);
    XDEBUG("BoB fetch completed (iq {})", id);
    return true;
}
