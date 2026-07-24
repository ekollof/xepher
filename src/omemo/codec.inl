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
                                   bool quiet,
                                   bool *out_is_duplicate,
                                   bool suppress_peer_traffic)
{
    OMEMO_ASSERT(account != nullptr, "OMEMO decode requires a valid account");
    OMEMO_ASSERT(jid != nullptr, "OMEMO decode requires a peer jid");
    OMEMO_ASSERT(encrypted != nullptr, "OMEMO decode requires an encrypted stanza");

    if (out_is_duplicate)
        *out_is_duplicate = false;

    if (!account || !jid || !encrypted)
    {
        print_error(buffer, "OMEMO decode received invalid input.");
        return std::nullopt;
    }

    const ::xmpp::StanzaView enc_view(encrypted);
    const ::xmpp::StanzaView header = enc_view.child("header");
    const ::xmpp::StanzaView payload_view = enc_view.child("payload");
    if (!header.valid())
    {
        print_error(buffer, "OMEMO message is missing header.");
        return std::nullopt;
    }

    const auto sender_device_id = parse_uint32(header.attr_string("sid"));
    if (!sender_device_id)
    {
        print_error(buffer, "OMEMO message header is missing a valid sender sid.");
        return std::nullopt;
    }
    // payload may be absent for KeyTransportElement (XEP-0384 §7.3).
    const auto payload_text = payload_view.valid() ? payload_view.text() : std::string {};
    const auto payload = payload_view.valid()
        ? base64_decode(*account->context, payload_text)
        : std::vector<std::uint8_t> {};
    // A missing or empty payload is only an error for full encrypted messages,
    // not for key-transport stanzas.
    if (payload_view.valid() && payload.empty())
    {
        print_error(buffer, "OMEMO payload is present but empty or invalid base64.");
        return std::nullopt;
    }

    const std::string own_bare_jid = ::jid(nullptr, account->jid().data()).bare;
    const std::string peer_bare = normalize_bare_jid(*account->context, jid);

    // Learn sender device for future encode (Psi+/multi-client often appears as
    // sid before PEP devicelist is refreshed). Skip own-device self-echo.
    if (!peer_bare.empty() && *sender_device_id != device_id
        && peer_bare != own_bare_jid)
    {
        if (ensure_axolotl_device_on_list(*this, peer_bare, *sender_device_id)
            && !suppress_peer_traffic)
        {
            // New device: pull full list (merge with Converse/etc.) and bundle.
            request_axolotl_devicelist(*account, peer_bare);
            if (!has_session(peer_bare.c_str(), *sender_device_id))
                request_axolotl_bundle(*account, peer_bare, *sender_device_id);
        }
    }

    const auto enc_ns = enc_view.xmlns();
    if (enc_ns)
        XDEBUG("OMEMO decode: <encrypted> xmlns='{}' peer='{}' sid={} payload={} mam_replay={}",
               *enc_ns, jid, *sender_device_id,
               payload_view.valid() && !payload.empty() ? "yes" : "no",
               suppress_peer_traffic ? "yes" : "no");

    // Inbound copies of messages sent from this device cannot be decrypted — the
    // Signal protocol never establishes {own_jid, own_device_id} as an inbound
    // session. Catches any handler path that still reaches decode().
    if (*sender_device_id == device_id)
    {
        XDEBUG("OMEMO decode: skipping self-sent stanza (sid == own device_id {})", device_id);
        return std::nullopt;
    }

    if (!enc_ns || *enc_ns != kLegacyOmemoNs)
    {
        if (!quiet)
            print_error(buffer, fmt::format(
                "OMEMO: unexpected <encrypted> namespace '{}'; expected '{}'.",
                enc_ns ? std::string(*enc_ns) : "(none)", kLegacyOmemoNs));
        return std::nullopt;
    }

    const ::xmpp::StanzaView iv_view = header.child("iv");

    // Receiving an OMEMO message from a peer counts as observed traffic for
    // that peer — this unblocks bundle requests (which check has_peer_traffic)
    // so the recovery path can fetch their bundle and send a key-transport when
    // decryption fails due to a stale/mismatched session.
    // During MAM replay we suppress this: we must not mark peers as having
    // observed live traffic based on archived messages, as that would trigger
    // spurious bundle fetches (e.g. for own ConverseJS devices).
    // suppress_peer_traffic is passed as true for MAM replays; quiet alone is
    // no longer sufficient since MAM now decodes with quiet=false for logging.
    if (!quiet && !suppress_peer_traffic)
        note_peer_traffic(account->context, jid);

    // Legacy transport key: {innerKey16, authTag16}
    std::optional<std::pair<std::array<std::uint8_t, 16>, std::array<std::uint8_t, 16>>> legacy_transport_key;
    std::optional<std::uint32_t> used_prekey_id;
    std::optional<std::uint32_t> ratchet_message_counter;
    bool decoded_as_prekey = false; // true if the decrypted key element had kex/prekey=true
    bool found_keys_elem = false;
    bool found_keys_for_our_bare_jid = false;
    bool found_key_for_us = false;
    for (const ::xmpp::StanzaView child : header)
    {
        if (legacy_transport_key)
            break;
        if (!stanza_attr_iequals(child.name(), "keys"))
            continue;
        found_keys_elem = true;

        const auto keys_jid = child.attr("jid");
        XDEBUG("OMEMO decode: <keys jid='{}'> element found",
               keys_jid ? std::string(*keys_jid) : "(none)");
        if (!keys_jid)
        {
            // Legacy compatibility: OMEMO:1 senders may omit keys@jid.
            XDEBUG("OMEMO decode: accepting legacy <keys> without jid attribute.");
            found_keys_for_our_bare_jid = true;
        }

        if (keys_jid && keys_jid->contains('/'))
        {
            print_error(buffer, fmt::format(
                "OMEMO message has non-bare keys jid '{}'; ignoring non-compliant element.",
                *keys_jid));
            continue;
        }

        if (keys_jid && own_bare_jid != *keys_jid)
            continue;

        if (keys_jid)
            found_keys_for_our_bare_jid = true;

        for (const ::xmpp::StanzaView key_stanza : child)
        {
            if (legacy_transport_key)
                break;
            if (!stanza_attr_iequals(key_stanza.name(), "key"))
                continue;

            const auto rid_val = parse_uint32(key_stanza.attr_string("rid")).value_or(0);
            XDEBUG("OMEMO decode:   <key rid='{}'> (our device_id={})",
                   key_stanza.attr_string("rid"), device_id);
            if (rid_val != device_id)
                continue;

            found_key_for_us = true;
            const bool is_prekey = omemo_key_is_prekey(key_stanza);
            decoded_as_prekey = is_prekey;
            if (!key_stanza.attr("kex") && key_stanza.attr("prekey"))
            {
                XDEBUG("OMEMO decode: accepting legacy 'prekey' key attribute; strict XEP-0384 uses 'kex'.");
            }
            const auto serialized = base64_decode(*account->context, key_stanza.text());
            XDEBUG("OMEMO decode:   found key for us: is_prekey={} serialized-bytes={}",
                   is_prekey, serialized.size());
            if (serialized.empty())
            {
                print_error(buffer, "OMEMO key element for our device has empty/invalid base64.");
                continue;
            }

            XDEBUG("OMEMO decode: decrypting transport key for {}/{} prekey={} bytes={}",
                   jid, *sender_device_id, is_prekey, serialized.size());
            legacy_transport_key = decrypt_axolotl_transport_key(*this, jid, *sender_device_id, serialized, is_prekey,
                                                                 is_prekey ? &used_prekey_id : nullptr,
                                                                 &ratchet_message_counter,
                                                                 out_is_duplicate);
            XDEBUG("OMEMO decode: transport key decrypt {} for {}/{}",
                   legacy_transport_key ? "ok" : "failed", jid, *sender_device_id);
            if (!legacy_transport_key && !quiet && !(out_is_duplicate && *out_is_duplicate))
                XDEBUG("OMEMO decode: legacy Signal transport-key decrypt failed for {}/{}",
                       jid, *sender_device_id);
        }
    }

    // Legacy compatibility: some OMEMO:1 payloads place <key/> elements
    // directly under <header> instead of wrapping them in <keys/>.
    if (!legacy_transport_key && !found_keys_elem)
    {
        for (const ::xmpp::StanzaView key_stanza : header)
        {
            if (legacy_transport_key)
                break;
            if (!stanza_attr_iequals(key_stanza.name(), "key"))
                continue;

            // Legacy OMEMO:1 layout: <header><key .../></header> has no
            // intermediate <keys jid='...'> wrapper. Seeing at least one
            // <key/> means keys are present for this (bare) peer.
            found_keys_elem = true;
            found_keys_for_our_bare_jid = true;

            const auto rid_val = parse_uint32(key_stanza.attr_string("rid")).value_or(0);
            if (rid_val != device_id)
                continue;

            found_key_for_us = true;

            const bool is_prekey = omemo_key_is_prekey(key_stanza);
            decoded_as_prekey = is_prekey;

            const auto serialized = base64_decode(*account->context, key_stanza.text());
            if (serialized.empty())
                continue;

            XDEBUG("OMEMO decode: decrypting legacy flat <key> for {}/{} prekey={}",
                   jid, *sender_device_id, is_prekey);
            legacy_transport_key = decrypt_axolotl_transport_key(*this, jid, *sender_device_id, serialized, is_prekey,
                                                                 is_prekey ? &used_prekey_id : nullptr,
                                                                 &ratchet_message_counter,
                                                                 out_is_duplicate);
            XDEBUG("OMEMO decode: legacy flat <key> decrypt {} for {}/{}",
                   legacy_transport_key ? "ok" : "failed", jid, *sender_device_id);
        }
    }

    if (!legacy_transport_key)
    {
        if (!quiet && !suppress_peer_traffic)
        {
            if (!found_keys_elem)
                XDEBUG("OMEMO decode: message has no <keys> element in header (peer={})", jid);
            else if (!found_keys_for_our_bare_jid)
                XDEBUG("OMEMO decode: message has no <keys jid='{}'> for our bare JID (peer={})",
                       own_bare_jid, jid);
        }
        if (!found_key_for_us)
        {
            // The sender does not (yet) know our device_id. Request their bundle
            // once per peer device and avoid repeating the same error every message.
            // During MAM replay (suppress_peer_traffic==true) we suppress both the
            // error print and the bundle fetch — archived messages are not live traffic.
            if (!suppress_peer_traffic && account && sender_device_id)
            {
                const std::string bare_jid = normalize_bare_jid(*account->context, jid);
                const auto key = std::make_pair(bare_jid, *sender_device_id);
                const bool already_attempted = key_transport_bootstrap_attempted.contains(key);
                const bool already_pending = pending_key_transport.contains(key);

                if (!already_attempted && !already_pending)
                {
                    if (!quiet)
                        print_error(buffer, fmt::format(
                            "OMEMO message has no key for our device {} (sender did not encrypt for us).",
                            device_id));

                    pending_key_transport.insert(key);
                    request_axolotl_bundle(*account, bare_jid, *sender_device_id);
                }
            }
            else if (!quiet && !suppress_peer_traffic)
            {
                print_error(buffer, fmt::format(
                    "OMEMO message has no key for our device {} (sender did not encrypt for us).",
                    device_id));
            }
        }
        else
        {
            // found_key_for_us==true but decryption failed.
            // If it's a duplicate (SG_ERR_DUPLICATE_MESSAGE), the Signal ratchet
            // already consumed this message on a prior live delivery.  Skip all
            // recovery actions — there is nothing to recover.
            const bool is_duplicate = out_is_duplicate && *out_is_duplicate;
            if (!quiet && !suppress_peer_traffic && !is_duplicate)
                XDEBUG("OMEMO decode: transport-key decrypt failed for {}/{} — recovery may follow",
                       jid, sender_device_id ? *sender_device_id : 0u);
            if (!is_duplicate && account && sender_device_id)
            {
                const std::string bare_jid = normalize_bare_jid(*account->context, jid);
                // Self-outbound copy: do not corrupt own session.
                if (bare_jid == own_bare_jid)
                    return std::nullopt;
                const auto kex_key = std::make_pair(bare_jid, *sender_device_id);
                if (!suppress_peer_traffic)
                {
                    // Live delivery: aggressively recover — delete stale session and
                    // immediately request bundle + queue key-transport.
                    if (!key_transport_bootstrap_attempted.contains(kex_key))
                    {
                        key_transport_bootstrap_attempted.insert(kex_key);
                        if (!quiet)
                            XDEBUG("omemo: queueing key-transport to {}/{} to recover stale session",
                                   bare_jid, *sender_device_id);
                        // Delete the stale session so the key-transport forces a fresh
                        // PreKeySignalMessage exchange from the sender's side.
                        auto addr = make_signal_address(bare_jid,
                                                        static_cast<std::int32_t>(*sender_device_id));
                        signal_protocol_session_delete_session(store_context, &addr.address);
                        pending_key_transport.insert(kex_key);
                        request_axolotl_bundle(*account, bare_jid, *sender_device_id);
                    }
                }
                else
                {
                    // MAM replay: decryption failed for a real peer's archived message.
                    // Queue a deferred key-transport so the session is recovered after
                    // MAM catchup ends (process_postponed_key_transports will send it).
                    // Do not delete the session here — if we still have a valid session
                    // from a more recent message, we do not want to clobber it.
                    if (!key_transport_bootstrap_attempted.contains(kex_key)
                        && !pending_key_transport.contains(kex_key))
                    {
                        XDEBUG("omemo: MAM decrypt failed for {}/{} — queuing deferred session recovery",
                               bare_jid, *sender_device_id);
                        pending_key_transport.insert(kex_key);
                    }
                }
            }
        }
        return std::nullopt;
    }

    // Key-transport element: no payload, nothing more to decrypt.
    // The session is now established from our side.
    if (!payload_view.valid() || payload.empty())
    {
        XDEBUG("OMEMO: received KeyTransportElement — session established.");
        return std::nullopt;
    }

    // Legacy format: AES-128-GCM decrypt path (no SCE wrapping)
    const auto iv_text = iv_view.text();
    const auto iv_vec = base64_decode(*account->context, iv_text);
    if (iv_vec.size() != 12)
    {
        print_error(buffer, "OMEMO (legacy): IV element has wrong size (expected 12 bytes).");
        return std::nullopt;
    }
    std::array<std::uint8_t, 12> iv {};
    std::ranges::copy_n(iv_vec.begin(), 12, iv.begin());
    const auto& [lkey, ltag] = *legacy_transport_key;
    const auto result = axolotl_omemo_decrypt(lkey, iv, ltag, payload);
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
                        (void)send_within_stanza_byte_limit(
                            account->connection, lbs.get(),
                            k_proxy_safe_stanza_bytes, "OMEMO bundle republish");
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
            && !heartbeat_sent.contains(hb_key))
        {
            heartbeat_sent.insert(hb_key);
            XDEBUG("omemo: queueing heartbeat key-transport to {}/{} (counter={})",
                   bare_jid, *sender_device_id, *ratchet_message_counter);
            deferred_live_key_transports.insert(hb_key);
        }
    }
    // XEP-0384 §6: after successfully decrypting a PreKeySignalMessage, send back an
    // empty OMEMO message so the sender can stop sending OMEMOKeyExchanges.  During
    // MAM catch-up the reply is deferred until <fin> (§6 also requires this empty
    // message after key exchange during catch-up to forward the ratchet).
    if (decoded_as_prekey && account && sender_device_id)
    {
        const std::string bare_jid = normalize_bare_jid(*account->context, jid);
        const auto pk_key = std::make_pair(bare_jid, *sender_device_id);
        if (!prekey_reply_sent.contains(pk_key))
        {
            prekey_reply_sent.insert(pk_key);
            if (global_mam_catchup)
            {
                XDEBUG("omemo (legacy): deferring prekey-reply key-transport to {}/{} (MAM catchup)",
                       bare_jid, *sender_device_id);
                postponed_key_transports.insert(pk_key);
            }
            else
            {
                XDEBUG("omemo (legacy): queueing prekey-reply key-transport to {}/{}",
                       bare_jid, *sender_device_id);
                deferred_live_key_transports.insert(pk_key);
            }
        }
    }
    return std::string(*result);
}

