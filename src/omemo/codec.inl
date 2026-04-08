XMPP_TEST_EXPORT bool weechat::xmpp::omemo::has_session(const char *jid, std::uint32_t remote_device_id)
{
    OMEMO_ASSERT(jid != nullptr, "session lookup requires a non-null jid");

    if (!db_env || !jid)
        return false;

    auto address = make_signal_address(jid, static_cast<std::int32_t>(remote_device_id));
    return signal_protocol_session_contains_session(store_context, &address.address) == 1;
}

std::optional<std::string> weechat::xmpp::omemo::decode(weechat::account *account,
                                   struct t_gui_buffer *buffer,
                                   const char *jid,
                                   xmpp_stanza_t *encrypted,
                                   bool quiet)
{
    OMEMO_ASSERT(account != nullptr, "OMEMO decode requires a valid account");
    OMEMO_ASSERT(jid != nullptr, "OMEMO decode requires a peer jid");
    OMEMO_ASSERT(encrypted != nullptr, "OMEMO decode requires an encrypted stanza");

    if (!account || !jid || !encrypted)
    {
        print_error(buffer, "OMEMO decode received invalid input.");
        return std::nullopt;
    }

    xmpp_stanza_t *header = xmpp_stanza_get_child_by_name(encrypted, "header");
    xmpp_stanza_t *payload_stanza = xmpp_stanza_get_child_by_name(encrypted, "payload");
    if (!header)
    {
        print_error(buffer, "OMEMO message is missing header.");
        return std::nullopt;
    }

    const auto sender_device_id = parse_uint32(
        xmpp_stanza_get_attribute(header, "sid")
            ? xmpp_stanza_get_attribute(header, "sid")
            : "");
    if (!sender_device_id)
    {
        print_error(buffer, "OMEMO message header is missing a valid sender sid.");
        return std::nullopt;
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
        return std::nullopt;
    }

    const std::string own_bare_jid = ::jid(nullptr, account->jid().data()).bare;

    // Detect legacy (eu.siacs.conversations.axolotl) format.
    // Primary signal: xmlns on <encrypted>; secondary fallback: <iv> in header.
    {
        const char *enc_ns = xmpp_stanza_get_ns(encrypted);
        if (enc_ns)
            XDEBUG("OMEMO decode: <encrypted> xmlns='{}'", enc_ns);
    }
    const char *enc_ns = xmpp_stanza_get_ns(encrypted);
    xmpp_stanza_t *iv_stanza = xmpp_stanza_get_child_by_name(header, "iv");
    const bool is_axolotl_format = (enc_ns != nullptr
        && std::string_view {enc_ns} == kLegacyOmemoNs)
        || (enc_ns == nullptr && iv_stanza != nullptr);

    // Only accept axolotl-format messages (we no longer support OMEMO:2 decoding).
    if (!is_axolotl_format)
    {
        if (!quiet)
            print_error(buffer, "OMEMO: received non-axolotl (OMEMO:2) encrypted message; ignoring.");
        return std::nullopt;
    }

    // Receiving an OMEMO message from a peer counts as observed traffic for
    // that peer — this unblocks bundle requests (which check has_peer_traffic)
    // so the recovery path can fetch their bundle and send a key-transport when
    // decryption fails due to a stale/mismatched session.
    note_peer_traffic(account->context, jid);

    // Legacy transport key: {innerKey16, authTag16}
    std::optional<std::pair<std::array<std::uint8_t, 16>, std::array<std::uint8_t, 16>>> legacy_transport_key;
    std::optional<std::uint32_t> used_prekey_id;
    std::optional<std::uint32_t> ratchet_message_counter;
    bool decoded_as_prekey = false; // true if the decrypted key element had kex/prekey=true
    bool found_keys_elem = false;
    bool found_keys_for_our_bare_jid = false;
    bool found_key_for_us = false;
    for (xmpp_stanza_t *child = xmpp_stanza_get_children(header);
         child && !legacy_transport_key;
         child = xmpp_stanza_get_next(child))
    {
        const char *name = xmpp_stanza_get_name(child);
        if (!name || weechat_strcasecmp(name, "keys") != 0)
            continue;
        found_keys_elem = true;

        const char *keys_jid = xmpp_stanza_get_attribute(child, "jid");
        XDEBUG("OMEMO decode: <keys jid='{}'> element found", keys_jid ? keys_jid : "(none)");
        if (!keys_jid)
        {
            // Legacy compatibility: OMEMO:1 senders may omit keys@jid.
            XDEBUG("OMEMO decode: accepting legacy <keys> without jid attribute.");
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
             key_stanza && !legacy_transport_key;
             key_stanza = xmpp_stanza_get_next(key_stanza))
        {
            const char *key_name = xmpp_stanza_get_name(key_stanza);
            if (!key_name || weechat_strcasecmp(key_name, "key") != 0)
                continue;

            const char *rid = xmpp_stanza_get_attribute(key_stanza, "rid");
            const auto rid_val = rid ? parse_uint32(rid).value_or(0) : 0;
            XDEBUG("OMEMO decode:   <key rid='{}'> (our device_id={})",
                   rid ? rid : "(null)", device_id);
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
            decoded_as_prekey = is_prekey;
            if (!kex_val && legacy_prekey_val)
            {
                XDEBUG("OMEMO decode: accepting legacy 'prekey' key attribute; strict XEP-0384 uses 'kex'.");
            }
            const auto serialized = base64_decode(*account->context, stanza_text(key_stanza));
            XDEBUG("OMEMO decode:   found key for us: is_prekey={} serialized-bytes={}",
                   is_prekey, serialized.size());
            if (serialized.empty())
            {
                print_error(buffer, "OMEMO key element for our device has empty/invalid base64.");
                continue;
            }

            legacy_transport_key = decrypt_axolotl_transport_key(*this, jid, *sender_device_id, serialized, is_prekey,
                                                                 is_prekey ? &used_prekey_id : nullptr,
                                                                 &ratchet_message_counter);
            if (!legacy_transport_key && !quiet)
                print_error(buffer, "OMEMO (legacy) Signal decryption of transport key failed.");
        }
    }

    // Legacy compatibility: some OMEMO:1 payloads place <key/> elements
    // directly under <header> instead of wrapping them in <keys/>.
    if (!legacy_transport_key && !found_keys_elem)
    {
        for (xmpp_stanza_t *key_stanza = xmpp_stanza_get_children(header);
             key_stanza && !legacy_transport_key;
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
            decoded_as_prekey = is_prekey;

            const auto serialized = base64_decode(*account->context, stanza_text(key_stanza));
            if (serialized.empty())
                continue;

            legacy_transport_key = decrypt_axolotl_transport_key(*this, jid, *sender_device_id, serialized, is_prekey,
                                                                 is_prekey ? &used_prekey_id : nullptr,
                                                                 &ratchet_message_counter);
        }
    }

    if (!legacy_transport_key)
    {
        if (!quiet)
        {
            if (!found_keys_elem)
                print_error(buffer, "OMEMO message has no <keys> element in header.");
            else if (!found_keys_for_our_bare_jid)
                print_error(buffer, fmt::format(
                    "OMEMO message has no <keys jid='{}'> element for our bare JID.",
                    own_bare_jid));
        }
        if (!found_key_for_us)
        {
            // The sender does not (yet) know our device_id. Request their bundle
            // once per peer device and avoid repeating the same error every message.
            // During MAM replay (quiet==true) we suppress both the error print and
            // the bundle fetch.
            if (!quiet && account && sender_device_id)
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
                    request_axolotl_bundle(*account, bare_jid, *sender_device_id);
                }
            }
            else if (!quiet)
            {
                print_error(buffer, fmt::format(
                    "OMEMO message has no key for our device {} (sender did not encrypt for us).",
                    device_id));
            }
        }
        else
        {
            // found_key_for_us==true but decryption failed.
            if (!quiet)
                print_error(buffer, "OMEMO transport key decryption failed.");
            if (!quiet && account && sender_device_id)
            {
                const std::string bare_jid = normalize_bare_jid(*account->context, jid);
                // Self-outbound copy: do not corrupt own session.
                if (bare_jid == own_bare_jid)
                    return std::nullopt;
                const auto kex_key = std::make_pair(bare_jid, *sender_device_id);
                if (key_transport_bootstrap_attempted.count(kex_key) == 0)
                {
                    key_transport_bootstrap_attempted.insert(kex_key);
                    print_info(buffer, fmt::format(
                        "OMEMO: queueing key-transport to {}/{} to recover stale session",
                        bare_jid, *sender_device_id));
                    // Delete the stale session so the key-transport forces a fresh
                    // PreKeySignalMessage exchange from the sender's side.
                    auto addr = make_signal_address(bare_jid,
                                                    static_cast<std::int32_t>(*sender_device_id));
                    signal_protocol_session_delete_session(store_context, &addr.address);
                    pending_key_transport.insert(kex_key);
                    request_axolotl_bundle(*account, bare_jid, *sender_device_id);
                }
            }
        }
        return std::nullopt;
    }

    // Key-transport element: no payload, nothing more to decrypt.
    // The session is now established from our side.
    if (!payload_stanza || payload.empty())
    {
        print_info(buffer, "OMEMO: received KeyTransportElement — session established.");
        return std::nullopt;
    }

    // Legacy format: AES-128-GCM decrypt path (no SCE wrapping)
    const auto iv_text = stanza_text(iv_stanza);
    const auto iv_vec = base64_decode(*account->context, iv_text);
    if (iv_vec.size() != 12)
    {
        print_error(buffer, "OMEMO (legacy): IV element has wrong size (expected 12 bytes).");
        return std::nullopt;
    }
    std::array<std::uint8_t, 12> iv {};
    std::copy_n(iv_vec.begin(), 12, iv.begin());
    const auto result = axolotl_omemo_decrypt(legacy_transport_key->first, iv,
                                             legacy_transport_key->second, payload);
    if (!result)
    {
        print_error(buffer, "OMEMO (legacy) payload decryption failed.");
        return std::nullopt;
    }
    if (used_prekey_id && account)
    {
            if (replace_used_prekey(*this, *account->context, *used_prekey_id))
            {
                if (global_mam_catchup)
                {
                    bundle_republish_pending = true;
                    XDEBUG("omemo: consumed pre-key {} — bundle republish deferred (MAM catchup active)",
                           *used_prekey_id);
                }
                else
                {
                    print_info(buffer, fmt::format(
                        "OMEMO: replaced consumed pre-key {} — republishing bundle",
                        *used_prekey_id));
                    if (std::shared_ptr<xmpp_stanza_t> lbs { get_axolotl_bundle(*account->context, nullptr, nullptr), xmpp_stanza_release })
                        account->connection.send(lbs.get());
                }
            }
    }
    // XEP-0384 §6: MUST send a heartbeat when counter >= 53 (first time per ratchet key).
    {
        const std::string bare_jid = normalize_bare_jid(*account->context, jid);
        const auto hb_key = std::make_pair(bare_jid, *sender_device_id);
        if (decoded_as_prekey)
            heartbeat_sent.erase(hb_key);
        if (ratchet_message_counter && *ratchet_message_counter >= 53
            && !global_mam_catchup
            && heartbeat_sent.count(hb_key) == 0)
        {
            heartbeat_sent.insert(hb_key);
            XDEBUG("omemo: sending heartbeat to {}/{} (counter={})",
                   bare_jid, *sender_device_id, *ratchet_message_counter);
            send_key_transport(*this, *account, buffer, bare_jid.c_str(), *sender_device_id);
        }
    }
    // XEP-0384 §6 SHOULD: after successfully decrypting a PreKeySignalMessage,
    // send back an empty OMEMO message so the sender knows it can stop sending OMEMOKeyExchanges.
    if (decoded_as_prekey && !global_mam_catchup && account && sender_device_id)
    {
        const std::string bare_jid = normalize_bare_jid(*account->context, jid);
        const auto pk_key = std::make_pair(bare_jid, *sender_device_id);
        if (prekey_reply_sent.count(pk_key) == 0)
        {
            prekey_reply_sent.insert(pk_key);
            XDEBUG("omemo (legacy): sending prekey-reply key-transport to {}/{}",
                   bare_jid, *sender_device_id);
            send_key_transport(*this, *account, buffer, bare_jid.c_str(), *sender_device_id);
        }
    }
    return std::string(*result);
}

