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
        || check_devlist(load_string(*this, key_for_axolotl_devicelist(sender_bare_jid)));
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
    if (!db_env)
    {
        print_error(buffer, "OMEMO is not initialized.");
        return;
    }

    // Helper: format raw identity key bytes as colon-separated uppercase hex,
    // then compute and return the ATM trust status label.
    auto format_fp_hex = [](const std::vector<std::uint8_t> &bytes) -> std::string {
        if (bytes.empty())
            return {};
        std::string out;
        out.reserve(bytes.size() * 3);
        for (std::size_t i = 0; i < bytes.size(); ++i)
        {
            if (i > 0)
                out += ':';
            constexpr const char *hex = "0123456789ABCDEF";
            out += hex[(bytes[i] >> 4) & 0xF];
            out += hex[bytes[i] & 0xF];
        }
        return out;
    };

    auto trust_label = [&](std::string_view jid_sv, const std::vector<std::uint8_t> &bytes) -> std::string {
        const std::string fp = atm_fingerprint_b64(bytes);
        if (fp.empty())
            return "unknown";
        const auto t = load_atm_trust(*this, jid_sv, fp);
        if (!t)
            return "undecided";
        return *t;
    };

    if (jid)
    {
        // Peer fingerprints: iterate all known devices for this JID.
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
        append_devices(load_string(*this, key_for_axolotl_devicelist(jid)));
        std::sort(devices.begin(), devices.end());
        devices.erase(std::unique(devices.begin(), devices.end()), devices.end());

        if (devices.empty())
        {
            print_info(buffer, fmt::format("OMEMO: no known devices for {}.", jid));
            return;
        }

        print_info(buffer, fmt::format("OMEMO fingerprints for {} ({} device(s)):", jid, devices.size()));
        for (const auto dev_id : devices)
        {
            const auto ik = load_bytes(*this,
                key_for_identity(jid, static_cast<std::int32_t>(dev_id)));
            if (!ik || ik->empty())
            {
                print_info(buffer, fmt::format("  device {:10} : (no key yet)", dev_id));
                continue;
            }
            const std::string hex = format_fp_hex(*ik);
            const std::string trust = trust_label(jid, *ik);
            print_info(buffer, fmt::format("  device {:10} [{}]: {}", dev_id, trust, hex));
        }
        return;
    }

    // Own fingerprint
    if (device_id == 0)
    {
        print_error(buffer, "OMEMO is not initialized.");
        return;
    }

    const auto pub = load_bytes(*this, kIdentityPublicKey);
    if (!pub || pub->empty())
    {
        print_error(buffer, "OMEMO: own identity key not yet generated.");
        return;
    }

    const std::string hex = format_fp_hex(*pub);
    print_info(buffer, fmt::format("OMEMO own fingerprint (device {}):", device_id));
    print_info(buffer, fmt::format("  {}", hex));
}

void weechat::xmpp::omemo::send_atm_distrust_pub(weechat::account &account, const char *jid)
{
    if (!db_env || !jid)
        return;

    // Collect (jid, fingerprint) pairs for all known devices of this JID,
    // then send one batched <distrust> trust-message (XEP-0434 §4 batching).
    std::vector<std::pair<std::string,std::string>> pairs;
    auto collect_devlist = [&](const std::optional<std::string> &devlist) {
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
            pairs.emplace_back(jid, fp);
        }
    };

    collect_devlist(load_string(*this, key_for_devicelist(jid)));
    collect_devlist(load_string(*this, key_for_axolotl_devicelist(jid)));

    if (!pairs.empty())
        send_atm_distrust_message(*this, account, pairs);
}

// Normalise a fingerprint string for comparison: strip colons/spaces,
// convert to uppercase.  Returns empty string on invalid input.
static std::string normalise_fp_hex(std::string_view raw)
{
    std::string out;
    out.reserve(raw.size());
    for (char c : raw)
    {
        if (c == ':' || c == ' ')
            continue;
        if (c >= 'a' && c <= 'f')
            c = static_cast<char>(c - 'a' + 'A');
        else if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F')))
            return {};  // invalid character
        out += c;
    }
    return out;
}

// Format raw bytes as uppercase hex (no separators) for comparison with
// a normalised fingerprint from the user.
static std::string bytes_to_hex(const std::vector<std::uint8_t> &bytes)
{
    constexpr const char *hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (auto b : bytes)
    {
        out += hex[(b >> 4) & 0xF];
        out += hex[b & 0xF];
    }
    return out;
}