xmpp_stanza_t *weechat::xmpp::omemo::encode(weechat::account *account,
                                            struct t_gui_buffer *buffer,
                                            std::string_view jid,
                                            std::string_view unencrypted)
{
    OMEMO_ASSERT(account != nullptr, "OMEMO encode requires a valid account");
    OMEMO_ASSERT(!jid.empty(), "OMEMO encode requires a peer jid");
    OMEMO_ASSERT(!unencrypted.empty(), "OMEMO encode requires plaintext input");

    if (!*this || !account || jid.empty() || unencrypted.empty())
        return nullptr;

    ensure_local_identity(*this);
    ensure_registration_id(*this);

    std::string target_jid = ::jid(nullptr, jid.data()).bare;
    if (target_jid.empty())
        target_jid = std::string(jid);

    note_peer_traffic(account->context, target_jid);
    {
        const std::array<std::string_view, 1> prefetch_jids{target_jid};
        prefetch_encode_lmdb(*this, prefetch_jids);
    }

    // Look up device list — stored under legacy key set by the legacy devicelist handler
    const auto devicelist = load_axolotl_devicelist(*this, target_jid);
    if (!devicelist || devicelist->empty())
    {
        request_axolotl_devicelist(*account, target_jid);
        print_error(buffer, fmt::format(
            "OMEMO: no device list cached for {}. Requested.", target_jid));
        return nullptr;
    }

    // Legacy OMEMO uses AES-128-GCM on the raw message text (no SCE wrapping)
    const auto ep = axolotl_omemo_encrypt(unencrypted);
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

    auto add_axolotl_keys = [&](std::string_view recipient_jid,
                               std::string_view device_list_str,
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

            // BTBV trust gate: skip UNTRUSTED; skip UNDECIDED for peers only.
            // Own-account sibling devices must still receive keys so carbons and
            // other clients (e.g. Movim) can decrypt outbound PM traffic.
            {
                const auto trust = load_tofu_trust(*this, recipient_jid, *remote_device_id);
                const bool is_own_account = (recipient_jid == own_jid);
                if (trust && *trust == omemo_trust::UNTRUSTED)
                {
                    XDEBUG("omemo encode: skipping device {}/{} (trust=UNTRUSTED)",
                           recipient_jid, *remote_device_id);
                    continue;
                }
                if (trust && *trust == omemo_trust::UNDECIDED && !is_own_account)
                {
                    XDEBUG("omemo encode: skipping device {}/{} (trust=UNDECIDED)",
                           recipient_jid, *remote_device_id);
                    continue;
                }
            }

            if (failed_session_bootstrap.contains({std::string(recipient_jid), *remote_device_id}))
                continue;

            if (!has_session(std::string(recipient_jid).c_str(), *remote_device_id))
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

            const auto& [tkey, is_kex] = *transport;
            const auto encoded_transport = base64_encode(*account->context, tkey);
            // Flat legacy layout: add <key> directly under <header>
            header_spec.add_key(stanza::xep0384::axolotl_key(
                fmt::format("{}", *remote_device_id),
                encoded_transport,
                is_kex));
            added_keys = true;
        }

        return added_keys;
    };

    int incomplete_recipient_count = 0;
    added_any_key = add_axolotl_keys(target_jid, *devicelist, &incomplete_recipient_count);

    // Do not block the whole send when some peer devices lack sessions (stale
    // multi-client lists). Encrypt for every device we can; refresh the rest.
    if (incomplete_recipient_count > 0 && !added_any_key)
    {
        request_axolotl_devicelist(*account, target_jid);
        print_error(buffer, fmt::format(
            "OMEMO: waiting for {} recipient device bundle(s) for {} — message not sent.",
            incomplete_recipient_count, target_jid));
        return nullptr;
    }
    if (incomplete_recipient_count > 0)
    {
        print_info(buffer, fmt::format(
            "OMEMO: encrypted for some devices of {}; {} still missing bundle/session "
            "(requested). Other clients on that account may not decrypt this message.",
            target_jid, incomplete_recipient_count));
    }

    // Encrypt for own devices (carbon copy sync + self-message support).
    {
        const auto own_legacy_devicelist = load_axolotl_devicelist(*this, own_jid);
        if (own_legacy_devicelist && !own_legacy_devicelist->empty())
        {
            int incomplete_own_devices = 0;
            added_any_key = add_axolotl_keys(own_jid, *own_legacy_devicelist,
                                             &incomplete_own_devices,
                                             /*include_own_device=*/true) || added_any_key;
            if (incomplete_own_devices > 0)
            {
                print_info(buffer, fmt::format(
                    "OMEMO: message sent, but {} own device(s) may not decrypt the "
                    "carbon copy (missing bundle/session). Try /omemo fetch {} <device-id>",
                    incomplete_own_devices, own_jid));
            }
        }
        else
        {
            request_axolotl_devicelist(*account, own_jid);
            print_info(buffer, fmt::format(
                "OMEMO: own devicelist not cached — requested refresh; other clients "
                "may miss this message until /omemo fetch {} <device-id> completes",
                own_jid));
        }
    }

    if (!added_any_key)
    {
        print_error(buffer, fmt::format(
            "OMEMO: could not encrypt for any known device of {}.", target_jid));
        return nullptr;
    }

    // Legacy: include <iv>base64</iv> in the header (12-byte GCM nonce)
    const auto encoded_iv = base64_encode(*account->context, ep->iv);
    header_spec.add_iv(stanza::xep0384::axolotl_iv(encoded_iv));

    // Legacy: <payload>base64 AES-128-GCM ciphertext (auth tag stripped)</payload>
    const auto encoded_payload = base64_encode(*account->context, ep->payload);

    stanza::xep0384::axolotl_encrypted enc_spec;
    enc_spec.add_header(header_spec)
            .add_payload(stanza::xep0384::axolotl_payload(encoded_payload));

    auto sp = enc_spec.build(*account->context);
    xmpp_stanza_clone(sp.get());  // bump refcount; shared_ptr dtor will release its ref
    return sp.get();              // caller owns one ref; must call xmpp_stanza_release()
}

