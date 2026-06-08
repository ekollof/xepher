bool weechat::connection::handle_avatar_pubsub_iq_event(
    xmpp_stanza_t *stanza,
    std::string_view own_jid_str)
{
    const char *from = xmpp_stanza_get_from(stanza);
    const char *type = xmpp_stanza_get_attribute(stanza, "type");
    if (!type || weechat_strcasecmp(type, "result") != 0)
        return false;

    xmpp_stanza_t *pubsub = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "pubsub", "http://jabber.org/protocol/pubsub");
    if (!pubsub)
        return false;

    xmpp_stanza_t *items = xmpp_stanza_get_child_by_name(pubsub, "items");
    if (!items)
        return false;

    const char *items_node = xmpp_stanza_get_attribute(items, "node");
    if (!items_node || std::string_view(items_node) != ::xmpp::k_pep_avatar_data)
        return false;

    xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(items, "item");
    if (!item)
        return false;

    xmpp_stanza_t *data_elem = xmpp_stanza_get_child_by_name_and_ns(
        item, "data", ::xmpp::k_pep_avatar_data.data());
    if (!data_elem)
        return false;

    const std::string b64_data = stanza_element_text(data_elem);
    if (b64_data.empty())
        return false;

    const char *from_jid = from ? from : own_jid_str.data();
    if (!weechat::avatar::apply_pep_data_b64(account, from_jid, b64_data))
        return false;

    XDEBUG("Avatar data IQ result from {}", from_jid);
    return true;
}