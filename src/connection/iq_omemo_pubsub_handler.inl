bool weechat::connection::handle_omemo_pubsub_iq_event(xmpp_stanza_t *stanza, std::string_view own_jid_str)
{
    const char *id = xmpp_stanza_get_id(stanza);
    const char *from = xmpp_stanza_get_from(stanza);
    const char *to = xmpp_stanza_get_to(stanza);
    const char *type = xmpp_stanza_get_attribute(stanza, "type");
    const std::string own_jid_storage(own_jid_str);
    const char *own_jid = own_jid_storage.c_str();
    xmpp_stanza_t *pubsub = nullptr, *items = nullptr, *item = nullptr;
    bool handled = false;
        // OMEMO PubSub publish error recovery: if the server rejects our bundle or
        // devicelist publish with <precondition-not-met/>, send a node configure IQ
        // to set access_model=open + persist_items=true, then retry the publish.
        if (type && weechat_strcasecmp(type, "error") == 0 && account.omemo)
        {
            xmpp_stanza_t *err_elem = xmpp_stanza_get_child_by_name(stanza, "error");
            if (err_elem)
            {
                xmpp_stanza_t *precond = xmpp_stanza_get_child_by_name_and_ns(
                    err_elem, "precondition-not-met",
                    "http://jabber.org/protocol/pubsub#errors");
                if (precond)
                {
                    // Identify the node from the pubsub child of the failed IQ.
                    xmpp_stanza_t *failed_pubsub = xmpp_stanza_get_child_by_name_and_ns(
                        stanza, "pubsub", "http://jabber.org/protocol/pubsub");
                    std::string target_node;
                    if (failed_pubsub)
                    {
                        xmpp_stanza_t *pub = xmpp_stanza_get_child_by_name(failed_pubsub, "publish");
                        if (pub)
                        {
                            const char *n = xmpp_stanza_get_attribute(pub, "node");
                            if (n) target_node = n;
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
                        weechat_printf(account.buffer,
                            "%somemo: publish to '%s' rejected (precondition-not-met) — "
                            "configuring node and retrying",
                            weechat_prefix("network"), target_node.c_str());
    
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
            xmpp_stanza_t *dl_err_elem = xmpp_stanza_get_child_by_name(stanza, "error");
            xmpp_stanza_t *dl_pubsub = xmpp_stanza_get_child_by_name_and_ns(
                stanza, "pubsub", "http://jabber.org/protocol/pubsub");
            bool is_item_not_found = dl_err_elem && xmpp_stanza_get_child_by_name_and_ns(
                dl_err_elem, "item-not-found", "urn:ietf:params:xml:ns:xmpp-stanzas");
            bool is_legacy_devicelist_err = false;
            if (dl_pubsub)
            {
                xmpp_stanza_t *dl_items = xmpp_stanza_get_child_by_name(dl_pubsub, "items");
                if (dl_items)
                {
                    const char *dl_node = xmpp_stanza_get_attribute(dl_items, "node");
                    if (dl_node && std::string_view(dl_node) == "eu.siacs.conversations.axolotl.devicelist")
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
                            weechat_printf(dl_ch.buffer,
                                "%sOMEMO: %s has no legacy OMEMO device list either. "
                                "Keeping %zu queued message(s).",
                                weechat_prefix("error"),
                                dl_target_jid.c_str(),
                                dl_ch.pending_omemo_messages.size());
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
                    const std::string err_text = dl_err_elem
                        ? stanza_element_text(dl_err_elem) : std::string {};
                    weechat_printf(account.buffer,
                        "%somemo: transient devicelist fetch error for %s (%s) — "
                        "guard cleared, will retry on next send",
                        weechat_prefix("network"),
                        dl_target_jid.c_str(),
                        err_text.empty() ? "unknown error" : err_text.c_str());
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
    
                weechat_printf(account.buffer,
                    "%somemo: node '%s' configured — re-publishing",
                    weechat_prefix("network"), retry_node.c_str());
    
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
                        account.connection.send(bundle_stanza);
                        xmpp_stanza_release(bundle_stanza);
                    }
                }
            }
        }
    
        pubsub = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "pubsub", "http://jabber.org/protocol/pubsub");
        if (pubsub)
        {
            const char *items_node;
    
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
    
            items = xmpp_stanza_get_child_by_name(pubsub, "items");
            if (items)
            {
                items_node = xmpp_stanza_get_attribute(items, "node");
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
                                                               items);
    
                    if (is_own_devicelist)
                    {
                        bool found_self = false;
                        xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(items, "item");
                        xmpp_stanza_t *list = item
                            ? xmpp_stanza_get_child_by_name_and_ns(
                                item, "list", "eu.siacs.conversations.axolotl")
                            : nullptr;
    
                        for (xmpp_stanza_t *device = list ? xmpp_stanza_get_children(list) : nullptr;
                             device; device = xmpp_stanza_get_next(device))
                        {
                            const char *name = xmpp_stanza_get_name(device);
                            if (!name || weechat_strcasecmp(name, "device") != 0)
                                continue;
    
                            const char *did = xmpp_stanza_get_attribute(device, "id");
                            const auto parsed_did = parse_omemo_device_id(did);
                            if (parsed_did && *parsed_did == account.omemo.device_id)
                            {
                                found_self = true;
                                break;
                            }
                        }
    
                        if (!found_self)
                        {
                            weechat_printf(account.buffer,
                                "%somemo: our device %u missing from server legacy devicelist — re-publishing",
                                weechat_prefix("network"), account.omemo.device_id);
    
                            auto dl_stanza = account.get_devicelist();
                            if (dl_stanza)
                                account.connection.send(dl_stanza.get());
                        }
                    }
                    else if (account.omemo)
                    {
                        xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(items, "item");
                        xmpp_stanza_t *list = item
                            ? xmpp_stanza_get_child_by_name_and_ns(
                                item, "list", "eu.siacs.conversations.axolotl")
                            : nullptr;
    
                        int legacy_device_count = 0;
                        for (xmpp_stanza_t *device = list ? xmpp_stanza_get_children(list) : nullptr;
                             device;
                             device = xmpp_stanza_get_next(device))
                        {
                            const char *name = xmpp_stanza_get_name(device);
                            if (!name || weechat_strcasecmp(name, "device") != 0)
                                continue;
    
                            const char *did = xmpp_stanza_get_attribute(device, "id");
                            const auto parsed_did = parse_omemo_device_id(did);
                            if (!parsed_did)
                                continue;
    
                            if (*parsed_did == account.omemo.device_id)
                                continue;
    
                            const auto bundle_key =
                                std::make_pair(std::string(node_owner_str), *parsed_did);
                            if (account.omemo.pending_bundle_fetch.contains(bundle_key))
                                continue;
    
                            const auto bundle_node = fmt::format(
                                "eu.siacs.conversations.axolotl.bundles:{}",
                                *parsed_did);
                            std::string uuid = stanza::uuid(account.context);
                            stanza::xep0060::items its(bundle_node);
                            stanza::xep0060::pubsub ps;
                            ps.items(its);
                            stanza::iq iq_s;
                            iq_s.id(uuid).from(account.jid()).to(node_owner_str).type("get");
                            iq_s.pubsub(ps);
                            account.omemo.pending_iq_jid[uuid] = node_owner_str;
                            account.omemo.pending_bundle_fetch.insert(bundle_key);
    
                            ++legacy_device_count;
                            account.connection.send(iq_s.build(account.context).get());
                        }
    
                    XDEBUG("omemo: requested {} legacy bundle(s) for {}",
                           legacy_device_count,
                           node_owner_str);
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
                        item = xmpp_stanza_get_child_by_name(items, "item");
                        if (item)
                        {
                            const char *item_id = xmpp_stanza_get_id(item);
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
                                                               items);
                        else
                            weechat_printf(account.buffer,
                                           "%somemo: legacy bundle result for %s has missing/invalid device id",
                                           weechat_prefix("error"),
                                           bundle_jid.c_str());
                    }
                    else if (type && weechat_strcasecmp(type, "error") == 0)
                    {
                        weechat_printf(account.buffer,
                                       "%somemo: legacy bundle fetch for %s/%u returned error",
                                       weechat_prefix("error"),
                                       bundle_jid.c_str(), bundle_device_id);
                    }
                handled = true;
                }
            }
        }
    return handled;
}
