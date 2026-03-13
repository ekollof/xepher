namespace {

[[nodiscard]] static bool devicelist_contains_device(omemo &self,
                                                     std::string_view key,
                                                     std::uint32_t device_id)
{
    const auto list = load_string(self, key);
    if (!list || list->empty())
        return false;

    for (const auto &dev : split(*list, ';'))
    {
        const auto parsed_device_id = parse_uint32(dev);
        if (parsed_device_id && *parsed_device_id == device_id)
            return true;
    }

    return false;
}

[[nodiscard]] static auto resolve_device_mode(omemo &self,
                                              weechat::account &account,
                                              std::string_view jid,
                                              std::uint32_t device_id,
                                              omemo::peer_mode preferred = omemo::peer_mode::unknown)
    -> omemo::peer_mode
{
    if (jid.empty() || device_id == 0)
        return preferred;

    const std::string bare_jid = normalize_bare_jid(account.context, jid);
    const auto stored_mode = load_device_mode(self, bare_jid, device_id);
    const bool in_omemo2_list = devicelist_contains_device(self, key_for_devicelist(bare_jid), device_id);
    const bool in_legacy_list = devicelist_contains_device(self, key_for_legacy_devicelist(bare_jid), device_id);

    if (in_omemo2_list && !in_legacy_list)
        return omemo::peer_mode::omemo2;
    if (in_legacy_list && !in_omemo2_list)
        return omemo::peer_mode::legacy;

    if (preferred != omemo::peer_mode::unknown)
        return preferred;

    if (stored_mode)
        return *stored_mode;

    if (account.peer_has_legacy_axolotl_only(bare_jid))
        return omemo::peer_mode::legacy;

    if (in_omemo2_list)
        return omemo::peer_mode::omemo2;
    if (in_legacy_list)
        return omemo::peer_mode::legacy;

    return omemo::peer_mode::unknown;
}

}

static void send_key_transport(omemo &self,
                               weechat::account &account,
                               struct t_gui_buffer *buffer,
                               const char *peer_jid,
                               std::uint32_t remote_device_id);

void weechat::xmpp::omemo::request_devicelist(weechat::account &account, std::string_view jid)
{
    const std::string bare_jid = normalize_bare_jid(account.context, jid);
    // Probe both namespaces up front. Device support is resolved per-device
    // during bundle bootstrap rather than assumed peer-wide.
    ::request_devicelist(account, bare_jid);
    ::request_legacy_devicelist(account, bare_jid);
}

void weechat::xmpp::omemo::request_legacy_devicelist(weechat::account &account, std::string_view jid)
{
    ::request_legacy_devicelist(account, jid);
}

void weechat::xmpp::omemo::force_fetch(weechat::account &account,
                                       struct t_gui_buffer *buffer,
                                       std::string_view jid,
                                       std::optional<std::uint32_t> device_id)
{
    const std::string bare_jid = normalize_bare_jid(account.context, jid);
    if (bare_jid.empty())
    {
        print_error(buffer ? buffer : account.buffer,
                    "OMEMO: force-fetch requires a valid JID.");
        return;
    }

    request_devicelist(account, bare_jid);

    if (device_id)
    {
        if (!is_valid_omemo_device_id(*device_id))
        {
            print_error(buffer ? buffer : account.buffer,
                        fmt::format("OMEMO: invalid device id {}.", *device_id));
            return;
        }

        request_bundle(account, bare_jid, *device_id);
        request_legacy_bundle(account, bare_jid, *device_id);
        print_info(buffer ? buffer : account.buffer,
                   fmt::format("OMEMO: forced devicelist + bundle refresh for {}/{}.",
                               bare_jid, *device_id));
        return;
    }

    std::set<std::uint32_t> known_devices;
    auto collect_devices = [&](const std::optional<std::string> &list)
    {
        if (!list || list->empty())
            return;
        for (const auto &dev : split(*list, ';'))
        {
            const auto parsed = parse_uint32(dev);
            if (parsed && is_valid_omemo_device_id(*parsed))
                known_devices.insert(*parsed);
        }
    };

    collect_devices(load_string(*this, key_for_devicelist(bare_jid)));
    collect_devices(load_string(*this, key_for_legacy_devicelist(bare_jid)));

    for (const auto known_device_id : known_devices)
    {
        request_bundle(account, bare_jid, known_device_id);
        request_legacy_bundle(account, bare_jid, known_device_id);
    }

    if (known_devices.empty())
    {
        print_info(buffer ? buffer : account.buffer,
                   fmt::format("OMEMO: forced devicelist refresh for {} (no cached device ids yet).",
                               bare_jid));
        return;
    }

    print_info(buffer ? buffer : account.buffer,
               fmt::format("OMEMO: forced devicelist + bundle refresh for {} ({} device(s)).",
                           bare_jid, known_devices.size()));
}

