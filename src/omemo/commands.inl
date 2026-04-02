void weechat::xmpp::omemo::store_atm_trust_pub(const char *jid, const char *key_b64,
                                               const std::string &level)
{
    if (!db_env || !jid || !key_b64)
        return;
    store_atm_trust(*this, jid, key_b64, level);
}

bool weechat::xmpp::omemo::sender_atm_trusted_pub(const char *sender_bare_jid)
{
    if (!db_env || !sender_bare_jid)
        return false;

    // Walk all known devices for this JID and check whether at least one
    // identity key has ATM trust level "trusted".
    auto check_devlist = [&](const std::optional<std::string> &devlist) -> bool {
        if (!devlist || devlist->empty())
            return false;
        for (const auto &dev : split(*devlist, ';'))
        {
            const auto dev_id = parse_uint32(dev);
            if (!dev_id || !is_valid_omemo_device_id(*dev_id))
                continue;
            const auto ik_bytes = load_bytes(*this,
                key_for_identity(sender_bare_jid,
                                 static_cast<std::int32_t>(*dev_id)));
            if (!ik_bytes || ik_bytes->empty())
                continue;
            const std::string fp = atm_fingerprint_b64(*ik_bytes);
            if (fp.empty())
                continue;
            const auto trust = load_atm_trust(*this, sender_bare_jid, fp);
            if (trust && *trust == "trusted")
                return true;
        }
        return false;
    };

    return check_devlist(load_string(*this, key_for_devicelist(sender_bare_jid)))
        || check_devlist(load_string(*this, key_for_legacy_devicelist(sender_bare_jid)));
}

// XEP-0450 §4: parse a <trust-message> from inside a decrypted SCE envelope,
// gate on sender auth, and apply trust decisions.
void weechat::xmpp::omemo::process_atm_trust_sce_pub(xmpp_ctx_t *ctx,
                                                     const char *sender_bare_jid,
                                                     const char *sce_xml)
{
    if (!db_env || !ctx || !sender_bare_jid || !sce_xml || !*sce_xml)
        return;

    const bool sender_trusted = sender_atm_trusted_pub(sender_bare_jid);

    // Parse the SCE envelope and locate <trust-message> inside <content>.
    auto stanza = std::unique_ptr<xmpp_stanza_t, decltype(&xmpp_stanza_release)>(
        xmpp_stanza_new_from_string(ctx, sce_xml),
        xmpp_stanza_release);
    if (!stanza)
        return;

    xmpp_stanza_t *envelope = stanza.get();
    // If the root is the envelope itself, use it; otherwise look for it.
    const char *root_ns = xmpp_stanza_get_ns(envelope);
    if (!root_ns || std::string_view(root_ns) != "urn:xmpp:sce:1")
    {
        envelope = xmpp_stanza_get_child_by_name_and_ns(envelope, "envelope", "urn:xmpp:sce:1");
        if (!envelope)
            return;
    }

    xmpp_stanza_t *content = xmpp_stanza_get_child_by_name(envelope, "content");
    if (!content)
        return;

    xmpp_stanza_t *trust_msg = xmpp_stanza_get_child_by_name_and_ns(
        content, "trust-message", "urn:xmpp:tm:1");
    if (!trust_msg)
        return;

    const char *usage = xmpp_stanza_get_attribute(trust_msg, "usage");
    if (!usage || std::string_view(usage) != "urn:xmpp:atm:1")
        return;

    // Accept both OMEMO:2 and legacy namespaces.
    const char *enc_ns = xmpp_stanza_get_attribute(trust_msg, "encryption");
    if (!enc_ns)
        return;
    const std::string_view enc_sv(enc_ns);
    if (enc_sv != "urn:xmpp:omemo:2" && enc_sv != "eu.siacs.conversations.axolotl")
        return;

    // Walk <key-owner> children and apply or defer trust decisions.
    // XEP-0450 §5 MUST: only apply if sender is ATM-trusted.
    // XEP-0450 §5.1: if sender not yet trusted, defer for later application.
    xmpp_stanza_t *ko = xmpp_stanza_get_children(trust_msg);
    while (ko)
    {
        const char *ko_name = xmpp_stanza_get_name(ko);
        const char *ko_jid = (ko_name && std::string_view(ko_name) == "key-owner")
            ? xmpp_stanza_get_attribute(ko, "jid") : nullptr;
        if (ko_jid)
        {
            xmpp_stanza_t *decision = xmpp_stanza_get_children(ko);
            while (decision)
            {
                const char *dname = xmpp_stanza_get_name(decision);
                if (dname && (std::string_view(dname) == "trust"
                               || std::string_view(dname) == "distrust"))
                {
                    const char *fp = xmpp_stanza_get_text_ptr(decision);
                    if (fp && *fp)
                    {
                        const std::string level =
                            (std::string_view(dname) == "trust") ? "trusted" : "distrusted";
                        if (sender_trusted)
                        {
                            store_atm_trust(*this, ko_jid, fp, level);
                        }
                        else
                        {
                            // §5.1: defer until sender becomes trusted.
                            pending_atm_trust_from_unauthenticated[sender_bare_jid]
                                .emplace_back(ko_jid, fp, level);
                        }
                    }
                }
                decision = xmpp_stanza_get_next(decision);
            }
        }
        ko = xmpp_stanza_get_next(ko);
    }
}