void weechat::xmpp::omemo::approve_jid(struct t_gui_buffer *buffer,
                                        weechat::account &account,
                                        const char *jid,
                                        const char *fp_hex)
{
    if (!db_env || !jid)
    {
        print_error(buffer, "OMEMO is not initialized.");
        return;
    }

    const std::string filter = fp_hex ? normalise_fp_hex(fp_hex) : std::string{};
    if (fp_hex && filter.empty())
    {
        print_error(buffer, "OMEMO: invalid fingerprint format.");
        return;
    }

    std::vector<std::pair<std::string,std::string>> pairs;
    auto collect = [&](const std::optional<std::string> &devlist) {
        if (!devlist || devlist->empty())
            return;
        for (const auto &dev : split(*devlist, ';'))
        {
            const auto dev_id = parse_uint32(dev);
            if (!dev_id || !is_valid_omemo_device_id(*dev_id))
                continue;
            const auto ik = load_bytes(*this,
                key_for_identity(jid, static_cast<std::int32_t>(*dev_id)));
            if (!ik || ik->empty())
                continue;
            if (!filter.empty() && bytes_to_hex(*ik) != filter)
                continue;
            const std::string fp = atm_fingerprint_b64(*ik);
            if (fp.empty())
                continue;
            // Only change undecided (or absent) keys; do not silently override
            // an explicit "distrusted" decision.
            const auto existing = load_atm_trust(*this, jid, fp);
            if (existing && *existing == "distrusted")
            {
                print_info(buffer, fmt::format(
                    "OMEMO: device {} is currently distrusted; use /omemo trust <jid> to reset.",
                    *dev_id));
                continue;
            }
            store_atm_trust(*this, jid, fp, "trusted");
            pairs.emplace_back(jid, fp);
            print_info(buffer, fmt::format("OMEMO: marked device {} for {} as trusted.", *dev_id, jid));
        }
    };

    collect(load_string(*this, key_for_devicelist(jid)));
    collect(load_string(*this, key_for_axolotl_devicelist(jid)));

    if (pairs.empty())
    {
        if (!filter.empty())
            print_error(buffer, fmt::format(
                "OMEMO: no known key for {} matches that fingerprint.", jid));
        else
            print_info(buffer, fmt::format(
                "OMEMO: no undecided keys found for {}.", jid));
        return;
    }

    if (weechat::config::instance
        && weechat_config_boolean(weechat::config::instance->look.omemo_atm))
    {
        send_atm_trust_message(*this, account, pairs);
    }
}

void weechat::xmpp::omemo::distrust_fp(struct t_gui_buffer *buffer,
                                        weechat::account &account,
                                        const char *jid,
                                        const char *fp_hex)
{
    if (!db_env || !jid)
    {
        print_error(buffer, "OMEMO is not initialized.");
        return;
    }

    const std::string filter = fp_hex ? normalise_fp_hex(fp_hex) : std::string{};
    if (fp_hex && filter.empty())
    {
        print_error(buffer, "OMEMO: invalid fingerprint format.");
        return;
    }

    std::vector<std::pair<std::string,std::string>> pairs;
    auto collect = [&](const std::optional<std::string> &devlist) {
        if (!devlist || devlist->empty())
            return;
        for (const auto &dev : split(*devlist, ';'))
        {
            const auto dev_id = parse_uint32(dev);
            if (!dev_id || !is_valid_omemo_device_id(*dev_id))
                continue;
            const auto ik = load_bytes(*this,
                key_for_identity(jid, static_cast<std::int32_t>(*dev_id)));
            if (!ik || ik->empty())
                continue;
            if (!filter.empty() && bytes_to_hex(*ik) != filter)
                continue;
            const std::string fp = atm_fingerprint_b64(*ik);
            if (fp.empty())
                continue;
            store_atm_trust(*this, jid, fp, "distrusted");
            pairs.emplace_back(jid, fp);
            print_info(buffer, fmt::format("OMEMO: marked device {} for {} as distrusted.", *dev_id, jid));
        }
    };

    collect(load_string(*this, key_for_devicelist(jid)));
    collect(load_string(*this, key_for_axolotl_devicelist(jid)));

    if (pairs.empty())
    {
        if (!filter.empty())
            print_error(buffer, fmt::format(
                "OMEMO: no known key for {} matches that fingerprint.", jid));
        else
            print_info(buffer, fmt::format("OMEMO: no known keys for {}.", jid));
        return;
    }

    if (weechat::config::instance
        && weechat_config_boolean(weechat::config::instance->look.omemo_atm))
    {
        send_atm_distrust_message(*this, account, pairs);
    }
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
    append_devices(load_string(*this, key_for_axolotl_devicelist(jid)));

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

void weechat::xmpp::omemo::send_opt_out(weechat::account &account,
                                        struct t_gui_buffer *buffer,
                                        const char *jid,
                                        const char *reason)
{
    if (!db_env || !jid)
    {
        print_error(buffer, "OMEMO is not initialized.");
        return;
    }

    const std::string bare_jid = normalize_bare_jid(*account.context, jid);
    if (bare_jid.empty())
    {
        print_error(buffer, "OMEMO: invalid JID for opt-out.");
        return;
    }

    const std::string_view reason_sv = reason ? reason : std::string_view {};
    send_omemo2_opt_out(*this, account, bare_jid, reason_sv);

    // Record that we ourselves have opted out toward this peer so encode()
    // will block outgoing OMEMO to them until optout_ack() is called.
    omemo_opted_out_peers.insert(bare_jid);

    if (reason && *reason)
    {
        print_info(buffer, fmt::format(
            "OMEMO: sent opt-out to {} (reason: {}). "
            "Outgoing OMEMO messages to this contact are now blocked. "
            "Use /omemo optout-ack {} to re-enable OMEMO or keep it disabled.",
            bare_jid, reason, bare_jid));
    }
    else
    {
        print_info(buffer, fmt::format(
            "OMEMO: sent opt-out to {}. "
            "Outgoing OMEMO messages to this contact are now blocked. "
            "Use /omemo optout-ack {} to re-enable OMEMO or keep it disabled.",
            bare_jid, bare_jid));
    }
}

void weechat::xmpp::omemo::optout_ack(struct t_gui_buffer *buffer, const char *jid)
{
    if (!jid)
    {
        print_error(buffer, "OMEMO: optout-ack requires a JID.");
        return;
    }

    const std::string bare_jid = jid; // Already expected to be bare by caller
    const bool was_blocked = omemo_opted_out_peers.erase(bare_jid) > 0;

    if (was_blocked)
    {
        print_info(buffer, fmt::format(
            "OMEMO: opt-out for {} acknowledged. "
            "Outgoing OMEMO messages to this contact are now unblocked.",
            bare_jid));
    }
    else
    {
        print_info(buffer, fmt::format(
            "OMEMO: {} was not in the opt-out list (nothing changed).",
            bare_jid));
    }
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
}