void weechat::xmpp::omemo::force_kex(weechat::account &account,
                                     struct t_gui_buffer *buffer,
                                     std::string_view jid,
                                     std::optional<std::uint32_t> device_id)
{
    const std::string bare_jid = normalize_bare_jid(account.context, jid);
    if (bare_jid.empty())
    {
        print_error(buffer ? buffer : account.buffer,
                    "OMEMO: force-kex requires a valid JID.");
        return;
    }

    std::set<std::uint32_t> target_devices;
    if (device_id)
    {
        if (!is_valid_omemo_device_id(*device_id))
        {
            print_error(buffer ? buffer : account.buffer,
                        fmt::format("OMEMO: invalid device id {}.", *device_id));
            return;
        }
        target_devices.insert(*device_id);
    }
    else
    {
        auto collect_devices = [&](const std::optional<std::string> &list)
        {
            if (!list || list->empty())
                return;
            for (const auto &dev : split(*list, ';'))
            {
                const auto parsed = parse_uint32(dev);
                if (parsed && is_valid_omemo_device_id(*parsed))
                    target_devices.insert(*parsed);
            }
        };

        collect_devices(load_string(*this, key_for_devicelist(bare_jid)));
        collect_devices(load_string(*this, key_for_legacy_devicelist(bare_jid)));
    }

    if (target_devices.empty())
    {
        request_devicelist(account, bare_jid);
        print_info(buffer ? buffer : account.buffer,
                   fmt::format("OMEMO: no known devices for {}; requested devicelist refresh.",
                               bare_jid));
        return;
    }

    std::size_t sent_now = 0;
    std::size_t queued = 0;
    for (const auto remote_device_id : target_devices)
    {
        failed_session_bootstrap.erase({bare_jid, remote_device_id});

        bool have_session = has_session(bare_jid.c_str(), remote_device_id);
        if (!have_session)
        {
            try
            {
                have_session = establish_session_from_bundle(
                    *this, *account.context, bare_jid, remote_device_id);
            }
            catch (const std::exception &ex)
            {
                print_error(buffer ? buffer : account.buffer,
                            fmt::format("OMEMO: session bootstrap error for {}/{}: {}",
                                        bare_jid, remote_device_id, ex.what()));
                have_session = false;
            }
        }

        if (have_session)
        {
            send_key_transport(*this, account, buffer ? buffer : account.buffer,
                               bare_jid.c_str(), remote_device_id);
            ++sent_now;
            continue;
        }

        pending_key_transport.insert({bare_jid, remote_device_id});
        request_bundle(account, bare_jid, remote_device_id);
        request_legacy_bundle(account, bare_jid, remote_device_id);
        ++queued;
    }

    print_info(buffer ? buffer : account.buffer,
               fmt::format("OMEMO: force-kex for {}: sent now={}, queued after bundle fetch={}",
                           bare_jid, sent_now, queued));
}

void weechat::xmpp::omemo::note_peer_traffic(xmpp_ctx_t *context, std::string_view jid)
{
    if (jid.empty())
        return;

    peers_with_observed_traffic.insert(normalize_bare_jid(context, jid));
}

auto weechat::xmpp::omemo::has_peer_traffic(xmpp_ctx_t *context,
                                            std::string_view jid) const -> bool
{
    if (jid.empty())
        return false;

    const std::string bare_jid = normalize_bare_jid(context, jid);
    return peers_with_observed_traffic.count(bare_jid) != 0;
}

auto weechat::xmpp::omemo::select_peer_mode(weechat::account &account,
                                            std::string_view jid) -> peer_mode
{
    if (!db_env || jid.empty())
        return peer_mode::unknown;

    const std::string bare_jid = normalize_bare_jid(account.context, jid);
    const auto omemo2_list = load_string(*this, key_for_devicelist(bare_jid));
    const auto legacy_list = load_string(*this, key_for_legacy_devicelist(bare_jid));

    std::unordered_set<std::uint32_t> all_devices;
    auto collect_devices = [&](const std::optional<std::string> &list)
    {
        if (!list || list->empty())
            return;

        for (const auto &dev : split(*list, ';'))
        {
            const auto device_id = parse_uint32(dev);
            if (device_id && is_valid_omemo_device_id(*device_id))
                all_devices.insert(*device_id);
        }
    };

    collect_devices(omemo2_list);
    collect_devices(legacy_list);

    std::size_t known_omemo2_devices = 0;
    std::size_t known_legacy_devices = 0;
    for (const auto device_id : all_devices)
    {
        const auto resolved_mode = resolve_device_mode(*this, account, bare_jid, device_id);
        if (resolved_mode == peer_mode::omemo2)
            ++known_omemo2_devices;
        else if (resolved_mode == peer_mode::legacy)
            ++known_legacy_devices;
    }

    if (known_omemo2_devices != 0 && known_legacy_devices == 0)
        return peer_mode::omemo2;
    if (known_legacy_devices != 0 && known_omemo2_devices == 0)
        return peer_mode::legacy;

    const bool has_omemo2 = omemo2_list && !omemo2_list->empty();
    const bool has_legacy = legacy_list && !legacy_list->empty();

    if (has_omemo2 && !has_legacy)
        return peer_mode::omemo2;
    if (has_legacy && !has_omemo2)
        return peer_mode::legacy;
    if (has_omemo2 && has_legacy)
    {
        if (account.peer_has_legacy_axolotl_only(bare_jid))
            return peer_mode::legacy;
        return peer_mode::omemo2;
    }

    if (account.peer_has_legacy_axolotl_only(bare_jid))
        return peer_mode::legacy;

    return peer_mode::unknown;
}

