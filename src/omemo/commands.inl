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
        bool first = true;
        for (auto b : bytes)
        {
            if (!first)
                out += ':';
            first = false;
            constexpr const char *hex = "0123456789ABCDEF";
            out += hex[(b >> 4) & 0xF];
            out += hex[b & 0xF];
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
            std::ranges::copy(
                split(*devlist, ';')
                | std::views::transform(parse_uint32)
                | std::views::filter([](auto p) { return p && is_valid_omemo_device_id(*p); })
                | std::views::transform([](auto p) { return *p; }),
                std::back_inserter(devices));
        }
        std::ranges::sort(devices);
        devices.erase(std::ranges::unique(devices).begin(), devices.end());

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
    if (!db_env || !jid || jid[0] == '\0')
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
            store_tofu_trust(*this, std::string_view(jid), *dev_id, omemo_trust::UNTRUSTED);
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

void weechat::xmpp::omemo::trust_jid(struct t_gui_buffer *buffer,
                                      weechat::account &,
                                      const char *jid,
                                      std::optional<std::uint32_t> device_id)
{
    if (!db_env || !jid || jid[0] == '\0')
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
            store_tofu_trust(*this, std::string_view(jid), *dev_id, omemo_trust::VERIFIED);
            print_info(buffer, fmt::format("OMEMO: marked device {} for {} as VERIFIED.", *dev_id, jid));
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

    const std::string bare = normalize_bare_jid(nullptr, jid);
    const std::string peer = bare.empty() ? std::string(jid) : bare;

    remove_prefixed_keys(*this, fmt::format("session:{}:", peer));
    remove_prefixed_keys(*this, fmt::format("identity:{}:", peer));
    remove_prefixed_keys(*this, fmt::format("bundle:{}:", peer));
    remove_prefixed_keys(*this, fmt::format("axolotl_bundle:{}:", peer));
    remove_prefixed_keys(*this, fmt::format("legacy_bundle:{}:", peer));
    remove_prefixed_keys(*this, fmt::format("trust:{}:", peer));
    remove_prefixed_keys(*this, fmt::format("device_mode:{}:", peer));
    remove_prefixed_keys(*this, fmt::format("atm_trust:{}:", peer));
    remove_prefixed_keys(*this, fmt::format("axolotl_devicelist:{}", peer));
    remove_prefixed_keys(*this, fmt::format("legacy_devicelist:{}", peer));
    remove_prefixed_keys(*this, fmt::format("devicelist:{}", peer));
    axolotl_devicelist_cache_.erase(peer);
    peers_with_observed_traffic.erase(peer);
    print_info(buffer, fmt::format("Removed stored OMEMO data for {}.", peer));
}

