bool weechat::connection::handle_avatar_pubsub_iq_event(
    xmpp_stanza_t *stanza,
    std::string_view own_jid_str)
{
    const ::xmpp::StanzaView view(stanza);
    const std::string iq_from_str = view.attr_string("from");
    const std::string iq_type_str = view.attr_string("type");
    const char *from = iq_from_str.empty() ? nullptr : iq_from_str.c_str();
    const char *type = iq_type_str.empty() ? nullptr : iq_type_str.c_str();
    if (!type || weechat_strcasecmp(type, "result") != 0)
        return false;

    const auto pubsub = view.child("pubsub", "http://jabber.org/protocol/pubsub");
    if (!pubsub.valid())
        return false;

    const auto items = pubsub.child("items");
    if (!items.valid())
        return false;

    const std::string items_node = items.attr_string("node");
    if (items_node.empty() || std::string_view(items_node) != ::xmpp::k_pep_avatar_data)
        return false;

    const auto item = items.child("item");
    if (!item.valid())
        return false;

    const auto data_elem = item.child("data", ::xmpp::k_pep_avatar_data);
    if (!data_elem.valid())
        return false;

    const std::string b64_data = data_elem.text();
    if (b64_data.empty())
        return false;

    const char *from_jid = from ? from : own_jid_str.data();
    if (!weechat::avatar::apply_pep_data_b64(account, from_jid, b64_data))
        return false;

    XDEBUG("Avatar data IQ result from {}", from_jid);
    return true;
}