void weechat::xmpp::omemo::clear_cached_bundle(std::string_view jid,
                                               std::uint32_t remote_device_id)
{
    if (!db_env || jid.empty() || remote_device_id == 0)
        return;

    auto transaction = lmdb::txn::begin(db_env);
    dbi.omemo.del(transaction, fmt::format("bundle:{}:{}", jid, remote_device_id));
    transaction.commit();
}

void weechat::xmpp::omemo::handle_devicelist(weechat::account *account,
                                             const char *jid,
                                             xmpp_stanza_t *items)
{
    if (!db_env || !jid)
    {
        weechat_printf(nullptr, "%somemo: handle_devicelist: invalid args (jid=%s)",
                       weechat_prefix("error"), jid ? jid : "(null)");
        return;
    }

    std::string bare_jid = jid;
    if (account && account->context)
    {
        xmpp_string_guard bare_g(*account->context,
                                 xmpp_jid_bare(*account->context, jid));
        if (bare_g.ptr && *bare_g.ptr)
            bare_jid = bare_g.ptr;
    }

    const auto devices = extract_devices_from_items(items);
    const auto devicelist_str = join(devices, ";");

    missing_omemo2_devicelist.erase(bare_jid);
    
    weechat_printf(nullptr, "%somemo: handle_devicelist for %s: storing %zu device(s) [%s]",
                   weechat_prefix("network"), bare_jid.c_str(), devices.size(),
                   devicelist_str.empty() ? "(empty)" : devicelist_str.c_str());
    
    store_string(*this, key_for_devicelist(bare_jid), devicelist_str);
    for (const auto &dev : devices)
    {
        const auto remote_device_id = parse_uint32(dev);
        if (!remote_device_id || !is_valid_omemo_device_id(*remote_device_id))
            continue;
        store_device_mode(*this, bare_jid, *remote_device_id, peer_mode::omemo2);
    }

    // Conversations resends waiting messages once OMEMO metadata/session setup
    // completes. If we already have at least one usable session for this JID
    // when a devicelist update arrives, opportunistically flush PM queue now.
    if (!account)
        return;

    auto ch_it = account->channels.find(bare_jid);
    if (ch_it == account->channels.end())
        return;

    auto &ch = ch_it->second;
    if (ch.type != weechat::channel::chat_type::PM)
        return;

    if (ch.pending_omemo_messages.empty())
        return;

    bool has_any_session = false;
    for (const auto &dev : devices)
    {
        const auto remote_device_id = parse_uint32(dev);
        if (!remote_device_id || !is_valid_omemo_device_id(*remote_device_id))
            continue;

        if (has_session(bare_jid.c_str(), *remote_device_id))
        {
            has_any_session = true;
            break;
        }
    }

    if (has_any_session)
        ch.flush_pending_omemo_messages();
}