void weechat::xmpp::omemo::prune_peer_cache(struct t_gui_buffer *buffer,
                                            std::string_view own_bare_jid)
{
    if (!db_env)
    {
        print_error(buffer, "OMEMO: no database open.");
        return;
    }

    const std::string own = own_bare_jid.empty()
        ? std::string{}
        : normalize_bare_jid(nullptr, own_bare_jid);
    if (own.empty())
    {
        print_error(buffer, "OMEMO: prune requires own bare JID.");
        return;
    }

    // Keep local key material and any keys scoped to our own bare JID.
    auto keep_key = [&](std::string_view key) -> bool {
        if (key == "device_id" || key == "registration_id" || key == "prekeys"
            || key == "identity:public" || key == "identity:private"
            || key == "signed_pre_key:id" || key == "signed_pre_key:record"
            || key == "signed_pre_key:public" || key == "signed_pre_key:signature")
            return true;
        if (key.starts_with("prekey:") || key.starts_with("signed-prekey:"))
            return true;

        // session:jid:dev  identity:jid:dev  trust:jid:dev  bundle:jid:dev
        // axolotl_bundle:jid:dev  device_mode:jid:dev  axolotl_devicelist:jid
        auto jid_from_key = [&](std::string_view prefix) -> std::optional<std::string_view> {
            if (!key.starts_with(prefix))
                return std::nullopt;
            auto rest = key.substr(prefix.size());
            if (rest.empty())
                return std::nullopt;
            // Optional trailing :device_id
            const auto colon = rest.find(':');
            return colon == std::string_view::npos ? rest : rest.substr(0, colon);
        };

        for (const auto pfx : {
                 "session:", "identity:", "trust:", "bundle:", "axolotl_bundle:",
                 "legacy_bundle:", "device_mode:", "atm_trust:", "senderkey:",
                 "axolotl_devicelist:", "legacy_devicelist:", "devicelist:"})
        {
            if (auto j = jid_from_key(pfx); j && *j == own)
                return true;
        }
        // identity:public / private already handled; identity:ownjid:N covered above
        return false;
    };

    std::size_t deleted = 0;
    try
    {
        auto transaction = lmdb::txn::begin(db_env);
        auto cursor = lmdb::cursor::open(transaction, dbi.omemo);
        std::string_view key;
        std::string_view value;

        for (bool found = cursor.get(key, value, MDB_FIRST); found;
             found = cursor.get(key, value, MDB_NEXT))
        {
            if (keep_key(key))
                continue;
            cursor.del();
            ++deleted;
        }
        transaction.commit();
    }
    catch (const lmdb::error &ex)
    {
        print_error(buffer, fmt::format("OMEMO: prune failed: {}", ex.what()));
        return;
    }

    // Drop peer-related RAM caches; keep local identity/device state.
    axolotl_devicelist_cache_.clear();
    tofu_trust_cache_.clear();
    peers_with_observed_traffic.clear();
    pending_bundle_fetch.clear();
    pending_key_transport.clear();
    key_transport_bootstrap_attempted.clear();
    failed_session_bootstrap.clear();
    postponed_key_transports.clear();
    deferred_live_key_transports.clear();
    heartbeat_sent.clear();
    prekey_reply_sent.clear();
    missing_axolotl_devicelist.clear();
    pending_iq_jid.clear();
    pending_configure_retry.clear();

    print_info(buffer, fmt::format(
        "OMEMO: pruned peer cache ({} keys removed). Own identity/prekeys kept. "
        "Sessions will re-form on next OMEMO traffic.",
        deleted));
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
        std::ranges::copy(
            split(*devlist, ';')
            | std::views::transform(parse_uint32)
            | std::views::filter([](auto p) { return p && is_valid_omemo_device_id(*p); })
            | std::views::transform([](auto p) { return *p; }),
            std::back_inserter(devices));
    }

    std::ranges::sort(devices);
    devices.erase(std::ranges::unique(devices).begin(), devices.end());

    if (devices.empty())
    {
        print_info(buffer, fmt::format("No OMEMO devices known for {}.", jid));
        return;
    }

    print_info(buffer, fmt::format("OMEMO devices for {} ({}):", jid, devices.size()));
    for (const auto device : devices)
    {
        const auto t = load_tofu_trust(*this, std::string(jid), device);
        std::string trust_str;
        if (!t)
            trust_str = "UNDECIDED";
        else
        {
            switch (*t)
            {
                case omemo_trust::UNTRUSTED: trust_str = "UNTRUSTED"; break;
                case omemo_trust::VERIFIED:  trust_str = "VERIFIED";  break;
                case omemo_trust::UNDECIDED: trust_str = "UNDECIDED"; break;
                case omemo_trust::BLIND:     trust_str = "BLIND";     break;
                default: trust_str = "UNKNOWN"; break;
            }
        }
        print_info(buffer, fmt::format("  {:10}  [{}]", device, trust_str));
    }
}

std::vector<std::uint32_t> weechat::xmpp::omemo::get_cached_device_ids(std::string_view jid)
{
    if (!db_env || jid.empty())
        return {};
    const auto devlist = load_string(*this, key_for_axolotl_devicelist(jid));
    if (!devlist || devlist->empty())
        return {};

    return split(*devlist, ';')
        | std::views::transform(parse_uint32)
        | std::views::filter([](auto p) { return p && is_valid_omemo_device_id(*p); })
        | std::views::transform([](auto p) { return *p; })
        | std::ranges::to<std::vector>();
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
