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