void weechat::xmpp::omemo::handle_legacy_devicelist(weechat::account *account,
                                                    const char *jid,
                                                    xmpp_stanza_t *items)
{
    if (!db_env || !jid)
    {
        weechat_printf(nullptr,
                       "%somemo: handle_legacy_devicelist: invalid args (jid=%s)",
                       weechat_prefix("error"),
                       jid ? jid : "(null)");
        return;
    }

    std::string bare_jid = jid;
    if (account && account->context)
    {
        xmpp_string_guard bare_g(*account->context,
                                 xmpp_jid_bare(*account->context, jid));
        if (bare_g.ptr && *bare_g.ptr)
            bare_jid = bare_g.ptr;
    }

    std::vector<std::string> devices;
    if (items)
    {
        xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(items, "item");
        xmpp_stanza_t *list = item
            ? xmpp_stanza_get_child_by_name_and_ns(item, "list", kLegacyOmemoNs.data())
            : nullptr;

        for (xmpp_stanza_t *device = list ? xmpp_stanza_get_children(list) : nullptr;
             device;
             device = xmpp_stanza_get_next(device))
        {
            const char *name = xmpp_stanza_get_name(device);
            if (!name || weechat_strcasecmp(name, "device") != 0)
                continue;

            const char *device_id = xmpp_stanza_get_attribute(device, "id");
            const auto parsed_device_id = parse_uint32(device_id ? device_id : "");
            if (!parsed_device_id || !is_valid_omemo_device_id(*parsed_device_id))
                continue;

            devices.emplace_back(fmt::format("{}", *parsed_device_id));
        }
    }

    const auto devicelist_str = join(devices, ";");
    missing_legacy_devicelist.erase(bare_jid);
    store_string(*this, key_for_legacy_devicelist(bare_jid), devicelist_str);
    for (const auto &dev : devices)
    {
        const auto remote_device_id = parse_uint32(dev);
        if (!remote_device_id || !is_valid_omemo_device_id(*remote_device_id))
            continue;
        store_device_mode(*this, bare_jid, *remote_device_id, peer_mode::legacy);
    }

    weechat_printf(nullptr,
                   "%somemo: handle_legacy_devicelist for %s: storing %zu device(s) [%s]",
                   weechat_prefix("network"),
                   bare_jid.c_str(),
                   devices.size(),
                   devicelist_str.empty() ? "(empty)" : devicelist_str.c_str());

    if (!account)
        return;

    auto ch_it = account->channels.find(bare_jid);
    if (ch_it == account->channels.end())
        return;

    auto &ch = ch_it->second;
    if (ch.type != weechat::channel::chat_type::PM || ch.pending_omemo_messages.empty())
        return;

    bool has_any_session = false;
    for (const auto &dev : devices)
    {
        const auto remote_device_id = parse_uint32(dev);
        if (!remote_device_id || !is_valid_omemo_device_id(*remote_device_id))
            continue;

        if (has_session(bare_jid.c_str(), *remote_device_id))
        {
            has_any_session = true;
            break;
        }
    }

    if (has_any_session)
        ch.flush_pending_omemo_messages();
}

// Build and send an OMEMO:2 KeyTransportElement to `peer_jid` for `device_id`.
// A key-transport message establishes the Signal session from our side without
// sending any plaintext body.  Per XEP-0384 §7.3, it is a <message> stanza
// carrying <encrypted> with a <header> (keys) but NO <payload> element.
// This allows the remote party to learn our device_id and start encrypting
// future messages to us.
static void send_omemo2_key_transport(omemo &self,
                                      weechat::account &account,
                                      struct t_gui_buffer *buffer,
                                      const char *peer_jid,
                                      std::uint32_t remote_device_id)
{
    if (!self || !peer_jid)
        return;

    // Generate a fresh random 48-byte transport key bundle (key32 || hmac16).
    // We use omemo2_encrypt on a dummy single-byte plaintext just to get a
    // properly-derived key; the resulting payload is discarded.
    const auto ep = omemo2_encrypt(std::string_view("\x00", 1));
    if (!ep)
    {
        print_error(buffer, fmt::format(
            "OMEMO: key-transport key generation failed for {}/{}",
            peer_jid, remote_device_id));
        return;
    }

    // Build <encrypted xmlns='urn:xmpp:omemo:2'>
    xmpp_stanza_t *encrypted = xmpp_stanza_new(*account.context);
    xmpp_stanza_set_name(encrypted, "encrypted");
    xmpp_stanza_set_ns(encrypted, kOmemoNs.data());

    // <header sid='our_device_id'>
    xmpp_stanza_t *header = xmpp_stanza_new(*account.context);
    xmpp_stanza_set_name(header, "header");
    xmpp_stanza_set_attribute(header, "sid", fmt::format("{}", self.device_id).c_str());

    auto add_omemo2_key = [&](const std::string &target_jid,
                              std::uint32_t target_device_id) -> bool
    {
        if (!is_valid_omemo_device_id(target_device_id))
            return false;

        const auto target_transport = encrypt_transport_key(self, target_jid.c_str(), target_device_id, *ep);
        if (!target_transport)
            return false;

        const auto target_encoded_transport = base64_encode(*account.context,
                                                            target_transport->first.data(),
                                                            target_transport->first.size());

        xmpp_stanza_t *keys = xmpp_stanza_new(*account.context);
        xmpp_stanza_set_name(keys, "keys");
        xmpp_stanza_set_attribute(keys, "jid", target_jid.c_str());

        xmpp_stanza_t *key_elem = xmpp_stanza_new(*account.context);
        xmpp_stanza_set_name(key_elem, "key");
        xmpp_stanza_set_attribute(key_elem, "rid", fmt::format("{}", target_device_id).c_str());
        if (target_transport->second)
            xmpp_stanza_set_attribute(key_elem, "kex", "true");

        xmpp_stanza_t *key_text = xmpp_stanza_new(*account.context);
        xmpp_stanza_set_text(key_text, target_encoded_transport.c_str());
        xmpp_stanza_add_child(key_elem, key_text);
        xmpp_stanza_release(key_text);

        xmpp_stanza_add_child(keys, key_elem);
        xmpp_stanza_release(key_elem);
        xmpp_stanza_add_child(header, keys);
        xmpp_stanza_release(keys);
        return true;
    };

    // Recipient-side key transport target.
    bool added_any_key = add_omemo2_key(peer_jid, remote_device_id);

    // Also include our other own devices so carbon-copied stanzas can be
    // decrypted on sibling clients (e.g., Gajim), avoiding spurious warnings.
    xmpp_string_guard own_bare_g(*account.context,
                                 xmpp_jid_bare(*account.context, account.jid().data()));
    const std::string own_bare_jid = own_bare_g.ptr && *own_bare_g.ptr
        ? own_bare_g.ptr
        : std::string(account.jid());
    const auto own_devicelist = load_string(self, key_for_devicelist(own_bare_jid));
    if (own_devicelist)
    {
        for (const auto &dev : split(*own_devicelist, ';'))
        {
            const auto own_device = parse_uint32(dev);
            if (!own_device || *own_device == self.device_id)
                continue;

            if (self.failed_session_bootstrap.count({own_bare_jid, *own_device}) > 0)
                continue;

            if (!self.has_session(own_bare_jid.c_str(), *own_device)
                && !establish_session_from_bundle(self, *account.context, own_bare_jid, *own_device))
            {
                request_bundle(account, own_bare_jid, *own_device);
                continue;
            }

            if (add_omemo2_key(own_bare_jid, *own_device))
                added_any_key = true;
        }
    }

    if (!added_any_key)
    {
        xmpp_stanza_release(header);
        xmpp_stanza_release(encrypted);
        print_error(buffer, fmt::format(
            "OMEMO: key-transport encrypt failed for {}/{}",
            peer_jid, remote_device_id));
        return;
    }

    xmpp_stanza_add_child(encrypted, header);
    xmpp_stanza_release(header);

    // Wrap in a <message type='chat' to='peer_jid'>
    xmpp_stanza_t *message = xmpp_message_new(*account.context, "chat", peer_jid, nullptr);
    xmpp_stanza_add_child(message, encrypted);
    xmpp_stanza_release(encrypted);

    // Add <store xmlns='urn:xmpp:hints'/> so the server archives it and
    // Conversations can pick up our session even if it's offline.
    xmpp_stanza_t *store_hint = xmpp_stanza_new(*account.context);
    xmpp_stanza_set_name(store_hint, "store");
    xmpp_stanza_set_ns(store_hint, "urn:xmpp:hints");
    xmpp_stanza_add_child(message, store_hint);
    xmpp_stanza_release(store_hint);

    print_info(buffer, fmt::format(
        "OMEMO: sending key-transport to {}/{} (kex={})",
        peer_jid, remote_device_id, "mixed"));

    account.connection.send(message);
    xmpp_stanza_release(message);
}

