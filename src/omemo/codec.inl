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
    // Using the namespace avoids misrouting adversarially crafted OMEMO:2 stanzas
    // that happen to contain a rogue <iv> child.
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

    // Receiving an OMEMO message from a peer counts as observed traffic for
    // that peer — this unblocks bundle requests (which check has_peer_traffic)
    // so the recovery path can fetch their bundle and send a key-transport when
    // decryption fails due to a stale/mismatched session.
    note_peer_traffic(account->context, jid);

    store_device_mode(*this,
                      normalize_bare_jid(*account->context, jid),
                      *sender_device_id,
                      is_axolotl_format ? peer_mode::axolotl : peer_mode::omemo2);

    // OMEMO:2 transport key: {key32, hmac16}
    std::optional<std::pair<std::array<std::uint8_t, 32>, std::array<std::uint8_t, 16>>> transport_key;
    // Legacy transport key: {innerKey16, authTag16}
    std::optional<std::pair<std::array<std::uint8_t, 16>, std::array<std::uint8_t, 16>>> legacy_transport_key;
    std::optional<std::uint32_t> used_prekey_id;
    std::optional<std::uint32_t> ratchet_message_counter;
    bool decoded_as_prekey = false; // true if the decrypted key element had kex/prekey=true
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
             key_stanza && !transport_key && !legacy_transport_key;
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

            if (is_axolotl_format)
            {
                legacy_transport_key = decrypt_axolotl_transport_key(*this, jid, *sender_device_id, serialized, is_prekey,
                                                                     is_prekey ? &used_prekey_id : nullptr,
                                                                     &ratchet_message_counter);
                if (!legacy_transport_key && !quiet)
                    print_error(buffer, "OMEMO (legacy) Signal decryption of transport key failed.");
            }
            else
            {
                transport_key = decrypt_transport_key(*this, jid, *sender_device_id, serialized, is_prekey,
                                                      is_prekey ? &used_prekey_id : nullptr,
                                                      &ratchet_message_counter);
                if (!transport_key && !quiet)
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
            decoded_as_prekey = is_prekey;

            const auto serialized = base64_decode(*account->context, stanza_text(key_stanza));
            if (serialized.empty())
                continue;

            if (is_axolotl_format)
            {
                legacy_transport_key = decrypt_axolotl_transport_key(*this, jid, *sender_device_id, serialized, is_prekey,
                                                                     is_prekey ? &used_prekey_id : nullptr,
                                                                     &ratchet_message_counter);
            }
            else
            {
                transport_key = decrypt_transport_key(*this, jid, *sender_device_id, serialized, is_prekey,
                                                      is_prekey ? &used_prekey_id : nullptr,
                                                      &ratchet_message_counter);
            }
        }
    }

    if (!transport_key && !legacy_transport_key)
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
            // the bundle fetch: the sender simply did not include our device at that
            // point in time.  Issuing a bundle request here would queue a
            // pending_key_transport that — when the bundle response arrives — calls
            // establish_session_from_bundle and overwrites the existing session with
            // a fresh one, making all subsequent MAM messages (encrypted under the
            // old session) undecryptable.
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
                    const auto selected_mode = resolve_device_mode(
                        *this,
                        *account,
                        bare_jid,
                        *sender_device_id,
                        is_axolotl_format ? peer_mode::axolotl : peer_mode::unknown);

                    if (selected_mode == peer_mode::axolotl)
                        request_axolotl_bundle(*account, bare_jid, *sender_device_id);
                    else
                        request_bundle(*account, bare_jid, *sender_device_id);
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
            // found_key_for_us==true but decryption failed.  This happens when the
            // Signal ratchet has no usable state for the received message — most
            // commonly a MAM replay of a prekey message whose prekey was already
            // consumed when the session was first established.
            //
            // Only trigger stale-session recovery for live (non-MAM) messages.
            // During MAM replay (quiet==true) decryption failures are expected for
            // old messages that used a now-consumed prekey or an old ratchet state.
            // Triggering recovery there would delete a perfectly good existing session
            // and cause all subsequent messages — including live ones — to fail.
            if (!quiet)
                print_error(buffer, "OMEMO transport key decryption failed.");
            if (!quiet && account && sender_device_id)
            {
                const std::string bare_jid = normalize_bare_jid(*account->context, jid);
                // Self-outbound copy: the key was encrypted for our own device by
                // ourselves (self-copy mechanism, XEP-0384 §7.2). Decryption fails
                // on MAM replay because the ratchet has moved forward — this is
                // expected and harmless. Do NOT delete the self-session or queue a
                // key-transport to ourselves: that would corrupt our own Signal state
                // and trigger nonsensical IQ bundle requests to our own device.
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
                    // Queue a key-transport after re-establishing the outbound session.
                    pending_key_transport.insert(kex_key);
                    const auto selected_mode = resolve_device_mode(
                        *this, *account, bare_jid, *sender_device_id,
                        is_axolotl_format ? peer_mode::axolotl : peer_mode::unknown);
                    if (selected_mode == peer_mode::axolotl)
                        request_axolotl_bundle(*account, bare_jid, *sender_device_id);
                    else
                        request_bundle(*account, bare_jid, *sender_device_id);
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
    if (is_axolotl_format && legacy_transport_key)
    {
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
                        // Defer the republish until MAM catchup finishes.
                        // Many stale prekey messages may arrive in a row; the
                        // flag collapses all of them into a single IQ pair sent
                        // at the MAM <fin> by process_postponed_bundle_republish().
                        bundle_republish_pending = true;
                        XDEBUG("omemo: consumed pre-key {} — bundle republish deferred (MAM catchup active)",
                               *used_prekey_id);
                    }
                    else
                    {
                        print_info(buffer, fmt::format(
                            "OMEMO: replaced consumed pre-key {} — republishing bundle",
                            *used_prekey_id));
                        if (std::shared_ptr<xmpp_stanza_t> bs { get_bundle(*account->context, nullptr, nullptr), xmpp_stanza_release })
                            account->connection.send(bs.get());

                        if (std::shared_ptr<xmpp_stanza_t> lbs { get_axolotl_bundle(*account->context, nullptr, nullptr), xmpp_stanza_release })
                            account->connection.send(lbs.get());
                    }
                }
        }
        // XEP-0384 §6: MUST send a heartbeat when counter >= 53 (first time per ratchet key).
        // On a new session (kex/prekey message), reset the heartbeat guard so we re-arm
        // for the next ratchet stretch.
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
        // XEP-0384 §6 SHOULD: after successfully decrypting a PreKeySignalMessage
        // (i.e. the first message of a new session from this device), send back an
        // empty OMEMO message so the sender knows it can stop sending OMEMOKeyExchanges.
        // Guarded against MAM replay and duplicate sends (one reply per fresh session).
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

    const auto decrypted_xml = omemo2_decrypt(transport_key->first, transport_key->second, payload);
    if (!decrypted_xml)
    {
        print_error(buffer, "OMEMO payload decryption failed.");
        return std::nullopt;
    }

    // Attempt SCE (XEP-0420) unwrap first; fall back to treating the decrypted
    // bytes as plain UTF-8 text (e.g. older XEP-0384 drafts or non-SCE senders).
    const auto body = sce_unwrap(*account->context, *decrypted_xml);
    if (!body)
    {
        // XEP-0384 §5.7: check for <opt-out xmlns='urn:xmpp:omemo:2'/> inside
        // the SCE <content> element.  This element replaces <body> so sce_unwrap
        // above correctly returns nullopt; we handle it here before falling back.
        const auto opt_out_reason = sce_detect_opt_out(*account->context, *decrypted_xml);
        if (opt_out_reason)
        {
            const std::string bare_jid = normalize_bare_jid(*account->context, jid);
            // Record that this peer has opted out; outgoing messages to them are blocked
            // until the user explicitly acknowledges the switch to plaintext.
            omemo_opted_out_peers.insert(bare_jid);

            if (opt_out_reason->empty())
            {
                print_info(buffer, fmt::format(
                    "OMEMO: {} has requested to switch to plaintext (opt-out). "
                    "Outgoing OMEMO messages to this contact are now blocked "
                    "until you confirm with /omemo optout-ack {}",
                    bare_jid, bare_jid));
            }
            else
            {
                print_info(buffer, fmt::format(
                    "OMEMO: {} has requested to switch to plaintext (opt-out). "
                    "Reason: {}. "
                    "Outgoing OMEMO messages to this contact are now blocked "
                    "until you confirm with /omemo optout-ack {}",
                    bare_jid, *opt_out_reason, bare_jid));
            }
            return std::nullopt;
        }

        // Fallback: return the raw decrypted text directly.
        if (!decrypted_xml->empty())
            return std::string(*decrypted_xml);
        print_error(buffer, "OMEMO SCE envelope unwrap failed and payload is empty.");
        return std::nullopt;
    }

    // Per XEP-0384 §7.3 and Signal protocol best practice: after successfully
    // decrypting a PreKeySignalMessage (first message from a device), replace
    // the consumed pre-key with a fresh one of the same ID and republish the
    // bundle so contacts always have fresh pre-keys available.
    if (used_prekey_id && account)
    {
        if (replace_used_prekey(*this, *account->context, *used_prekey_id))
        {
            if (global_mam_catchup)
            {
                // Defer the republish until MAM catchup finishes.
                // Many stale prekey messages may arrive in a row; the flag
                // collapses all of them into a single IQ pair sent at the
                // MAM <fin> by process_postponed_bundle_republish().
                bundle_republish_pending = true;
                XDEBUG("omemo: consumed pre-key {} — bundle republish deferred (MAM catchup active)",
                       *used_prekey_id);
            }
            else
            {
                print_info(buffer, fmt::format(
                    "OMEMO: replaced consumed pre-key {} — republishing bundle",
                    *used_prekey_id));
                if (std::shared_ptr<xmpp_stanza_t> bs { get_bundle(*account->context, nullptr, nullptr), xmpp_stanza_release })
                    account->connection.send(bs.get());

                if (std::shared_ptr<xmpp_stanza_t> lbs { get_axolotl_bundle(*account->context, nullptr, nullptr), xmpp_stanza_release })
                    account->connection.send(lbs.get());
            }
        }
        else
        {
            print_error(buffer, fmt::format(
                "OMEMO: failed to replace consumed pre-key {} (non-fatal)",
                *used_prekey_id));
        }
    }

    // XEP-0384 §6: MUST send a heartbeat when counter >= 53 (first time per ratchet key).
    // On a new session (kex/prekey message), reset the heartbeat guard so we re-arm
    // for the next ratchet stretch.
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
    // XEP-0384 §6 SHOULD: after successfully decrypting a PreKeySignalMessage
    // (i.e. the first message of a new session from this device), send back an
    // empty OMEMO message so the sender knows it can stop sending OMEMOKeyExchanges.
    // Guarded against MAM replay and duplicate sends (one reply per fresh session).
    if (decoded_as_prekey && !global_mam_catchup && account && sender_device_id)
    {
        const std::string bare_jid = normalize_bare_jid(*account->context, jid);
        const auto pk_key = std::make_pair(bare_jid, *sender_device_id);
        if (prekey_reply_sent.count(pk_key) == 0)
        {
            prekey_reply_sent.insert(pk_key);
            XDEBUG("omemo: sending prekey-reply key-transport to {}/{}",
                   bare_jid, *sender_device_id);
            send_key_transport(*this, *account, buffer, bare_jid.c_str(), *sender_device_id);
        }
    }

    return std::string(*body);
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
    if (ensure_prekeys(*this, *account->context))
    {
        // New prekeys were just generated (or repaired) — the server-side bundle
        // is now stale.  Publish both bundles immediately so peers (e.g.
        // Conversations) will fetch the updated bundle on their next session
        // bootstrap instead of hitting a Bad MAC / DecryptionException.
        print_info(buffer, "OMEMO: prekeys regenerated — republishing bundle");
        if (std::shared_ptr<xmpp_stanza_t> bs { get_bundle(*account->context, nullptr, nullptr), xmpp_stanza_release })
            account->connection.send(bs.get());
        if (std::shared_ptr<xmpp_stanza_t> lbs { get_axolotl_bundle(*account->context, nullptr, nullptr), xmpp_stanza_release })
            account->connection.send(lbs.get());
    }

    // Conversations addresses OMEMO device/session material by bare JID.
    // Normalize once here so devicelist/session lookups don't miss when
    // messages are addressed to a full JID (user@domain/resource).
    std::string target_jid = ::jid(nullptr, jid).bare;
    if (target_jid.empty())
        target_jid = jid;

    note_peer_traffic(account->context, target_jid);

    // XEP-0384 §5.7: if this peer has opted out of OMEMO, block outgoing
    // encrypted messages until the user explicitly acknowledges the switch
    // to plaintext via /omemo optout-ack <jid>.
    if (omemo_opted_out_peers.count(target_jid) > 0)
    {
        print_error(buffer, fmt::format(
            "OMEMO: {} has opted out of OMEMO encryption. "
            "Use /omemo optout-ack {} to acknowledge and unblock, "
            "then switch to plaintext with /plain if desired.",
            target_jid, target_jid));
        return nullptr;
    }

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
    
    // Determine if target is a MUC so SCE adds the mandatory <to/> affix
    // (XEP-0420 §4.3: MUST include <to/> iff message is addressed to a group).
    bool target_is_muc = false;
    {
        auto ch_it = account->channels.find(target_jid);
        if (ch_it != account->channels.end())
            target_is_muc = (ch_it->second.type == weechat::channel::chat_type::MUC);
    }

    const auto sce = sce_wrap(*account->context, *account, unencrypted, target_jid, target_is_muc);
    const auto encrypted_payload = omemo2_encrypt(sce);
    if (!encrypted_payload)
    {
        print_error(buffer, "OMEMO payload encryption failed.");
        return nullptr;
    }

    const std::string own_jid = [&]{
        auto b = ::jid(nullptr, account->jid().data()).bare;
        return b.empty() ? std::string(account->jid()) : b;
    }();

    // Helper: build a stanza::xep0384::keys for a set of device IDs.
    // Returns nullopt if no valid keys could be added.
    // out_count receives the number of keys successfully added.
    // out_incomplete_count (if non-null) receives the count of devices that
    // had no session and couldn't be established synchronously.
    auto build_keys_spec = [&](const char *bks_target_jid,
                               const std::string &device_list_str,
                               int &out_count,
                               int *out_incomplete_count = nullptr,
                               bool include_own_device = false)
        -> std::optional<stanza::xep0384::keys>
    {
        stanza::xep0384::keys ks(bks_target_jid);
        out_count = 0;
        if (out_incomplete_count)
            *out_incomplete_count = 0;

        for (const auto &dev : split(device_list_str, ';'))
        {
            const auto remote_device_id = parse_uint32(dev);
            if (!remote_device_id || !is_valid_omemo_device_id(*remote_device_id))
            {
                print_error(buffer, fmt::format(
                    "OMEMO: skipping invalid device id '{}' for {}", dev, bks_target_jid));
                continue;
            }

            if (own_jid == bks_target_jid && *remote_device_id == device_id && !include_own_device)
                continue;

            if (failed_session_bootstrap.count({bks_target_jid, *remote_device_id}) > 0)
                continue;

            if (!has_session(bks_target_jid, *remote_device_id))
            {
                if (!establish_session_from_bundle(*this, *account->context, bks_target_jid, *remote_device_id))
                {
                    request_bundle(*account, bks_target_jid, *remote_device_id);
                    if (out_incomplete_count)
                        ++*out_incomplete_count;
                    continue;
                }
            }

            const auto transport = encrypt_transport_key(*this, bks_target_jid, *remote_device_id, *encrypted_payload);
            if (!transport)
            {
                if (out_incomplete_count)
                    ++*out_incomplete_count;
                print_info(buffer, fmt::format(
                    "OMEMO failed to encrypt for device {}/{}.", bks_target_jid, *remote_device_id));
                continue;
            }

            const auto encoded_transport = base64_encode(*account->context,
                                                         transport->first.data(),
                                                         transport->first.size());
            ks.add_key(stanza::xep0384::key(
                fmt::format("{}", *remote_device_id),
                encoded_transport,
                transport->second));
            ++out_count;
        }

        if (out_count == 0)
            return std::nullopt;
        return ks;
    };

    stanza::xep0384::header header_spec(fmt::format("{}", device_id));
    bool added_any_key = false;

    // Encrypt for recipient devices
    {
        int count = 0;
        int incomplete_count = 0;
        auto ks = build_keys_spec(target_jid.c_str(), *devicelist, count, &incomplete_count);
        if (ks)
        {
            header_spec.add_keys(*ks);
            added_any_key = true;
        }
        else
        {
            print_error(buffer, fmt::format(
                "OMEMO encode: failed to build keys for recipient (no valid sessions)"));
        }

        if (incomplete_count > 0)
            return nullptr;
    }

    // Encrypt for own devices per XEP-0384 §7.2 so carbon copies can be
    // decrypted on other sessions, and so the sender's own device can decrypt
    // a self-addressed message.
    //
    // When own_jid != target_jid: the first block above already encrypted for
    // recipient devices; now add keys for our other devices (exclude the
    // current device via include_own_device=false — we don't need to encrypt
    // to ourselves since we have the plaintext).  include_own_device=true
    // ensures the current device *is* included when needed (see below).
    //
    // When own_jid == target_jid (self-message): the first block encrypted for
    // all own devices with include_own_device=false, meaning the current device
    // was skipped.  Do a second pass with include_own_device=true so the
    // current device also gets a key (required for self-send decryption and
    // carbon-copy consistency — Profanity always encrypts for all devices
    // including the sender's).
    {
        const auto own_devicelist_omemo2 = load_string(*this, key_for_devicelist(own_jid));
        const auto *own_devicelist_to_use =
            (own_devicelist_omemo2 && !own_devicelist_omemo2->empty())
                ? &*own_devicelist_omemo2
                : nullptr;

        std::optional<std::string> own_devicelist_axolotl;
        if (!own_devicelist_to_use)
        {
            own_devicelist_axolotl = load_string(*this, key_for_axolotl_devicelist(own_jid));
            if (own_devicelist_axolotl && !own_devicelist_axolotl->empty())
                own_devicelist_to_use = &*own_devicelist_axolotl;
        }

        if (own_devicelist_to_use)
        {
            int count = 0;
            int own_incomplete = 0;
            // When sending to self: include_own_device=true so the current
            // device (skipped by the first pass) also gets a key.
            // When sending to another JID: include_own_device=true here too
            // so multi-device sync includes our own current device, matching
            // Profanity's behaviour of encrypting for every device including
            // the sender's active one.
            auto own_ks = build_keys_spec(own_jid.c_str(), *own_devicelist_to_use, count,
                                          &own_incomplete, /*include_own_device=*/true);
            if (own_ks)
            {
                header_spec.add_keys(*own_ks);
                added_any_key = true;
            }
            // If any own-device bundle is still being fetched, defer the send
            // so the message will be retried once the bundle arrives and
            // flush_pending_omemo_messages() is called.  Without this, the
            // message goes out with no <keys jid='own_jid'> block and sibling
            // clients (e.g. Conversations) receiving the carbon copy cannot
            // decrypt it.
            if (own_incomplete > 0)
                return nullptr;
        }
    }

    if (!added_any_key)
    {
        print_error(buffer, fmt::format("OMEMO could not encrypt for any known device of {}.", target_jid));
        return nullptr;
    }

    const auto encoded_payload = base64_encode(*account->context,
                                               encrypted_payload->payload.data(),
                                               encrypted_payload->payload.size());

    stanza::xep0384::encrypted enc_spec;
    enc_spec.add_header(header_spec)
            .add_payload(stanza::xep0384::payload(encoded_payload));

    auto sp = enc_spec.build(*account->context);
    xmpp_stanza_clone(sp.get());  // bump refcount; shared_ptr dtor will release its ref
    return sp.get();              // caller owns one ref; must call xmpp_stanza_release()

}

    xmpp_stanza_t *weechat::xmpp::omemo::encode_axolotl(weechat::account *account,
                                                        struct t_gui_buffer *buffer,
                                                        const char *jid,
                                                        const char *unencrypted)
    {
        OMEMO_ASSERT(account != nullptr, "OMEMO encode_axolotl requires a valid account");
        OMEMO_ASSERT(jid != nullptr, "OMEMO encode_axolotl requires a peer jid");
        OMEMO_ASSERT(unencrypted != nullptr, "OMEMO encode_axolotl requires plaintext input");

        if (!*this || !account || !jid || !unencrypted)
            return nullptr;

        ensure_local_identity(*this);
        ensure_registration_id(*this);
        if (ensure_prekeys(*this, *account->context))
        {
            // New prekeys were just generated (or repaired) — republish both bundles
            // so peers fetch the updated material instead of using a stale cached bundle.
            print_info(buffer, "OMEMO (legacy): prekeys regenerated — republishing bundle");
            if (std::shared_ptr<xmpp_stanza_t> bs { get_bundle(*account->context, nullptr, nullptr), xmpp_stanza_release })
                account->connection.send(bs.get());
            if (std::shared_ptr<xmpp_stanza_t> lbs { get_axolotl_bundle(*account->context, nullptr, nullptr), xmpp_stanza_release })
                account->connection.send(lbs.get());
        }

        std::string target_jid = ::jid(nullptr, jid).bare;
        if (target_jid.empty())
            target_jid = jid;

        note_peer_traffic(account->context, target_jid);

        // XEP-0384 §5.7: block outgoing legacy-OMEMO if peer has opted out.
        if (omemo_opted_out_peers.count(target_jid) > 0)
        {
            print_error(buffer, fmt::format(
                "OMEMO (legacy): {} has opted out of OMEMO encryption. "
                "Use /omemo optout-ack {} to acknowledge and unblock.",
                target_jid, target_jid));
            return nullptr;
        }

        // Look up device list — stored under legacy key set by the legacy devicelist handler
        const auto devicelist = load_string(*this, key_for_axolotl_devicelist(target_jid));
        if (!devicelist || devicelist->empty())
        {
            request_axolotl_devicelist(*account, target_jid);
            print_error(buffer, fmt::format(
                "OMEMO (legacy): no device list cached for {}. Requested.", target_jid));
            return nullptr;
        }

        // Legacy OMEMO uses AES-128-GCM on the raw message text (no SCE wrapping)
        const auto ep = axolotl_omemo_encrypt(std::string_view(unencrypted));
        if (!ep)
        {
            print_error(buffer, "OMEMO (legacy): AES-128-GCM payload encryption failed.");
            return nullptr;
        }

        const std::string own_jid = [&]{
            auto b = ::jid(nullptr, account->jid().data()).bare;
            return b.empty() ? std::string(account->jid()) : b;
        }();

        bool added_any_key = false;

        // Helper: build stanza::xep0384::axolotl_keys for a set of device IDs.
        // Adds the result to header_spec if any keys were added.
        // Returns (added_keys, incomplete_count).
        stanza::xep0384::axolotl_header header_spec(fmt::format("{}", device_id));

        auto add_axolotl_keys = [&](const std::string &recipient_jid,
                                   const std::string &device_list_str,
                                   int *out_incomplete_count = nullptr,
                                   bool include_own_device = false) -> bool
        {
            stanza::xep0384::axolotl_keys ks(recipient_jid);
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

                if (own_jid == recipient_jid && *remote_device_id == device_id && !include_own_device)
                    continue;

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
                        "OMEMO (legacy): failed to encrypt transport key for {}/{} — purging stale session",
                        recipient_jid, *remote_device_id));
                    // The session exists in LMDB but is un-encryptable (e.g. corrupted
                    // ratchet state from a previous buggy session).  Purge it so that
                    // the next encode attempt will call establish_session_from_bundle()
                    // and request a fresh bundle, recovering automatically.
                    auto stale_addr = make_signal_address(
                        recipient_jid, static_cast<std::int32_t>(*remote_device_id));
                    signal_protocol_session_delete_session(store_context, &stale_addr.address);
                    request_axolotl_bundle(*account, recipient_jid, *remote_device_id);
                    continue;
                }

                const auto encoded_transport = base64_encode(*account->context,
                                                             transport->first.data(),
                                                             transport->first.size());
                ks.add_key(stanza::xep0384::axolotl_key(
                    fmt::format("{}", *remote_device_id),
                    encoded_transport,
                    transport->second));
                added_keys = true;
            }

            if (added_keys)
                header_spec.add_keys(ks);

            return added_keys;
        };

        int incomplete_recipient_count = 0;
        added_any_key = add_axolotl_keys(target_jid, *devicelist, &incomplete_recipient_count);

        if (incomplete_recipient_count > 0)
            return nullptr;

        // Encrypt for own devices (carbon copy sync + self-message support).
        // Remove the own_jid != target_jid guard so self-addressed messages
        // also produce a key for the current device (same fix as encode()).
        {
            const auto own_legacy_devicelist = load_string(*this, key_for_axolotl_devicelist(own_jid));
            if (own_legacy_devicelist && !own_legacy_devicelist->empty())
            {
                // include_own_device=true: we need a key for our own device so the
                // carbon copy of this sent message can be decrypted by us.
                added_any_key = add_axolotl_keys(own_jid, *own_legacy_devicelist,
                                                 nullptr, /*include_own_device=*/true) || added_any_key;
            }
            else
            {
                // Interop fallback: if legacy own-devicelist cache is empty,
                // reuse OMEMO:2 own device ids for sender-side sync coverage.
                const auto own_omemo2_devicelist = load_string(*this, key_for_devicelist(own_jid));
                if (own_omemo2_devicelist && !own_omemo2_devicelist->empty())
                {
                    added_any_key = add_axolotl_keys(own_jid, *own_omemo2_devicelist,
                                                     nullptr, /*include_own_device=*/true) || added_any_key;
                }
            }
        }

        if (!added_any_key)
        {
            print_error(buffer, fmt::format(
                "OMEMO (legacy): could not encrypt for any known device of {}.", target_jid));
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

