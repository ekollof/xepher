bool weechat::connection::handle_bob_iq_event(xmpp_stanza_t *stanza)
{
    const char *id = xmpp_stanza_get_id(stanza);
    const char *type = xmpp_stanza_get_attribute(stanza, "type");
    if (!id || !type)
        return false;

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

    xmpp_stanza_t *data_elem = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "data", ::xmpp::k_bob_ns.data());
    if (!data_elem)
        return false;

    const std::string b64_data = stanza_element_text(data_elem);
    if (b64_data.empty())
        return false;

    const std::string mime = get_attribute(data_elem, "type").value_or("application/octet-stream");
    const auto bytes = ::xmpp::bob_decode_base64(b64_data);
    if (bytes.empty())
        return false;

    ::xmpp::bob_complete_fetch_iq(account, id, mime, bytes);
    XDEBUG("BoB fetch completed (iq {})", id);
    return true;
}