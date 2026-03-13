bool weechat::xmpp::omemo::has_session(const char *jid, std::uint32_t remote_device_id)
{
    OMEMO_ASSERT(jid != nullptr, "session lookup requires a non-null jid");

    if (!db_env || !jid)
        return false;

    auto address = make_signal_address(jid, static_cast<std::int32_t>(remote_device_id));
    return signal_protocol_session_contains_session(store_context, &address.address) == 1;
}

char *weechat::xmpp::omemo::decode(weechat::account *account,
                                   struct t_gui_buffer *buffer,
                                   const char *jid,
                                   xmpp_stanza_t *encrypted)
{
    OMEMO_ASSERT(account != nullptr, "OMEMO decode requires a valid account");
    OMEMO_ASSERT(jid != nullptr, "OMEMO decode requires a peer jid");
    OMEMO_ASSERT(encrypted != nullptr, "OMEMO decode requires an encrypted stanza");

    if (!account || !jid || !encrypted)
    {
        print_error(buffer, "OMEMO decode received invalid input.");
        return nullptr;
    }

    xmpp_stanza_t *header = xmpp_stanza_get_child_by_name(encrypted, "header");
    xmpp_stanza_t *payload_stanza = xmpp_stanza_get_child_by_name(encrypted, "payload");
    if (!header)
    {
        print_error(buffer, "OMEMO message is missing header.");
        return nullptr;
    }

    const auto sender_device_id = parse_uint32(
        xmpp_stanza_get_attribute(header, "sid")
            ? xmpp_stanza_get_attribute(header, "sid")
            : "");
    if (!sender_device_id)
    {
        print_error(buffer, "OMEMO message header is missing a valid sender sid.");
        return nullptr;
    }

    // payload_stanza may be absent for KeyTransportElement (XEP-0384 §7.3).
    const auto payload_text = payload_stanza ? stanza_text(payload_stanza) : std::string {};
    const auto payload = payload_stanza
        ? base64_decode(*account->context, payload_text)
        : std::vector<std::uint8_t> {};
    // A missing or empty payload is only an error for full encrypted messages,
    // not for key-transport stanzas.
    if (payload_stanza && payload.empty())
    {
        print_error(buffer, "OMEMO payload is present but empty or invalid base64.");
        return nullptr;
    }

    xmpp_string_guard own_bare_g(*account->context,
                                 xmpp_jid_bare(*account->context, account->jid().data()));
    const std::string own_bare_jid = own_bare_g ? own_bare_g.str() : std::string(account->jid());

    // Detect legacy (eu.siacs.conversations.axolotl) format by presence of <iv> in header.
    // Legacy uses AES-128-GCM with explicit IV; OMEMO:2 uses HKDF-derived keys.
    xmpp_stanza_t *iv_stanza = xmpp_stanza_get_child_by_name(header, "iv");
    const bool is_legacy_format = (iv_stanza != nullptr);

    store_device_mode(*this,
                      normalize_bare_jid(*account->context, jid),
                      *sender_device_id,
                      is_legacy_format ? peer_mode::legacy : peer_mode::omemo2);

    // OMEMO:2 transport key: {key32, hmac16}
    std::optional<std::pair<std::array<std::uint8_t, 32>, std::array<std::uint8_t, 16>>> transport_key;
    // Legacy transport key: {innerKey16, authTag16}
    std::optional<std::pair<std::array<std::uint8_t, 16>, std::array<std::uint8_t, 16>>> legacy_transport_key;
    std::optional<std::uint32_t> used_prekey_id;
    bool found_keys_elem = false;
    bool found_keys_for_our_bare_jid = false;
    bool found_key_for_us = false;
    for (xmpp_stanza_t *child = xmpp_stanza_get_children(header);
         child && !transport_key && !legacy_transport_key;
         child = xmpp_stanza_get_next(child))
    {
        const char *name = xmpp_stanza_get_name(child);
        if (!name || weechat_strcasecmp(name, "keys") != 0)
            continue;
        found_keys_elem = true;

        const char *keys_jid = xmpp_stanza_get_attribute(child, "jid");
        print_info(buffer, fmt::format("OMEMO decode: <keys jid='{}'> element found",
                                       keys_jid ? keys_jid : "(none)"));
        if (!keys_jid)
        {
            // Legacy compatibility: OMEMO:1 senders may omit keys@jid.
            print_info(buffer,
                       "OMEMO decode: accepting legacy <keys> without jid attribute.");
            found_keys_for_our_bare_jid = true;
        }

        if (keys_jid && std::string_view {keys_jid}.find('/') != std::string_view::npos)
        {
            print_error(buffer, fmt::format(
                "OMEMO message has non-bare keys jid '{}'; ignoring non-compliant element.",
                keys_jid));
            continue;
        }

        if (keys_jid && own_bare_jid != keys_jid)
            continue;

        if (keys_jid)
            found_keys_for_our_bare_jid = true;

        for (xmpp_stanza_t *key_stanza = xmpp_stanza_get_children(child);
             key_stanza && !transport_key && !legacy_transport_key;
             key_stanza = xmpp_stanza_get_next(key_stanza))
        {
            const char *key_name = xmpp_stanza_get_name(key_stanza);
            if (!key_name || weechat_strcasecmp(key_name, "key") != 0)
                continue;

            const char *rid = xmpp_stanza_get_attribute(key_stanza, "rid");
            const auto rid_val = rid ? parse_uint32(rid).value_or(0) : 0;
            print_info(buffer, fmt::format("OMEMO decode:   <key rid='{}'> (our device_id={})",
                                           rid ? rid : "(null)", device_id));
            if (rid_val != device_id)
                continue;

            found_key_for_us = true;
            const char *kex_val = xmpp_stanza_get_attribute(key_stanza, "kex");
            const char *legacy_prekey_val = xmpp_stanza_get_attribute(key_stanza, "prekey");
            const bool is_prekey = (kex_val != nullptr
                && (weechat_strcasecmp(kex_val, "true") == 0
                    || weechat_strcasecmp(kex_val, "1") == 0))
                || (kex_val == nullptr && legacy_prekey_val != nullptr
                    && (weechat_strcasecmp(legacy_prekey_val, "true") == 0
                        || weechat_strcasecmp(legacy_prekey_val, "1") == 0));
            if (!kex_val && legacy_prekey_val)
            {
                print_info(buffer,
                           "OMEMO decode: accepting legacy 'prekey' key attribute; strict XEP-0384 uses 'kex'.");
            }
            const auto serialized = base64_decode(*account->context, stanza_text(key_stanza));
            print_info(buffer, fmt::format("OMEMO decode:   found key for us: is_prekey={} serialized-bytes={}",
                                           is_prekey, serialized.size()));
            if (serialized.empty())
            {
                print_error(buffer, "OMEMO key element for our device has empty/invalid base64.");
                continue;
            }

            if (is_legacy_format)
            {
                legacy_transport_key = decrypt_legacy_transport_key(*this, jid, *sender_device_id, serialized, is_prekey,
                                                                     is_prekey ? &used_prekey_id : nullptr);
                if (!legacy_transport_key)
                    print_error(buffer, "OMEMO (legacy) Signal decryption of transport key failed.");
            }
            else
            {
                transport_key = decrypt_transport_key(*this, jid, *sender_device_id, serialized, is_prekey,
                                                      is_prekey ? &used_prekey_id : nullptr);
                if (!transport_key)
                    print_error(buffer, "OMEMO Signal decryption of transport key failed.");
            }
        }
    }

    // Legacy compatibility: some OMEMO:1 payloads place <key/> elements
    // directly under <header> instead of wrapping them in <keys/>.
    if (!transport_key && !legacy_transport_key && !found_keys_elem)
    {
        for (xmpp_stanza_t *key_stanza = xmpp_stanza_get_children(header);
             key_stanza && !transport_key && !legacy_transport_key;
             key_stanza = xmpp_stanza_get_next(key_stanza))
        {
            const char *key_name = xmpp_stanza_get_name(key_stanza);
            if (!key_name || weechat_strcasecmp(key_name, "key") != 0)
                continue;

            // Legacy OMEMO:1 layout: <header><key .../></header> has no
            // intermediate <keys jid='...'> wrapper. Seeing at least one
            // <key/> means keys are present for this (bare) peer.
            found_keys_elem = true;
            found_keys_for_our_bare_jid = true;

            const char *rid = xmpp_stanza_get_attribute(key_stanza, "rid");
            const auto rid_val = rid ? parse_uint32(rid).value_or(0) : 0;
            if (rid_val != device_id)
                continue;

            found_key_for_us = true;

            const char *kex_val = xmpp_stanza_get_attribute(key_stanza, "kex");
            const char *legacy_prekey_val = xmpp_stanza_get_attribute(key_stanza, "prekey");
            const bool is_prekey = (kex_val != nullptr
                && (weechat_strcasecmp(kex_val, "true") == 0
                    || weechat_strcasecmp(kex_val, "1") == 0))
                || (kex_val == nullptr && legacy_prekey_val != nullptr
                    && (weechat_strcasecmp(legacy_prekey_val, "true") == 0
                        || weechat_strcasecmp(legacy_prekey_val, "1") == 0));

            const auto serialized = base64_decode(*account->context, stanza_text(key_stanza));
            if (serialized.empty())
                continue;

            if (is_legacy_format)
            {
                legacy_transport_key = decrypt_legacy_transport_key(*this, jid, *sender_device_id, serialized, is_prekey,
                                                                     is_prekey ? &used_prekey_id : nullptr);
            }
            else
            {
                transport_key = decrypt_transport_key(*this, jid, *sender_device_id, serialized, is_prekey,
                                                      is_prekey ? &used_prekey_id : nullptr);
            }
        }
    }

    if (!transport_key && !legacy_transport_key)
    {
        if (!found_keys_elem)
            print_error(buffer, "OMEMO message has no <keys> element in header.");
        else if (!found_keys_for_our_bare_jid)
            print_error(buffer, fmt::format(
                "OMEMO message has no <keys jid='{}'> element for our bare JID.",
                own_bare_jid));
        else if (!found_key_for_us)
        {
            // The sender does not (yet) know our device_id. Request their bundle
            // once per peer device and avoid repeating the same error every message.
            if (account && sender_device_id)
            {
                const std::string bare_jid = normalize_bare_jid(*account->context, jid);
                const auto key = std::make_pair(bare_jid, *sender_device_id);
                const bool already_attempted = key_transport_bootstrap_attempted.count(key) != 0;
                const bool already_pending = pending_key_transport.count(key) != 0;

                if (!already_attempted && !already_pending)
                {
                    print_error(buffer, fmt::format(
                        "OMEMO message has no key for our device {} (sender did not encrypt for us).",
                        device_id));

                    pending_key_transport.insert(key);
                    const auto selected_mode = resolve_device_mode(
                        *this,
                        *account,
                        bare_jid,
                        *sender_device_id,
                        is_legacy_format ? peer_mode::legacy : peer_mode::unknown);

                    if (selected_mode == peer_mode::legacy)
                        request_legacy_bundle(*account, bare_jid, *sender_device_id);
                    else
                        request_bundle(*account, bare_jid, *sender_device_id);
                }
            }
            else
            {
                print_error(buffer, fmt::format(
                    "OMEMO message has no key for our device {} (sender did not encrypt for us).",
                    device_id));
            }
        }
        else
            print_error(buffer, "OMEMO transport key decryption failed.");
        return nullptr;
    }

    // Key-transport element: no payload, nothing more to decrypt.
    // The session is now established from our side.
    if (!payload_stanza || payload.empty())
    {
        print_info(buffer, "OMEMO: received KeyTransportElement — session established.");
        return nullptr;
    }

    // Legacy format: AES-128-GCM decrypt path (no SCE wrapping)
    if (is_legacy_format && legacy_transport_key)
    {
        const auto iv_text = stanza_text(iv_stanza);
        const auto iv_vec = base64_decode(*account->context, iv_text);
        if (iv_vec.size() != 12)
        {
            print_error(buffer, "OMEMO (legacy): IV element has wrong size (expected 12 bytes).");
            return nullptr;
        }
        std::array<std::uint8_t, 12> iv {};
        std::copy_n(iv_vec.begin(), 12, iv.begin());
        const auto result = legacy_omemo_decrypt(legacy_transport_key->first, iv,
                                                 legacy_transport_key->second, payload);
        if (!result)
        {
            print_error(buffer, "OMEMO (legacy) payload decryption failed.");
            return nullptr;
        }
        if (used_prekey_id && account)
        {
            if (replace_used_prekey(*this, *account->context, *used_prekey_id))
            {
                print_info(buffer, fmt::format(
                    "OMEMO: replaced consumed pre-key {} — republishing bundle",
                    *used_prekey_id));
                xmpp_stanza_t *bundle_stanza = get_bundle(*account->context, nullptr, nullptr);
                if (bundle_stanza)
                {
                    account->connection.send(bundle_stanza);
                    xmpp_stanza_release(bundle_stanza);
                }

                xmpp_stanza_t *legacy_bundle_stanza = get_legacy_bundle(*account->context, nullptr, nullptr);
                if (legacy_bundle_stanza)
                {
                    account->connection.send(legacy_bundle_stanza);
                    xmpp_stanza_release(legacy_bundle_stanza);
                }
            }
        }
        return strdup(result->c_str());
    }

    const auto decrypted_xml = omemo2_decrypt(transport_key->first, transport_key->second, payload);
    if (!decrypted_xml)
    {
        print_error(buffer, "OMEMO payload decryption failed.");
        return nullptr;
    }

    // Attempt SCE (XEP-0420) unwrap first; fall back to treating the decrypted
    // bytes as plain UTF-8 text (e.g. older XEP-0384 drafts or non-SCE senders).
    const auto body = sce_unwrap(*account->context, *decrypted_xml);
    if (!body)
    {
        // Fallback: return the raw decrypted text directly.
        if (!decrypted_xml->empty())
            return strdup(decrypted_xml->c_str());
        print_error(buffer, "OMEMO SCE envelope unwrap failed and payload is empty.");
        return nullptr;
    }

    // Per XEP-0384 §7.3 and Signal protocol best practice: after successfully
    // decrypting a PreKeySignalMessage (first message from a device), replace
    // the consumed pre-key with a fresh one of the same ID and republish the
    // bundle so contacts always have fresh pre-keys available.
    if (used_prekey_id && account)
    {
        if (replace_used_prekey(*this, *account->context, *used_prekey_id))
        {
            print_info(buffer, fmt::format(
                "OMEMO: replaced consumed pre-key {} — republishing bundle",
                *used_prekey_id));
            xmpp_stanza_t *bundle_stanza = get_bundle(*account->context, nullptr, nullptr);
            if (bundle_stanza)
            {
                account->connection.send(bundle_stanza);
                xmpp_stanza_release(bundle_stanza);
            }

            xmpp_stanza_t *legacy_bundle_stanza = get_legacy_bundle(*account->context, nullptr, nullptr);
            if (legacy_bundle_stanza)
            {
                account->connection.send(legacy_bundle_stanza);
                xmpp_stanza_release(legacy_bundle_stanza);
            }
        }
        else
        {
            print_error(buffer, fmt::format(
                "OMEMO: failed to replace consumed pre-key {} (non-fatal)",
                *used_prekey_id));
        }
    }

    return strdup(body->c_str());
}