static void send_legacy_key_transport(omemo &self,
                                      weechat::account &account,
                                      struct t_gui_buffer *buffer,
                                      const char *peer_jid,
                                      std::uint32_t remote_device_id)
{
    if (!self || !peer_jid)
        return;

    const auto ep = legacy_omemo_encrypt(std::string_view("\x00", 1));
    if (!ep)
    {
        print_error(buffer, fmt::format(
            "OMEMO (legacy): key-transport key generation failed for {}/{}",
            peer_jid, remote_device_id));
        return;
    }

    const auto encoded_iv = base64_encode(*account.context,
                                          ep->iv.data(),
                                          ep->iv.size());

    xmpp_stanza_t *encrypted = xmpp_stanza_new(*account.context);
    xmpp_stanza_set_name(encrypted, "encrypted");
    xmpp_stanza_set_ns(encrypted, kLegacyOmemoNs.data());

    xmpp_stanza_t *header = xmpp_stanza_new(*account.context);
    xmpp_stanza_set_name(header, "header");
    xmpp_stanza_set_attribute(header, "sid", fmt::format("{}", self.device_id).c_str());

    auto add_legacy_key = [&](const std::string &target_jid,
                              std::uint32_t target_device_id) -> bool
    {
        if (!is_valid_omemo_device_id(target_device_id))
            return false;

        const auto target_transport = encrypt_legacy_transport_key(self, target_jid, target_device_id, *ep);
        if (!target_transport)
            return false;

        const auto target_encoded_transport = base64_encode(*account.context,
                                                            target_transport->first.data(),
                                                            target_transport->first.size());

        xmpp_stanza_t *key_elem = xmpp_stanza_new(*account.context);
        xmpp_stanza_set_name(key_elem, "key");
        xmpp_stanza_set_attribute(key_elem, "rid", fmt::format("{}", target_device_id).c_str());
        if (target_transport->second)
            xmpp_stanza_set_attribute(key_elem, "prekey", "true");

        xmpp_stanza_t *key_text = xmpp_stanza_new(*account.context);
        xmpp_stanza_set_text(key_text, target_encoded_transport.c_str());
        xmpp_stanza_add_child(key_elem, key_text);
        xmpp_stanza_release(key_text);
        xmpp_stanza_add_child(header, key_elem);
        xmpp_stanza_release(key_elem);
        return true;
    };

    bool added_any_key = add_legacy_key(peer_jid, remote_device_id);

    xmpp_string_guard own_bare_g(*account.context,
                                 xmpp_jid_bare(*account.context, account.jid().data()));
    const std::string own_bare_jid = own_bare_g.ptr && *own_bare_g.ptr
        ? own_bare_g.ptr
        : std::string(account.jid());

    auto add_own_legacy_targets = [&](const std::string &device_list_str)
    {
        for (const auto &dev : split(device_list_str, ';'))
        {
            const auto own_device = parse_uint32(dev);
            if (!own_device || *own_device == self.device_id)
                continue;

            if (self.failed_session_bootstrap.count({own_bare_jid, *own_device}) > 0)
                continue;

            if (!self.has_session(own_bare_jid.c_str(), *own_device)
                && !establish_session_from_bundle(self, *account.context, own_bare_jid, *own_device))
            {
                request_legacy_bundle(account, own_bare_jid, *own_device);
                continue;
            }

            if (add_legacy_key(own_bare_jid, *own_device))
                added_any_key = true;
        }
    };

    const auto own_legacy_devicelist = load_string(self, key_for_legacy_devicelist(own_bare_jid));
    if (own_legacy_devicelist && !own_legacy_devicelist->empty())
    {
        add_own_legacy_targets(*own_legacy_devicelist);
    }
    else
    {
        const auto own_omemo2_devicelist = load_string(self, key_for_devicelist(own_bare_jid));
        if (own_omemo2_devicelist && !own_omemo2_devicelist->empty())
            add_own_legacy_targets(*own_omemo2_devicelist);
    }

    if (!added_any_key)
    {
        xmpp_stanza_release(header);
        xmpp_stanza_release(encrypted);
        print_error(buffer, fmt::format(
            "OMEMO (legacy): key-transport encrypt failed for {}/{}",
            peer_jid, remote_device_id));
        return;
    }

    xmpp_stanza_t *iv_stanza = xmpp_stanza_new(*account.context);
    xmpp_stanza_set_name(iv_stanza, "iv");
    xmpp_stanza_t *iv_text = xmpp_stanza_new(*account.context);
    xmpp_stanza_set_text(iv_text, encoded_iv.c_str());
    xmpp_stanza_add_child(iv_stanza, iv_text);
    xmpp_stanza_release(iv_text);
    xmpp_stanza_add_child(header, iv_stanza);
    xmpp_stanza_release(iv_stanza);

    xmpp_stanza_add_child(encrypted, header);
    xmpp_stanza_release(header);

    xmpp_stanza_t *message = xmpp_message_new(*account.context, "chat", peer_jid, nullptr);
    xmpp_stanza_add_child(message, encrypted);
    xmpp_stanza_release(encrypted);

    xmpp_stanza_t *store_hint = xmpp_stanza_new(*account.context);
    xmpp_stanza_set_name(store_hint, "store");
    xmpp_stanza_set_ns(store_hint, "urn:xmpp:hints");
    xmpp_stanza_add_child(message, store_hint);
    xmpp_stanza_release(store_hint);

    print_info(buffer, fmt::format(
        "OMEMO (legacy): sending key-transport to {}/{} (prekey={})",
        peer_jid, remote_device_id, "mixed"));

    account.connection.send(message);
    xmpp_stanza_release(message);
}