// Multi-recipient encode for MUC OMEMO (docs/planning-muc-omemo.md §3.1 + §6).
// Encrypts the payload once, then for each recipient bare JID produces a
// <keys jid="..."> wrapper containing the encrypted transport keys for
// that recipient's devices (plus own devices). Uses the legacy axolotl
// namespace as required by the project.
//
// Trust note: Occupants are looked up by their real bare JID. New occupants
// discovered via presence/disco/admin are treated with the project's default
// BTBV trust (BLIND). Only VERIFIED and BLIND devices receive key material.
// This is intentional for v1 but users should be aware they are blind-trusting
// everyone in the room by default.
xmpp_stanza_t *weechat::xmpp::omemo::encode_muc(weechat::account *account,
                                                struct t_gui_buffer *buffer,
                                                std::string_view room_jid,
                                                const std::vector<std::string>& recipient_bare_jids,
                                                std::string_view unencrypted)
{
    (void)room_jid;  // may be used for logging/debug in future
    OMEMO_ASSERT(account != nullptr, "MUC OMEMO encode requires a valid account");
    if (!*this || !account || recipient_bare_jids.empty() || unencrypted.empty())
        return nullptr;

    ensure_local_identity(*this);
    ensure_registration_id(*this);

    // Encrypt the plaintext payload once (AES-128-GCM). All recipients get the same ciphertext.
    const auto ep = axolotl_omemo_encrypt(unencrypted);
    if (!ep)
    {
        print_error(buffer, "MUC OMEMO: AES-128-GCM payload encryption failed.");
        return nullptr;
    }

    const std::string own_jid = [&]{
        auto b = ::jid(nullptr, account->jid().data()).bare;
        return b.empty() ? std::string(account->jid()) : b;
    }();

    {
        std::vector<std::string> prefetch_jids;
        prefetch_jids.reserve(recipient_bare_jids.size() + 1);
        for (const auto &rec : recipient_bare_jids)
        {
            std::string r = ::jid(nullptr, rec).bare;
            if (r.empty())
                r = rec;
            prefetch_jids.push_back(std::move(r));
        }
        prefetch_jids.push_back(own_jid);
        std::vector<std::string_view> prefetch_views;
        prefetch_views.reserve(prefetch_jids.size());
        for (const auto &j : prefetch_jids)
            prefetch_views.push_back(j);
        prefetch_encode_lmdb(*this, prefetch_views);
    }

    stanza::xep0384::axolotl_header header_spec(fmt::format("{}", device_id));

    bool added_any_key = false;
    int total_incomplete = 0;
    std::vector<std::string> failed_recipients;  // for better error reporting (plan §3.2)

    auto encrypt_for_recipient = [&](std::string_view recipient_jid,
                                     std::string_view device_list_str,
                                     bool include_own_device) -> bool
    {
        bool added = false;
        stanza::xep0384::axolotl_keys keys_for_this(recipient_jid);

        for (const auto &dev : split(device_list_str, ';'))
        {
            const auto remote_device_id = parse_uint32(dev);
            if (!remote_device_id || !is_valid_omemo_device_id(*remote_device_id))
                continue;

            if (own_jid == recipient_jid && *remote_device_id == device_id && !include_own_device)
                continue;

            {
                const auto trust = load_tofu_trust(*this, recipient_jid, *remote_device_id);
                const bool is_own_account = (recipient_jid == own_jid);
                if (trust && *trust == omemo_trust::UNTRUSTED)
                    continue;
                if (trust && *trust == omemo_trust::UNDECIDED && !is_own_account)
                    continue;
            }

            if (failed_session_bootstrap.contains({std::string(recipient_jid), *remote_device_id}))
                continue;

            if (!has_session(std::string(recipient_jid).c_str(), *remote_device_id))
            {
                if (!establish_session_from_bundle(*this, *account->context, recipient_jid, *remote_device_id))
                {
                    request_axolotl_bundle(*account, recipient_jid, *remote_device_id);
                    ++total_incomplete;
                    if (std::ranges::find(failed_recipients, std::string(recipient_jid)) == failed_recipients.end())
                        failed_recipients.push_back(std::string(recipient_jid));
                    continue;
                }
            }

            const auto transport = encrypt_axolotl_transport_key(*this, recipient_jid, *remote_device_id, *ep);
            if (!transport)
            {
                ++total_incomplete;
                if (std::ranges::find(failed_recipients, std::string(recipient_jid)) == failed_recipients.end())
                    failed_recipients.push_back(std::string(recipient_jid));
                auto stale_addr = make_signal_address(recipient_jid, static_cast<std::int32_t>(*remote_device_id));
                signal_protocol_session_delete_session(store_context, &stale_addr.address);
                request_axolotl_bundle(*account, recipient_jid, *remote_device_id);
                continue;
            }

            const auto& [tkey, is_kex] = *transport;
            const auto encoded_transport = base64_encode(*account->context, tkey);
            keys_for_this.add_key(stanza::xep0384::axolotl_key(
                fmt::format("{}", *remote_device_id),
                encoded_transport,
                is_kex));
            added = true;
        }

        if (added)
        {
            header_spec.add_keys(keys_for_this);
            added_any_key = true;
        }
        return added;
    };

    for (const auto &rec : recipient_bare_jids)
    {
        std::string r = ::jid(nullptr, rec).bare;
        if (r.empty())
            r = rec;
        note_peer_traffic(account->context, r);
    }

    // For every recipient provided by the MUC caller
    for (const auto& rec : recipient_bare_jids)
    {
        std::string r = ::jid(nullptr, rec).bare;
        if (r.empty()) r = rec;

        const auto dl = load_axolotl_devicelist(*this, r);
        if (dl && !dl->empty())
        {
            encrypt_for_recipient(r, *dl, /*include_own_device*/ false);
        }
        else
        {
            request_axolotl_devicelist(*account, r);
            ++total_incomplete;
            if (std::ranges::find(failed_recipients, r) == failed_recipients.end())
                failed_recipients.push_back(r);
        }
    }

    // Own devices (so other of our clients can decrypt the MUC message)
    {
        const auto own_dl = load_axolotl_devicelist(*this, own_jid);
        if (own_dl && !own_dl->empty())
        {
            encrypt_for_recipient(own_jid, *own_dl, /*include_own_device*/ true);
        }
    }

    if (total_incomplete > 0 || !added_any_key)
    {
        if (!failed_recipients.empty())
        {
            std::string jids = failed_recipients.front();
            for (size_t i = 1; i < failed_recipients.size(); ++i)
                jids += ", " + failed_recipients[i];
            print_error(buffer, fmt::format("MUC OMEMO: could not encrypt for all required devices. Missing keys for: {}", jids));
        }
        else
        {
            print_error(buffer, "MUC OMEMO: could not encrypt for all required devices (some bundles still pending).");
        }
        return nullptr;
    }

    const auto encoded_iv = base64_encode(*account->context, ep->iv);
    header_spec.add_iv(stanza::xep0384::axolotl_iv(encoded_iv));

    const auto encoded_payload = base64_encode(*account->context, ep->payload);

    stanza::xep0384::axolotl_encrypted enc_spec;
    enc_spec.add_header(header_spec)
            .add_payload(stanza::xep0384::axolotl_payload(encoded_payload));

    auto sp = enc_spec.build(*account->context);
    xmpp_stanza_clone(sp.get());
    return sp.get();
}