xmpp_stanza_t *weechat::xmpp::omemo::encode(weechat::account *account,
                                            struct t_gui_buffer *buffer,
                                            const char *jid,
                                            const char *unencrypted)
{
    OMEMO_ASSERT(account != nullptr, "OMEMO encode requires a valid account");
    OMEMO_ASSERT(jid != nullptr, "OMEMO encode requires a peer jid");
    OMEMO_ASSERT(unencrypted != nullptr, "OMEMO encode requires plaintext input");

    if (!*this)
    {
        print_error(buffer, "OMEMO is not initialized.");
        return nullptr;
    }

    if (!account || !jid || !unencrypted)
    {
        print_error(buffer, "OMEMO encode received invalid input.");
        return nullptr;
    }

    ensure_local_identity(*this);
    ensure_registration_id(*this);
    ensure_prekeys(*this, *account->context);

    // Conversations addresses OMEMO device/session material by bare JID.
    // Normalize once here so devicelist/session lookups don't miss when
    // messages are addressed to a full JID (user@domain/resource).
    std::string target_jid = jid;
    xmpp_string_guard target_bare_g(*account->context,
                                    xmpp_jid_bare(*account->context, jid));
    if (target_bare_g.ptr && *target_bare_g.ptr)
        target_jid = target_bare_g.ptr;

    note_peer_traffic(account->context, target_jid);

    const auto devicelist = load_string(*this, key_for_devicelist(target_jid));
    if (!devicelist || devicelist->empty())
    {
        print_error(buffer, fmt::format(
            "OMEMO: no device list in cache for {} (nullptr={})", 
            target_jid, !devicelist ? "yes" : "no"));
        request_devicelist(*account, target_jid);
        print_error(buffer, fmt::format(
            "OMEMO has no known device list for {}. Requested the contact's OMEMO devices; retry after they arrive.", jid));
        return nullptr;
    }
    
    const auto sce = sce_wrap(*account->context, *account, unencrypted);
    const auto encrypted_payload = omemo2_encrypt(sce);
    if (!encrypted_payload)
    {
        print_error(buffer, "OMEMO payload encryption failed.");
        return nullptr;
    }

    xmpp_stanza_t *encrypted = xmpp_stanza_new(*account->context);
    xmpp_stanza_set_name(encrypted, "encrypted");
    xmpp_stanza_set_ns(encrypted, kOmemoNs.data());

    xmpp_stanza_t *header = xmpp_stanza_new(*account->context);
    xmpp_stanza_set_name(header, "header");
    xmpp_stanza_set_attribute(header, "sid", fmt::format("{}", device_id).c_str());

    xmpp_string_guard own_bare_g(*account->context,
        xmpp_jid_bare(*account->context, account->jid().data()));
    const std::string own_jid = own_bare_g ? own_bare_g.str() : std::string(account->jid());

    // Helper: build a <keys jid='target_jid'> element, encrypting the transport
    // key for each device in device_list_str (semicolon-separated device IDs).
    // Returns {stanza, added_count}. Stanza is nullptr if none could be added.
    auto build_keys_elem = [&](const char *target_jid,
                               const std::string &device_list_str,
                               int &out_count,
                               int *out_incomplete_count = nullptr) -> xmpp_stanza_t *
    {
        xmpp_stanza_t *keys = xmpp_stanza_new(*account->context);
        xmpp_stanza_set_name(keys, "keys");
        xmpp_stanza_set_attribute(keys, "jid", target_jid);
        out_count = 0;
        if (out_incomplete_count)
            *out_incomplete_count = 0;

        for (const auto &dev : split(device_list_str, ';'))
        {
            const auto remote_device_id = parse_uint32(dev);
            if (!remote_device_id || !is_valid_omemo_device_id(*remote_device_id))
            {
                print_error(buffer, fmt::format(
                    "OMEMO: skipping invalid device id '{}' for {}", dev, target_jid));
                continue;
            }

            // Never attempt to build a "remote" session to our own current
            // device; that path is local-only and not a valid recipient target.
            if (own_jid == target_jid && *remote_device_id == device_id)
            {
                continue;
            }

            if (failed_session_bootstrap.count({target_jid, *remote_device_id}) > 0)
            {
                continue;
            }

            if (!has_session(target_jid, *remote_device_id))
            {
                if (!establish_session_from_bundle(*this, *account->context, target_jid, *remote_device_id))
                {
                    request_bundle(*account, target_jid, *remote_device_id);
                    if (out_incomplete_count)
                        ++*out_incomplete_count;
                    continue;
                }
            }

            const auto transport = encrypt_transport_key(*this, target_jid, *remote_device_id, *encrypted_payload);
            if (!transport)
            {
                if (out_incomplete_count)
                    ++*out_incomplete_count;
                print_info(buffer, fmt::format(
                    "OMEMO failed to encrypt for device {}/{}.", target_jid, *remote_device_id));
                continue;
            }

            const auto encoded_transport = base64_encode(*account->context,
                                                         transport->first.data(),
                                                         transport->first.size());

            xmpp_stanza_t *key_stanza = xmpp_stanza_new(*account->context);
            xmpp_stanza_set_name(key_stanza, "key");
            xmpp_stanza_set_attribute(key_stanza, "rid", fmt::format("{}", *remote_device_id).c_str());
            if (transport->second)
                xmpp_stanza_set_attribute(key_stanza, "kex", "true");

            xmpp_stanza_t *key_text = xmpp_stanza_new(*account->context);
            xmpp_stanza_set_text(key_text, encoded_transport.c_str());
            xmpp_stanza_add_child(key_stanza, key_text);
            xmpp_stanza_release(key_text);

            xmpp_stanza_add_child(keys, key_stanza);
            xmpp_stanza_release(key_stanza);
            ++out_count;
        }

        if (out_count == 0)
        {
            xmpp_stanza_release(keys);
            return nullptr;
        }
        return keys;
    };

    bool added_any_key = false;

    // Encrypt for recipient devices
    {
        int count = 0;
        int incomplete_count = 0;

        xmpp_stanza_t *keys = build_keys_elem(target_jid.c_str(), *devicelist, count, &incomplete_count);
        if (keys)
        {
            xmpp_stanza_add_child(header, keys);
            xmpp_stanza_release(keys);
            added_any_key = true;
        }
        else
        {
            print_error(buffer, fmt::format(
                "OMEMO encode: failed to build keys for recipient (no valid sessions)"));
        }

        if (incomplete_count > 0)
        {
            xmpp_stanza_release(header);
            xmpp_stanza_release(encrypted);
            return nullptr;
        }
    }

    // Encrypt for own devices (other than the current one), per XEP-0384 §7.2.
    // Own devices allow the message to be readable on our other devices and
    // prevents Conversations from flagging this as "not encrypted for sender".
    {
        // Only add own-keys element if recipient is not already our own JID
        if (own_jid != target_jid)
        {
            const auto own_devicelist = load_string(*this, key_for_devicelist(own_jid));
            if (own_devicelist && !own_devicelist->empty())
            {
                int count = 0;
                xmpp_stanza_t *own_keys = build_keys_elem(own_jid.c_str(), *own_devicelist, count);
                if (own_keys)
                {
                    xmpp_stanza_add_child(header, own_keys);
                    xmpp_stanza_release(own_keys);
                    added_any_key = true;
                }
            }
        }
    }

    if (!added_any_key)
    {
        xmpp_stanza_release(header);
        xmpp_stanza_release(encrypted);
        print_error(buffer, fmt::format("OMEMO could not encrypt for any known device of {}.", target_jid));
        return nullptr;
    }

    xmpp_stanza_t *payload = xmpp_stanza_new(*account->context);
    xmpp_stanza_set_name(payload, "payload");
    const auto encoded_payload = base64_encode(*account->context,
                                               encrypted_payload->payload.data(),
                                               encrypted_payload->payload.size());
    xmpp_stanza_t *payload_text = xmpp_stanza_new(*account->context);
    xmpp_stanza_set_text(payload_text, encoded_payload.c_str());
    xmpp_stanza_add_child(payload, payload_text);
    xmpp_stanza_release(payload_text);

    xmpp_stanza_add_child(encrypted, header);
    xmpp_stanza_release(header);
    xmpp_stanza_add_child(encrypted, payload);
    xmpp_stanza_release(payload);
    return encrypted;

}

    xmpp_stanza_t *weechat::xmpp::omemo::encode_legacy(weechat::account *account,
                                                        struct t_gui_buffer *buffer,
                                                        const char *jid,
                                                        const char *unencrypted)
    {
        OMEMO_ASSERT(account != nullptr, "OMEMO encode_legacy requires a valid account");
        OMEMO_ASSERT(jid != nullptr, "OMEMO encode_legacy requires a peer jid");
        OMEMO_ASSERT(unencrypted != nullptr, "OMEMO encode_legacy requires plaintext input");

        if (!*this || !account || !jid || !unencrypted)
            return nullptr;

        ensure_local_identity(*this);
        ensure_registration_id(*this);
        ensure_prekeys(*this, *account->context);

        std::string target_jid = jid;
        xmpp_string_guard target_bare_g(*account->context, xmpp_jid_bare(*account->context, jid));
        if (target_bare_g.ptr && *target_bare_g.ptr)
            target_jid = target_bare_g.ptr;

        note_peer_traffic(account->context, target_jid);

        // Look up device list — stored under legacy key set by the legacy devicelist handler
        const auto devicelist = load_string(*this, key_for_legacy_devicelist(target_jid));
        if (!devicelist || devicelist->empty())
        {
            request_legacy_devicelist(*account, target_jid);
            print_error(buffer, fmt::format(
                "OMEMO (legacy): no device list cached for {}. Requested.", target_jid));
            return nullptr;
        }

        // Legacy OMEMO uses AES-128-GCM on the raw message text (no SCE wrapping)
        const auto ep = legacy_omemo_encrypt(std::string_view(unencrypted));
        if (!ep)
        {
            print_error(buffer, "OMEMO (legacy): AES-128-GCM payload encryption failed.");
            return nullptr;
        }

        xmpp_stanza_t *encrypted = xmpp_stanza_new(*account->context);
        xmpp_stanza_set_name(encrypted, "encrypted");
        xmpp_stanza_set_ns(encrypted, kLegacyOmemoNs.data());

        xmpp_stanza_t *header = xmpp_stanza_new(*account->context);
        xmpp_stanza_set_name(header, "header");
        xmpp_stanza_set_attribute(header, "sid", fmt::format("{}", device_id).c_str());

        xmpp_string_guard own_bare_g(*account->context,
            xmpp_jid_bare(*account->context, account->jid().data()));
        const std::string own_jid = own_bare_g ? own_bare_g.str() : std::string(account->jid());

        bool added_any_key = false;

        auto add_legacy_keys = [&](const std::string &recipient_jid,
                                   const std::string &device_list_str,
                                   int *out_incomplete_count = nullptr) -> bool
        {
            bool added_keys = false;
            if (out_incomplete_count)
                *out_incomplete_count = 0;

            for (const auto &dev : split(device_list_str, ';'))
            {
                const auto remote_device_id = parse_uint32(dev);
                if (!remote_device_id || !is_valid_omemo_device_id(*remote_device_id))
                {
                    print_error(buffer, fmt::format(
                        "OMEMO (legacy): skipping invalid device id '{}' for {}", dev, recipient_jid));
                    continue;
                }

                if (own_jid == recipient_jid && *remote_device_id == device_id)
                {
                    continue;
                }

                if (failed_session_bootstrap.count({recipient_jid, *remote_device_id}) > 0)
                {
                    continue;
                }

                if (!has_session(recipient_jid.c_str(), *remote_device_id))
                {
                    if (!establish_session_from_bundle(*this, *account->context, recipient_jid, *remote_device_id))
                    {
                        request_legacy_bundle(*account, recipient_jid, *remote_device_id);
                        if (out_incomplete_count)
                            ++*out_incomplete_count;
                        continue;
                    }
                }

                const auto transport = encrypt_legacy_transport_key(*this, recipient_jid, *remote_device_id, *ep);
                if (!transport)
                {
                    if (out_incomplete_count)
                        ++*out_incomplete_count;
                    print_info(buffer, fmt::format(
                        "OMEMO (legacy): failed to encrypt transport key for {}/{}",
                        recipient_jid, *remote_device_id));
                    continue;
                }

                const auto encoded_transport = base64_encode(*account->context,
                                                             transport->first.data(),
                                                             transport->first.size());

                xmpp_stanza_t *key_stanza = xmpp_stanza_new(*account->context);
                xmpp_stanza_set_name(key_stanza, "key");
                xmpp_stanza_set_attribute(key_stanza, "rid", fmt::format("{}", *remote_device_id).c_str());
                if (transport->second)
                    xmpp_stanza_set_attribute(key_stanza, "prekey", "true");

                xmpp_stanza_t *key_text = xmpp_stanza_new(*account->context);
                xmpp_stanza_set_text(key_text, encoded_transport.c_str());
                xmpp_stanza_add_child(key_stanza, key_text);
                xmpp_stanza_release(key_text);

                xmpp_stanza_add_child(header, key_stanza);
                xmpp_stanza_release(key_stanza);
                added_keys = true;
            }

            return added_keys;
        };

        int incomplete_recipient_count = 0;
        added_any_key = add_legacy_keys(target_jid, *devicelist, &incomplete_recipient_count);

        if (incomplete_recipient_count > 0)
        {
            xmpp_stanza_release(header);
            xmpp_stanza_release(encrypted);
            return nullptr;
        }

        if (own_jid != target_jid)
        {
            const auto own_legacy_devicelist = load_string(*this, key_for_legacy_devicelist(own_jid));
            if (own_legacy_devicelist && !own_legacy_devicelist->empty())
            {
                added_any_key = add_legacy_keys(own_jid, *own_legacy_devicelist) || added_any_key;
            }
            else
            {
                // Interop fallback: if legacy own-devicelist cache is empty,
                // reuse OMEMO:2 own device ids for sender-side sync coverage.
                const auto own_omemo2_devicelist = load_string(*this, key_for_devicelist(own_jid));
                if (own_omemo2_devicelist && !own_omemo2_devicelist->empty())
                {
                    added_any_key = add_legacy_keys(own_jid, *own_omemo2_devicelist) || added_any_key;
                }
            }
        }

        if (!added_any_key)
        {
            xmpp_stanza_release(header);
            xmpp_stanza_release(encrypted);
            print_error(buffer, fmt::format(
                "OMEMO (legacy): could not encrypt for any known device of {}.", target_jid));
            return nullptr;
        }

        // Legacy: include <iv>base64</iv> in the header (12-byte GCM nonce)
        const auto encoded_iv = base64_encode(*account->context, ep->iv.data(), ep->iv.size());
        xmpp_stanza_t *iv_stanza = xmpp_stanza_new(*account->context);
        xmpp_stanza_set_name(iv_stanza, "iv");
        xmpp_stanza_t *iv_text = xmpp_stanza_new(*account->context);
        xmpp_stanza_set_text(iv_text, encoded_iv.c_str());
        xmpp_stanza_add_child(iv_stanza, iv_text);
        xmpp_stanza_release(iv_text);
        xmpp_stanza_add_child(header, iv_stanza);
        xmpp_stanza_release(iv_stanza);

        xmpp_stanza_add_child(encrypted, header);
        xmpp_stanza_release(header);

        // Legacy: <payload>base64 AES-128-GCM ciphertext (auth tag stripped)</payload>
        const auto encoded_payload = base64_encode(*account->context,
                                                   ep->payload.data(),
                                                   ep->payload.size());
        xmpp_stanza_t *payload = xmpp_stanza_new(*account->context);
        xmpp_stanza_set_name(payload, "payload");
        xmpp_stanza_t *payload_text = xmpp_stanza_new(*account->context);
        xmpp_stanza_set_text(payload_text, encoded_payload.c_str());
        xmpp_stanza_add_child(payload, payload_text);
        xmpp_stanza_release(payload_text);
        xmpp_stanza_add_child(encrypted, payload);
        xmpp_stanza_release(payload);

        return encrypted;
    }