static void send_key_transport(omemo &self,
                               weechat::account &account,
                               struct t_gui_buffer *buffer,
                               const char *peer_jid,
                               std::uint32_t remote_device_id)
{
    if (!self || !peer_jid)
        return;

    const std::string bare_jid = normalize_bare_jid(*account.context, peer_jid);
    const auto mode = resolve_device_mode(self, account, bare_jid, remote_device_id,
                                          omemo::peer_mode::omemo2);

    if (mode == omemo::peer_mode::legacy)
    {
        send_legacy_key_transport(self, account, buffer, peer_jid, remote_device_id);
        return;
    }

    send_omemo2_key_transport(self, account, buffer, peer_jid, remote_device_id);
}

void weechat::xmpp::omemo::handle_bundle(weechat::account *account,
                                         struct t_gui_buffer *buffer,
                                         const char *jid,
                                         std::uint32_t remote_device_id,
                                         xmpp_stanza_t *items)
{
    pending_bundle_fetch.erase({jid ? jid : "", remote_device_id});
    const bool needs_key_transport = pending_key_transport.count({jid ? jid : "", remote_device_id}) > 0;
    pending_key_transport.erase({jid ? jid : "", remote_device_id});

    // Do not attempt X3DH session establishment with our own device.
    // Building a session with ourselves would fail in libsignal (we can't
    // be both initiator and responder), and the resulting exception thrown
    // through a C libstrophe callback would corrupt the stack.
    std::string bare_jid = jid ? jid : "";
    std::string own_bare_jid;
    if (account && account->context && jid)
    {
        xmpp_string_guard jid_bare_g(*account->context,
                                     xmpp_jid_bare(*account->context, jid));
        if (jid_bare_g.ptr && *jid_bare_g.ptr)
            bare_jid = jid_bare_g.ptr;

        xmpp_string_guard own_bare_g(*account->context,
                                     xmpp_jid_bare(*account->context, account->jid().data()));
        if (own_bare_g.ptr && *own_bare_g.ptr)
            own_bare_jid = own_bare_g.ptr;
        else
            own_bare_jid = account->jid();
    }
    const bool is_own_device = (account && !bare_jid.empty() && own_bare_jid == bare_jid)
                                && (remote_device_id == device_id);

    if (db_env && !bare_jid.empty())
    {
        if (const auto bundle = extract_bundle_from_items(items))
        {
            store_bundle(*this, bare_jid, remote_device_id, *bundle);
            store_device_mode(*this, bare_jid, remote_device_id, peer_mode::omemo2);
            if (!is_own_device)
            {
                failed_session_bootstrap.erase({bare_jid, remote_device_id});
                const bool had_session_before = has_session(bare_jid.c_str(), remote_device_id);
                try
                {
                    (void)establish_session_from_bundle(
                        *this, account ? *account->context : nullptr, bare_jid, remote_device_id);
                }
                catch (const std::exception &ex)
                {
                    if (buffer)
                        print_error(buffer, fmt::format(
                            "OMEMO session setup failed for {}/{}: {}",
                            jid, remote_device_id, ex.what()));
                }
                if (!has_session(bare_jid.c_str(), remote_device_id))
                    failed_session_bootstrap.insert({bare_jid, remote_device_id});
                const bool session_is_fresh = !had_session_before
                    && has_session(bare_jid.c_str(), remote_device_id);
                if ((session_is_fresh || needs_key_transport) && account && buffer)
                {
                    // Mark attempted regardless of defer/send so codec.inl
                    // doesn't re-queue another bundle fetch for this device.
                    key_transport_bootstrap_attempted.insert({bare_jid, remote_device_id});
                    if (global_mam_catchup)
                    {
                        // Defer until MAM catchup completes (process_postponed_key_transports).
                        postponed_key_transports.insert({bare_jid, remote_device_id});
                    }
                    else
                    {
                        send_key_transport(*this, *account, buffer, bare_jid.c_str(), remote_device_id);
                    }
                }
            }
        }
    }

    if (buffer && !bare_jid.empty())
    {
        const auto bundle = db_env ? load_bundle(*this, bare_jid, remote_device_id) : std::nullopt;
        const auto prekey_count = bundle ? bundle->prekeys.size() : 0U;
        if (!is_own_device)
            print_info(buffer, fmt::format(
                "OMEMO received bundle for {}/{} ({} prekeys).",
                bare_jid, remote_device_id, prekey_count));
    }

    // After successfully building a session with a remote contact's device,
    // auto-enable OMEMO on the corresponding PM channel if it has no transport
    // set yet. This ensures that the next message the user sends is encrypted
    // even if they haven't manually run /omemo on.
    if (!is_own_device
        && account
        && !bare_jid.empty()
        && has_session(bare_jid.c_str(), remote_device_id))
    {
        auto ch_it = account->channels.find(bare_jid);
        if (ch_it != account->channels.end())
        {
            auto &ch = ch_it->second;
            if (ch.type == weechat::channel::chat_type::PM
                && !ch.omemo.enabled
                && ch.transport == weechat::channel::transport::PLAIN)
            {
                weechat_printf(ch.buffer,
                               "%sAuto-enabling OMEMO (OMEMO session established with %s)",
                               weechat_prefix("network"), bare_jid.c_str());
                ch.omemo.enabled = 1;
                ch.set_transport(weechat::channel::transport::OMEMO, 0);
            }

            if (ch.type == weechat::channel::chat_type::PM)
                ch.flush_pending_omemo_messages();
        }

    }
}


    // Identical logic to handle_bundle() but uses extract_legacy_bundle_from_items()
    // which parses the Conversations/axolotl stanza format.
    void weechat::xmpp::omemo::handle_legacy_bundle(weechat::account *account,
                                                    struct t_gui_buffer *buffer,
                                                    const char *jid,
                                                    std::uint32_t remote_device_id,
                                                    xmpp_stanza_t *items)
    {
        pending_bundle_fetch.erase({jid ? jid : "", remote_device_id});
        const bool needs_key_transport = pending_key_transport.count({jid ? jid : "", remote_device_id}) > 0;
        pending_key_transport.erase({jid ? jid : "", remote_device_id});

        std::string bare_jid = jid ? jid : "";
        std::string own_bare_jid;
        if (account && account->context && jid)
        {
            xmpp_string_guard jid_bare_g(*account->context,
                                         xmpp_jid_bare(*account->context, jid));
            if (jid_bare_g.ptr && *jid_bare_g.ptr)
                bare_jid = jid_bare_g.ptr;

            xmpp_string_guard own_bare_g(*account->context,
                                         xmpp_jid_bare(*account->context, account->jid().data()));
            if (own_bare_g.ptr && *own_bare_g.ptr)
                own_bare_jid = own_bare_g.ptr;
            else
                own_bare_jid = account->jid();
        }
        const bool is_own_device = (account && !bare_jid.empty() && own_bare_jid == bare_jid)
                                    && (remote_device_id == device_id);

        if (db_env && !bare_jid.empty())
        {
            if (const auto bundle = extract_legacy_bundle_from_items(items))
            {
                // Store under the legacy key prefix so we know it came from the
                // axolotl namespace; the Signal session is shared with OMEMO:2.
                store_string(*this, key_for_legacy_bundle(bare_jid, remote_device_id),
                             serialize_bundle(*bundle));
                // Also store under the canonical bundle key so establish_session_from_bundle()
                // can find it without knowing which namespace it came from.
                store_bundle(*this, bare_jid, remote_device_id, *bundle);
                store_device_mode(*this, bare_jid, remote_device_id, peer_mode::legacy);

                if (!is_own_device)
                {
                    failed_session_bootstrap.erase({bare_jid, remote_device_id});
                    const bool had_session_before = has_session(bare_jid.c_str(), remote_device_id);
                    try
                    {
                        (void)establish_session_from_bundle(
                            *this, account ? *account->context : nullptr, bare_jid, remote_device_id);
                    }
                    catch (const std::exception &ex)
                    {
                        if (buffer)
                            print_error(buffer, fmt::format(
                                "OMEMO (legacy) session setup failed for {}/{}: {}",
                                jid, remote_device_id, ex.what()));
                    }
                    if (!has_session(bare_jid.c_str(), remote_device_id))
                        failed_session_bootstrap.insert({bare_jid, remote_device_id});
                    const bool session_is_fresh = !had_session_before
                        && has_session(bare_jid.c_str(), remote_device_id);
                    if ((session_is_fresh || needs_key_transport) && account && buffer)
                    {
                        key_transport_bootstrap_attempted.insert({bare_jid, remote_device_id});
                        if (global_mam_catchup)
                        {
                            postponed_key_transports.insert({bare_jid, remote_device_id});
                        }
                        else
                        {
                            send_key_transport(*this, *account, buffer, bare_jid.c_str(), remote_device_id);
                        }
                    }
                }

                if (buffer && !bare_jid.empty() && !is_own_device)
                {
                    const auto &lpks = bundle->prekeys;
                    print_info(buffer, fmt::format(
                        "OMEMO (legacy) received bundle for {}/{} ({} prekeys).",
                        bare_jid, remote_device_id, lpks.size()));
                }
            }
            else
            {
                if (buffer)
                    print_error(buffer, fmt::format(
                        "OMEMO (legacy) failed to parse bundle for {}/{}",
                        bare_jid, remote_device_id));
            }
        }

        // After successfully building a session, flush queued PM messages.
        if (!is_own_device
            && account
            && !bare_jid.empty()
            && has_session(bare_jid.c_str(), remote_device_id))
        {
            auto ch_it = account->channels.find(bare_jid);
            if (ch_it != account->channels.end())
            {
                auto &ch = ch_it->second;
                if (ch.type == weechat::channel::chat_type::PM)
                {
                    if (!ch.omemo.enabled
                        && ch.transport == weechat::channel::transport::PLAIN)
                    {
                        weechat_printf(ch.buffer,
                                       "%sAuto-enabling OMEMO (legacy session established with %s)",
                                       weechat_prefix("network"), bare_jid.c_str());
                        ch.omemo.enabled = 1;
                        ch.set_transport(weechat::channel::transport::OMEMO, 0);
                    }
                    ch.flush_pending_omemo_messages();
                }
            }
        }
    }


// Fire all key transports that were deferred during global MAM catchup.
// Called once from iq_handler.inl when the global MAM <fin> arrives.
void weechat::xmpp::omemo::process_postponed_key_transports(weechat::account &account)
{
    if (postponed_key_transports.empty())
        return;

    weechat_printf(account.buffer,
                   "%somemo: MAM catchup complete \xe2\x80\x94 sending %zu deferred key-transport(s)",
                   weechat_prefix("network"),
                   postponed_key_transports.size());

    for (const auto &[bare_jid, device_id] : postponed_key_transports)
    {
        struct t_gui_buffer *buf = account.buffer;
        auto ch_it = account.channels.find(bare_jid);
        if (ch_it != account.channels.end())
            buf = ch_it->second.buffer;

        send_key_transport(*this, account, buf, bare_jid.c_str(), device_id);
    }
    postponed_key_transports.clear();
}
