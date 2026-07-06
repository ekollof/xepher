bool weechat::connection::handle_omemo_pubsub_iq_event(xmpp_stanza_t *stanza, std::string_view own_jid_str)
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
    const std::string own_jid_storage(own_jid_str);
    const char *own_jid = own_jid_storage.c_str();
    bool handled = false;
        // OMEMO PubSub publish error recovery: if the server rejects our bundle or
        // devicelist publish with <precondition-not-met/>, send a node configure IQ
        // to set access_model=open + persist_items=true, then retry the publish.
        if (type && weechat_strcasecmp(type, "error") == 0 && account.omemo)
        {
            const ::xmpp::StanzaView err_elem = view.child("error");
            if (err_elem.valid())
            {
                const ::xmpp::StanzaView precond = err_elem.child("precondition-not-met", "http://jabber.org/protocol/pubsub#errors");
                if (precond.valid())
                {
                    // Identify the node from the pubsub child of the failed IQ.
                    const ::xmpp::StanzaView failed_pubsub = view.child("pubsub", "http://jabber.org/protocol/pubsub");
                    std::string target_node;
                    if (failed_pubsub.valid())
                    {
                        const ::xmpp::StanzaView pub = failed_pubsub.child("publish");
                        if (pub.valid())
                        {
                            const std::string n = pub.attr_string("node");
                            if (!n.empty()) target_node = n;
                        }
                    }
                    // Also match by known publish IQ id (e.g. "omemo-bundle").
                    if (target_node.empty() && id)
                    {
                        if (std::string_view(id) == "omemo-legacy-bundle")
                            target_node = fmt::format("eu.siacs.conversations.axolotl.bundles:{}",
                                                      account.omemo.device_id);
                        else if (std::string_view(id) == "announce-legacy1")
                            target_node = "eu.siacs.conversations.axolotl.devicelist";
                    }
    
                    if (!target_node.empty())
                    {
                        weechat::UiPort::for_buffer(account.buffer)->printf_network(fmt::format(
                            "omemo: publish to '{}' rejected (precondition-not-met) — "
                            "configuring node and retrying", target_node));
    
                        // Build node configure IQ using fluent builders.
                        const std::string cfg_uuid = stanza::uuid(account.context);
                        auto cfg_iq = stanza::iq()
                            .type("set")
                            .id(cfg_uuid)
                            .to(account.jid())
                            .pubsub_owner(stanza::xep0060::pubsub_owner()
                                .configure(stanza::xep0060::configure(target_node)
                                    .child_spec(stanza::xep0004::form("submit")
                                        .add_hidden("FORM_TYPE",
                                            "http://jabber.org/protocol/pubsub#meta-data")
                                        .add_text("pubsub#access_model", "open")
                                        .add_text("pubsub#persist_items", "true")
                                        .add_text("pubsub#max_items",     "max"))));
    
                        // Remember which node to re-publish after the configure succeeds.
                        if (!cfg_uuid.empty())
                            account.omemo.pending_configure_retry[cfg_uuid] = target_node;
    
                    account.connection.send(cfg_iq.build(account.context).get());
                    handled = true;
                }
            }
        }
    }

    // OMEMO devicelist fetch error handling:
        // - mark missing nodes to avoid request/error loops
        if (type && weechat_strcasecmp(type, "error") == 0 && id && account.omemo)
        {
            const ::xmpp::StanzaView dl_err_elem = view.child("error");
            const ::xmpp::StanzaView dl_pubsub = view.child("pubsub", "http://jabber.org/protocol/pubsub");
            bool is_item_not_found = dl_err_elem.valid() && dl_err_elem.child("item-not-found", "urn:ietf:params:xml:ns:xmpp-stanzas").valid();
            bool is_legacy_devicelist_err = false;
            if (dl_pubsub.valid())
            {
                const ::xmpp::StanzaView dl_items = dl_pubsub.child("items");
                if (dl_items.valid())
                {
                    const std::string dl_node = dl_items.attr_string("node");
                    if (!dl_node.empty() && std::string_view(dl_node) == "eu.siacs.conversations.axolotl.devicelist")
                        is_legacy_devicelist_err = true;
                }
            }
    
            if (is_item_not_found && is_legacy_devicelist_err)
            {
                // Resolve which JID this looked up — use pending_iq_jid first,
                // fall back to bare `from` of the error response.
                std::string dl_target_jid;
                if (auto dl_jid_it = account.omemo.pending_iq_jid.find(id); dl_jid_it != account.omemo.pending_iq_jid.end())
                {
                    auto& [_, jid] = *dl_jid_it;
                    dl_target_jid = jid;
                    account.omemo.pending_iq_jid.erase(dl_jid_it);
                }
                else if (from)
                {
                    const std::string dl_from_bare = ::jid(nullptr, from).bare;
                    if (!dl_from_bare.empty())
                        dl_target_jid = dl_from_bare;
                }
    
            if (!dl_target_jid.empty())
            {
                handled = true;
                bool first_legacy_miss = account.omemo.missing_axolotl_devicelist.insert(dl_target_jid).second;

                if (auto dl_ch_it = account.channels.find(dl_target_jid); dl_ch_it != account.channels.end())
                    {
                        auto& [_, dl_ch] = *dl_ch_it;
                        if (!dl_ch.pending_omemo_messages.empty() && first_legacy_miss)
                        {
                            weechat::UiPort::for_buffer(dl_ch.buffer)->printf_error(fmt::format(
                                "OMEMO: {} has no legacy OMEMO device list either. "
                                "Keeping {} queued message(s).",
                                dl_target_jid, dl_ch.pending_omemo_messages.size()));
                        }
                    }
                }
            }
            else if (!is_item_not_found && is_legacy_devicelist_err)
            {
                // A transient or cross-domain error — clear guard entries so next
                // encode attempt can retry.
                std::string dl_target_jid;
                if (auto dl_jid_it = account.omemo.pending_iq_jid.find(id); dl_jid_it != account.omemo.pending_iq_jid.end())
                {
                    auto& [_, jid] = *dl_jid_it;
                    dl_target_jid = jid;
                    account.omemo.pending_iq_jid.erase(dl_jid_it);
                }
                else if (from)
                {
                    const std::string dl_from_bare = ::jid(nullptr, from).bare;
                    if (!dl_from_bare.empty())
                        dl_target_jid = dl_from_bare;
                }
    
            if (!dl_target_jid.empty())
            {
                handled = true;
                account.omemo.missing_axolotl_devicelist.erase(dl_target_jid);

                // Log the transient error so the user has visibility.
                    const std::string err_text = dl_err_elem.valid()
                        ? dl_err_elem.text() : std::string{};
                    weechat::UiPort::for_buffer(account.buffer)->printf_network(fmt::format(
                        "omemo: transient devicelist fetch error for {} ({}) — "
                        "guard cleared, will retry on next send",
                        dl_target_jid,
                        err_text.empty() ? "unknown error" : err_text));
                }
            }
        }
    
        // After a successful node configure, re-publish the OMEMO bundle or
        // devicelist that originally failed with precondition-not-met.
        if (type && weechat_strcasecmp(type, "result") == 0 && account.omemo && id)
        {
        if (auto cfg_it = account.omemo.pending_configure_retry.find(id); cfg_it != account.omemo.pending_configure_retry.end())
        {
            handled = true;
            auto& [_, retry_node] = *cfg_it;
            account.omemo.pending_configure_retry.erase(cfg_it);
    
                weechat::UiPort::for_buffer(account.buffer)->printf_network(fmt::format(
                    "omemo: node '{}' configured — re-publishing", retry_node));
    
                if (retry_node == "eu.siacs.conversations.axolotl.devicelist")
                {
                    auto dl_stanza = account.get_devicelist();
                    if (dl_stanza)
                        account.connection.send(dl_stanza.get());
                }
                else if (std::string_view(retry_node).starts_with(
                             "eu.siacs.conversations.axolotl.bundles:"))
                {
                    std::string jid_str(account.jid());
                    xmpp_stanza_t *bundle_stanza =
                        account.omemo.get_axolotl_bundle(account.context, jid_str.data(), nullptr);
                    if (bundle_stanza)
                    {
                        (void)send_within_stanza_byte_limit(
                            account.connection, bundle_stanza,
                            k_proxy_safe_stanza_bytes, "OMEMO bundle republish");
                        xmpp_stanza_release(bundle_stanza);
                    }
                }
            }
        }
    
        const ::xmpp::StanzaView pubsub = view.child("pubsub", "http://jabber.org/protocol/pubsub");
        if (pubsub.valid())
        {
    
            // Resolve the node owner JID from pending_iq_jid map, then bare `from`,
            // then bare `to` / own JID. Used by axolotl devicelist/bundle PEP handlers.
            auto resolve_node_owner = [&]() -> std::string {
                std::string owner;
                if (id) {
                    if (auto it = account.omemo.pending_iq_jid.find(id); it != account.omemo.pending_iq_jid.end()) {
                        auto& [_, j] = *it;
                        owner = j;
                        account.omemo.pending_iq_jid.erase(it);
                    }
                }
                if (owner.empty()) {
                    if (from) {
                        const std::string bare = ::jid(nullptr, from).bare;
                        if (!bare.empty())
                            owner = bare;
                    }
                }
                if (owner.empty()) {
                    if (to) {
                        const std::string bare = ::jid(nullptr, to).bare;
                        owner = bare.empty() ? account.jid() : bare;
                    } else {
                        owner = account.jid();
                    }
                }
                return owner;
            };
    
            const ::xmpp::StanzaView items = pubsub.child("items");
            if (items.valid())
            {
                const std::string items_node_s = items.attr_string("node");
                const char *items_node = items_node_s.empty() ? nullptr : items_node_s.c_str();
                if (items_node
                         && weechat_strcasecmp(items_node,
                                               "eu.siacs.conversations.axolotl.devicelist") == 0)
                {
                    // Recover the node-owner JID for this PEP push.
                    std::string node_owner_str = resolve_node_owner();
    
                    const std::string account_bare_s = ::jid(nullptr, account.jid()).bare;
                    const std::string account_bare = account_bare_s.empty()
                        ? account.jid() : account_bare_s;
                    const bool is_own_devicelist = (account_bare == node_owner_str);
    
                    if (account.omemo)
                        account.omemo.handle_axolotl_devicelist(&account,
                                                               node_owner_str.c_str(),
                                                               items.raw());
    
                    if (is_own_devicelist)
                    {
                        bool found_self = false;
                        ::xmpp::StanzaView item = items.child("item");
                        const ::xmpp::StanzaView list = item.valid()
                            ? item.child("list", "eu.siacs.conversations.axolotl")
                            : ::xmpp::StanzaView{};
    
                        if (list.valid()) for (const ::xmpp::StanzaView device : list)
                        {
                            const char *name = device.name().data();
                            if (!name || weechat_strcasecmp(name, "device") != 0)
                                continue;
    
                            const std::string did = device.attr_string("id");
                            const auto parsed_did = parse_omemo_device_id(did);
                            if (parsed_did && *parsed_did == account.omemo.device_id)
                            {
                                found_self = true;
                                break;
                            }
                        }
    
                        if (!found_self)
                        {
                            weechat::UiPort::for_buffer(account.buffer)->printf_network(fmt::format(
                                "omemo: our device {} missing from server legacy devicelist — re-publishing",
                                account.omemo.device_id));
    
                            auto dl_stanza = account.get_devicelist();
                            if (dl_stanza)
                                account.connection.send(dl_stanza.get());
                        }
                    }
                handled = true;
            }
            else if (items_node
                         && std::string_view(items_node).starts_with(
                             "eu.siacs.conversations.axolotl.bundles:"))
                {
                    std::string bundle_jid;
                    if (account.omemo && id)
                    {
                        if (auto it = account.omemo.pending_iq_jid.find(id); it != account.omemo.pending_iq_jid.end())
                        {
                            auto& [_, j] = *it;
                            bundle_jid = j;
                            account.omemo.pending_iq_jid.erase(it);
                        }
                    }
                    if (bundle_jid.empty())
                        bundle_jid = from ? from : own_jid;
    
                    const std::string_view node(items_node);
                    const auto pos = node.find_last_of(':');
                    std::uint32_t bundle_device_id = 0;
                    if (pos != std::string_view::npos && pos + 1 < node.size())
                    {
                        const auto parsed_node_device_id =
                            parse_omemo_device_id(node.substr(pos + 1));
                        if (parsed_node_device_id)
                            bundle_device_id = *parsed_node_device_id;
                    }
    
                    if (bundle_device_id == 0)
                    {
                        const ::xmpp::StanzaView bundle_item = items.child("item");
                        if (bundle_item.valid())
                        {
                            const std::string item_id_str = bundle_item.attr_string("id");
                            const char *item_id = item_id_str.empty() ? nullptr : item_id_str.c_str();
                            const auto parsed_item_device_id = parse_omemo_device_id(item_id);
                            if (parsed_item_device_id)
                                bundle_device_id = *parsed_item_device_id;
                        }
                    }
    
                    if (account.omemo && bundle_device_id != 0)
                        account.omemo.pending_bundle_fetch.erase({bundle_jid, bundle_device_id});
    
                    if (type && weechat_strcasecmp(type, "result") == 0)
                    {
                        if (account.omemo && bundle_device_id != 0)
                            account.omemo.handle_axolotl_bundle(&account,
                                                               account.buffer,
                                                               bundle_jid.c_str(),
                                                               bundle_device_id,
                                                               items.raw());
                        else
                            weechat::UiPort::for_buffer(account.buffer)->printf_error(fmt::format(
                                "omemo: legacy bundle result for {} has missing/invalid device id",
                                bundle_jid));
                    }
                    else if (type && weechat_strcasecmp(type, "error") == 0)
                    {
                        // If our own bundle node is missing on the server
                        // (item-not-found), republish it. needs_bundle_publish()
                        // on connect only fires when local prekeys changed; if
                        // the server lost the node but our prekeys are
                        // unchanged, the bundle would never be republished
                        // without this check, and peers cannot reply with OMEMO.
                        const ::xmpp::StanzaView berr = view.child("error");
                        const std::string own_bare =
                            ::jid(nullptr, account.jid()).bare;
                        const bool own_bundle_missing =
                            berr.valid()
                            && berr.child("item-not-found",
                                          "urn:ietf:params:xml:ns:xmpp-stanzas").valid()
                            && account.omemo
                            && bundle_device_id == account.omemo.device_id
                            && !own_bare.empty()
                            && ::jid(nullptr, bundle_jid).bare == own_bare;

                        if (own_bundle_missing)
                        {
                            weechat::UiPort::for_buffer(account.buffer)->printf_network(fmt::format(
                                "omemo: our bundle node is missing on the server "
                                "— republishing device {}",
                                account.omemo.device_id));
                            std::string jid_str(account.jid());
                            if (auto bundle_sp = account.omemo.build_axolotl_bundle(
                                    account.context, jid_str.data(), nullptr))
                                (void)send_within_stanza_byte_limit(
                                    account.connection, bundle_sp.get(),
                                    k_proxy_safe_stanza_bytes, "OMEMO bundle republish");
                        }
                        else
                        {
                            weechat::UiPort::for_buffer(account.buffer)->printf_error(fmt::format(
                                "omemo: legacy bundle fetch for {}/{} returned error",
                                bundle_jid, bundle_device_id));
                        }
                        if (bundle_device_id != 0 && !bundle_jid.empty())
                        {
                            for (auto &[_, ch] : account.channels)
                            {
                                if (ch.type == weechat::channel::chat_type::MUC
                                    && ch.omemo_recipient_jids.contains(bundle_jid))
                                {
                                    ch.clear_omemo_bundle_pending(bundle_jid, bundle_device_id);
                                }
                            }
                        }
                    }
                handled = true;
                }
            }
        }
    return handled;
}