void weechat::xmpp::omemo::show_fingerprint(struct t_gui_buffer *buffer, const char *jid)
{
    if (jid)
    {
        std::vector<std::uint32_t> devices;
        auto append_devices = [&](const std::optional<std::string> &device_list)
        {
            if (!device_list || device_list->empty())
                return;
            for (const auto &device : split(*device_list, ';'))
            {
                const auto parsed = parse_uint32(device);
                if (parsed && is_valid_omemo_device_id(*parsed))
                    devices.push_back(*parsed);
            }
        };

        if (db_env)
        {
            append_devices(load_string(*this, key_for_devicelist(jid)));
            append_devices(load_string(*this, key_for_legacy_devicelist(jid)));
        }

        std::sort(devices.begin(), devices.end());
        devices.erase(std::unique(devices.begin(), devices.end()), devices.end());
        print_info(buffer, fmt::format(
            "OMEMO: {} known device(s) for {}; peer fingerprints are not implemented yet.",
            devices.size(), jid));
        return;
    }

    if (device_id == 0)
    {
        print_error(buffer, "OMEMO is not initialized.");
        return;
    }

    print_info(buffer, fmt::format(
        "OMEMO active; fingerprint generation is not implemented yet (device {}).",
        device_id));
}

void weechat::xmpp::omemo::send_atm_distrust_pub(weechat::account &account, const char *jid)
{
    if (!db_env || !jid)
        return;

    // Walk all known devices for this JID, compute their fingerprints, mark
    // each as "distrusted" in LMDB and broadcast a <distrust> trust-message.
    auto process_devlist = [&](const std::optional<std::string> &devlist) {
        if (!devlist || devlist->empty())
            return;
        for (const auto &dev : split(*devlist, ';'))
        {
            const auto dev_id = parse_uint32(dev);
            if (!dev_id || !is_valid_omemo_device_id(*dev_id))
                continue;
            const auto ik_bytes = load_bytes(*this,
                key_for_identity(jid, static_cast<std::int32_t>(*dev_id)));
            if (!ik_bytes || ik_bytes->empty())
                continue;
            const std::string fp = atm_fingerprint_b64(*ik_bytes);
            if (fp.empty())
                continue;
            store_atm_trust(*this, jid, fp, "distrusted");
            send_atm_distrust_message(*this, account, jid, fp);
        }
    };

    process_devlist(load_string(*this, key_for_devicelist(jid)));
    process_devlist(load_string(*this, key_for_legacy_devicelist(jid)));
}

void weechat::xmpp::omemo::distrust_jid(struct t_gui_buffer *buffer, const char *jid)
{
    if (!db_env || !jid)
        return;

    remove_prefixed_keys(*this, fmt::format("identity_key:{}", jid));
    remove_prefixed_keys(*this, fmt::format("bundle:{}:", jid));
    remove_prefixed_keys(*this, fmt::format("session:{}:", jid));
    print_info(buffer, fmt::format("Removed stored OMEMO data for {}.", jid));
}

void weechat::xmpp::omemo::show_devices(struct t_gui_buffer *buffer, const char *jid)
{
    if (!db_env || !jid)
    {
        print_error(buffer, "OMEMO device list is unavailable.");
        return;
    }

    std::vector<std::uint32_t> devices;
    auto append_devices = [&](const std::optional<std::string> &device_list)
    {
        if (!device_list || device_list->empty())
            return;
        for (const auto &device : split(*device_list, ';'))
        {
            const auto parsed = parse_uint32(device);
            if (parsed && is_valid_omemo_device_id(*parsed))
                devices.push_back(*parsed);
        }
    };

    append_devices(load_string(*this, key_for_devicelist(jid)));
    append_devices(load_string(*this, key_for_legacy_devicelist(jid)));

    std::sort(devices.begin(), devices.end());
    devices.erase(std::unique(devices.begin(), devices.end()), devices.end());

    if (devices.empty())
    {
        print_info(buffer, fmt::format("No OMEMO devices known for {}.", jid));
        return;
    }

    print_info(buffer, fmt::format("OMEMO devices for {}:", jid));
    for (const auto device : devices)
        print_info(buffer, fmt::format("  {}", device));
}

void weechat::xmpp::omemo::show_status(struct t_gui_buffer *buffer,
                                       const char *account_name,
                                       const char *channel_name,
                                       int channel_omemo_enabled)
{
    print_info(buffer, fmt::format("OMEMO status for account {}:", account_name ? account_name : "?"));
    print_info(buffer, fmt::format("  initialized: {}", *this ? "yes" : "no"));
    print_info(buffer, fmt::format("  device id: {}", device_id));
    print_info(buffer, fmt::format("  database: {}", db_path.empty() ? "(none)" : db_path));
    print_info(buffer, fmt::format("  channel: {} ({})",
        channel_name ? channel_name : "(none)",
        channel_omemo_enabled ? "enabled" : "disabled"));
    print_info(buffer, fmt::format("  pubsub nodes: {}, {}", kDevicesNode, kBundlesNode));
    print_info(buffer, "  note: fingerprint/status reporting is still incomplete.");
}