xmpp_stanza_t *weechat::xmpp::omemo::encode(weechat::account *account,
                                            struct t_gui_buffer *buffer,
                                            const char *jid,
                                            const char *unencrypted)
{
    OMEMO_ASSERT(account != nullptr, "OMEMO encode requires a valid account");
    OMEMO_ASSERT(jid != nullptr, "OMEMO encode requires a peer jid");
    OMEMO_ASSERT(unencrypted != nullptr, "OMEMO encode requires plaintext input");

    if (!*this || !account || !jid || !unencrypted)
        return nullptr;

    ensure_local_identity(*this);
    ensure_registration_id(*this);
    if (ensure_prekeys(*this, *account->context))
    {
        // New prekeys were just generated (or repaired) — republish so peers
        // fetch the updated material instead of using a stale cached bundle.
        print_info(buffer, "OMEMO: prekeys regenerated — republishing bundle");
        if (std::shared_ptr<xmpp_stanza_t> lbs { get_axolotl_bundle(*account->context, nullptr, nullptr), xmpp_stanza_release })
            account->connection.send(lbs.get());
    }

    std::string target_jid = ::jid(nullptr, jid).bare;
    if (target_jid.empty())
        target_jid = jid;

    note_peer_traffic(account->context, target_jid);

    // Look up device list — stored under legacy key set by the legacy devicelist handler
    const auto devicelist = load_string(*this, key_for_axolotl_devicelist(target_jid));
    if (!devicelist || devicelist->empty())
    {
        request_axolotl_devicelist(*account, target_jid);
        print_error(buffer, fmt::format(
            "OMEMO: no device list cached for {}. Requested.", target_jid));
        return nullptr;
    }

    // Legacy OMEMO uses AES-128-GCM on the raw message text (no SCE wrapping)
    const auto ep = axolotl_omemo_encrypt(std::string_view(unencrypted));
    if (!ep)
    {
        print_error(buffer, "OMEMO: AES-128-GCM payload encryption failed.");
        return nullptr;
    }

    const std::string own_jid = [&]{
        auto b = ::jid(nullptr, account->jid().data()).bare;
        return b.empty() ? std::string(account->jid()) : b;
    }();

    bool added_any_key = false;

    stanza::xep0384::axolotl_header header_spec(fmt::format("{}", device_id));

    auto add_axolotl_keys = [&](const std::string &recipient_jid,
                               const std::string &device_list_str,
                               int *out_incomplete_count = nullptr,
                               bool include_own_device = false) -> bool
    {
        // Legacy flat layout: emit <key rid='…'> directly under <header>
        // (no <keys jid='…'> wrapper) for Conversations/Gajim compatibility.
        bool added_keys = false;
        if (out_incomplete_count)
            *out_incomplete_count = 0;

        for (const auto &dev : split(device_list_str, ';'))
        {
            const auto remote_device_id = parse_uint32(dev);
            if (!remote_device_id || !is_valid_omemo_device_id(*remote_device_id))
            {
                print_error(buffer, fmt::format(
                    "OMEMO: skipping invalid device id '{}' for {}", dev, recipient_jid));
                continue;
            }

            if (own_jid == recipient_jid && *remote_device_id == device_id && !include_own_device)
                continue;

            // BTBV trust gate: skip devices that are UNTRUSTED or UNDECIDED
            {
                const auto trust = load_tofu_trust(*this, recipient_jid, *remote_device_id);
                if (trust == omemo_trust::UNTRUSTED || trust == omemo_trust::UNDECIDED)
                {
                    XDEBUG("omemo encode: skipping device {}/{} (trust={})",
                           recipient_jid, *remote_device_id,
                           trust ? static_cast<int>(*trust) : -1);
                    continue;
                }
            }

            if (failed_session_bootstrap.count({recipient_jid, *remote_device_id}) > 0)
                continue;

            if (!has_session(recipient_jid.c_str(), *remote_device_id))
            {
                if (!establish_session_from_bundle(*this, *account->context, recipient_jid, *remote_device_id))
                {
                    request_axolotl_bundle(*account, recipient_jid, *remote_device_id);
                    if (out_incomplete_count)
                        ++*out_incomplete_count;
                    continue;
                }
            }

            const auto transport = encrypt_axolotl_transport_key(*this, recipient_jid, *remote_device_id, *ep);
            if (!transport)
            {
                if (out_incomplete_count)
                    ++*out_incomplete_count;
                print_info(buffer, fmt::format(
                    "OMEMO: failed to encrypt transport key for {}/{} — purging stale session",
                    recipient_jid, *remote_device_id));
                auto stale_addr = make_signal_address(
                    recipient_jid, static_cast<std::int32_t>(*remote_device_id));
                signal_protocol_session_delete_session(store_context, &stale_addr.address);
                request_axolotl_bundle(*account, recipient_jid, *remote_device_id);
                continue;
            }

            const auto encoded_transport = base64_encode(*account->context,
                                                         transport->first.data(),
                                                         transport->first.size());
            // Flat legacy layout: add <key> directly under <header>
            header_spec.add_key(stanza::xep0384::axolotl_key(
                fmt::format("{}", *remote_device_id),
                encoded_transport,
                transport->second));
            added_keys = true;
        }

        return added_keys;
    };

    int incomplete_recipient_count = 0;
    added_any_key = add_axolotl_keys(target_jid, *devicelist, &incomplete_recipient_count);

    if (incomplete_recipient_count > 0)
        return nullptr;

    // Encrypt for own devices (carbon copy sync + self-message support).
    {
        const auto own_legacy_devicelist = load_string(*this, key_for_axolotl_devicelist(own_jid));
        if (own_legacy_devicelist && !own_legacy_devicelist->empty())
        {
            added_any_key = add_axolotl_keys(own_jid, *own_legacy_devicelist,
                                             nullptr, /*include_own_device=*/true) || added_any_key;
        }
    }

    if (!added_any_key)
    {
        print_error(buffer, fmt::format(
            "OMEMO: could not encrypt for any known device of {}.", target_jid));
        return nullptr;
    }

    // Legacy: include <iv>base64</iv> in the header (12-byte GCM nonce)
    const auto encoded_iv = base64_encode(*account->context, ep->iv.data(), ep->iv.size());
    header_spec.add_iv(stanza::xep0384::axolotl_iv(encoded_iv));

    // Legacy: <payload>base64 AES-128-GCM ciphertext (auth tag stripped)</payload>
    const auto encoded_payload = base64_encode(*account->context,
                                               ep->payload.data(),
                                               ep->payload.size());

    stanza::xep0384::axolotl_encrypted enc_spec;
    enc_spec.add_header(header_spec)
            .add_payload(stanza::xep0384::axolotl_payload(encoded_payload));

    auto sp = enc_spec.build(*account->context);
    xmpp_stanza_clone(sp.get());  // bump refcount; shared_ptr dtor will release its ref
    return sp.get();              // caller owns one ref; must call xmpp_stanza_release()
}
