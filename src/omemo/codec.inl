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

    print_info(buffer, fmt::format("OMEMO decode: sender {} device {} payload-bytes={}",
                                   jid, *sender_device_id, payload.size()));

    xmpp_string_guard own_bare_g(*account->context,
                                 xmpp_jid_bare(*account->context, account->jid().data()));
    const std::string own_bare_jid = own_bare_g ? own_bare_g.str() : std::string(account->jid());

    std::optional<std::pair<std::array<std::uint8_t, 32>, std::array<std::uint8_t, 16>>> transport_key;
    std::optional<std::uint32_t> used_prekey_id;
    bool found_keys_elem = false;
    bool found_keys_for_our_bare_jid = false;
    bool found_key_for_us = false;
    for (xmpp_stanza_t *child = xmpp_stanza_get_children(header);
         child && !transport_key;
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
            continue;

        if (std::string_view {keys_jid}.find('/') != std::string_view::npos)
        {
            print_error(buffer, fmt::format(
                "OMEMO message has non-bare keys jid '{}'; ignoring non-compliant element.",
                keys_jid));
            continue;
        }

        if (own_bare_jid != keys_jid)
            continue;

        found_keys_for_our_bare_jid = true;

        for (xmpp_stanza_t *key_stanza = xmpp_stanza_get_children(child);
             key_stanza && !transport_key;
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

            transport_key = decrypt_transport_key(*this, jid, *sender_device_id, serialized, is_prekey,
                                                  is_prekey ? &used_prekey_id : nullptr);
            if (!transport_key)
                print_error(buffer, "OMEMO Signal decryption of transport key failed.");
        }
    }

    if (!transport_key)
    {
        if (!found_keys_elem)
            print_error(buffer, "OMEMO message has no <keys> element in header.");
        else if (!found_keys_for_our_bare_jid)
            print_error(buffer, fmt::format(
                "OMEMO message has no <keys jid='{}'> element for our bare JID.",
                own_bare_jid));
        else if (!found_key_for_us)
        {
            print_error(buffer, fmt::format(
                "OMEMO message has no key for our device {} (sender did not encrypt for us).",
                device_id));
            // The sender does not (yet) know our device_id.  Request their bundle
            // so we can establish a session from our side, then send a
            // KeyTransportElement that will teach them to include us in future
            // messages.  pending_key_transport is checked in handle_bundle().
            if (account && sender_device_id)
            {
                const auto key = std::make_pair(std::string{jid}, *sender_device_id);
                if (!pending_key_transport.count(key))
                {
                    pending_key_transport.insert(key);
                    request_bundle(*account, jid, *sender_device_id);
                    print_info(buffer, fmt::format(
                        "OMEMO: requested bundle for {}/{} to establish session "
                        "and send key-transport.",
                        jid, *sender_device_id));
                }
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

    const auto decrypted_xml = omemo2_decrypt(transport_key->first, transport_key->second, payload);
    if (!decrypted_xml)
    {
        print_error(buffer, "OMEMO payload decryption failed.");
        return nullptr;
    }

    print_info(buffer, fmt::format("OMEMO decode: decrypted SCE payload size={}",
                                   decrypted_xml->size()));

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
    
    print_info(buffer, fmt::format(
        "OMEMO: found cached device list for {} (len={})",
        target_jid, devicelist->size()));

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
                               int &out_count) -> xmpp_stanza_t *
    {
        xmpp_stanza_t *keys = xmpp_stanza_new(*account->context);
        xmpp_stanza_set_name(keys, "keys");
        xmpp_stanza_set_attribute(keys, "jid", target_jid);
        out_count = 0;

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
                print_info(buffer, fmt::format(
                    "OMEMO: skipping own device {} (local-only)", *remote_device_id));
                continue;
            }

            if (!has_session(target_jid, *remote_device_id))
            {
                if (!establish_session_from_bundle(*this, *account->context, target_jid, *remote_device_id))
                {
                    request_bundle(*account, target_jid, *remote_device_id);
                    print_info(buffer, fmt::format(
                        "OMEMO has no usable bundle/session yet for {}/{}; requested bundle fetch.",
                        target_jid, *remote_device_id));
                    continue;
                }
                print_info(buffer, fmt::format(
                    "OMEMO: established session from bundle for {}/{}", target_jid, *remote_device_id));
            }
            else
            {
                print_info(buffer, fmt::format(
                    "OMEMO: using existing session for {}/{}", target_jid, *remote_device_id));
            }

            const auto transport = encrypt_transport_key(*this, target_jid, *remote_device_id, *encrypted_payload);
            if (!transport)
            {
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
        const auto recipient_devices_str = *devicelist;
        const auto device_list = split(recipient_devices_str, ';');
        print_info(buffer, fmt::format(
            "OMEMO encode for {}: devicelist has {} devices ('{}')",
            target_jid, device_list.size(), recipient_devices_str.empty() ? "(empty)" : recipient_devices_str.substr(0, 50)));
        
        xmpp_stanza_t *keys = build_keys_elem(target_jid.c_str(), *devicelist, count);
        if (keys)
        {
            xmpp_stanza_add_child(header, keys);
            xmpp_stanza_release(keys);
            added_any_key = true;
            print_info(buffer, fmt::format(
                "OMEMO encode: added {} recipient device key(s) to header",
                count));
        }
        else
        {
            print_error(buffer, fmt::format(
                "OMEMO encode: failed to build keys for recipient (no valid sessions)"));
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
                    print_info(buffer, fmt::format(
                        "OMEMO encode: added {} own device key(s) to header",
                        count));
                }
            }
            else
            {
                print_info(buffer, fmt::format(
                    "OMEMO: own device list empty/missing (own_jid={})",
                    own_jid));
            }
        }
        else
        {
            print_info(buffer, fmt::format(
                "OMEMO: encrypting to own JID, skipping own-device keys"));
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

