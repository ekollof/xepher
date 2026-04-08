void weechat::xmpp::omemo::show_fingerprint(struct t_gui_buffer *buffer, const char *jid)
{
    if (!db_env)
    {
        print_error(buffer, "OMEMO is not initialized.");
        return;
    }

    // Helper: format raw identity key bytes as colon-separated uppercase hex.
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

    // Map numeric trust value to a display label.
    auto trust_label = [&](std::string_view jid_sv, std::uint32_t dev_id) -> std::string {
        const auto t = load_tofu_trust(*this, std::string(jid_sv), dev_id);
        if (!t)
            return "UNDECIDED";
        switch (*t)
        {
            case omemo_trust::UNTRUSTED: return "UNTRUSTED";
            case omemo_trust::VERIFIED:  return "VERIFIED";
            case omemo_trust::UNDECIDED: return "UNDECIDED";
            case omemo_trust::BLIND:     return "BLIND";
            default: return "UNKNOWN";
        }
    };

    if (jid)
    {
        // Peer fingerprints: iterate all known axolotl devices for this JID.
        std::vector<std::uint32_t> devices;
        const auto devlist = load_string(*this, key_for_axolotl_devicelist(jid));
        if (devlist && !devlist->empty())
        {
            for (const auto &dev : split(*devlist, ';'))
            {
                const auto parsed = parse_uint32(dev);
                if (parsed && is_valid_omemo_device_id(*parsed))
                    devices.push_back(*parsed);
            }
        }
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
            const std::string trust = trust_label(jid, dev_id);
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

void weechat::xmpp::omemo::distrust_fp(struct t_gui_buffer *buffer,
                                        weechat::account &,
                                        const char *jid,
                                        std::optional<std::uint32_t> device_id)
{
    if (!db_env || !jid)
    {
        print_error(buffer, "OMEMO is not initialized.");
        return;
    }

    bool any_changed = false;
    const auto devlist = load_string(*this, key_for_axolotl_devicelist(jid));
    if (devlist && !devlist->empty())
    {
        for (const auto &dev : split(*devlist, ';'))
        {
            const auto dev_id = parse_uint32(dev);
            if (!dev_id || !is_valid_omemo_device_id(*dev_id))
                continue;
            if (device_id && *dev_id != *device_id)
                continue;
            store_tofu_trust(*this, jid, *dev_id, omemo_trust::UNTRUSTED);
            print_info(buffer, fmt::format("OMEMO: marked device {} for {} as UNTRUSTED.", *dev_id, jid));
            any_changed = true;
        }
    }

    if (!any_changed)
    {
        if (device_id)
            print_error(buffer, fmt::format(
                "OMEMO: no known device {} for {}.", *device_id, jid));
        else
            print_info(buffer, fmt::format("OMEMO: no known devices for {}.", jid));
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
    const auto devlist = load_string(*this, key_for_axolotl_devicelist(jid));
    if (devlist && !devlist->empty())
    {
        for (const auto &device : split(*devlist, ';'))
        {
            const auto parsed = parse_uint32(device);
            if (parsed && is_valid_omemo_device_id(*parsed))
                devices.push_back(*parsed);
        }
    }

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

std::vector<std::uint32_t> weechat::xmpp::omemo::get_cached_device_ids(std::string_view jid)
{
    std::vector<std::uint32_t> result;
    if (!db_env || jid.empty())
        return result;
    const auto devlist = load_string(*this, key_for_axolotl_devicelist(jid));
    if (!devlist || devlist->empty())
        return result;
    for (const auto &dev : split(*devlist, ';'))
    {
        const auto parsed = parse_uint32(dev);
        if (parsed && is_valid_omemo_device_id(*parsed))
            result.push_back(*parsed);
    }
    return result;
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
    print_info(buffer, fmt::format("  pubsub node: {}", kLegacyDevicesNode));
}
