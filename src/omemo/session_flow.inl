void weechat::xmpp::omemo::request_devicelist(weechat::account &account, std::string_view jid)
{
    ::request_devicelist(account, jid);
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
    
    weechat_printf(nullptr, "%somemo: handle_devicelist for %s: storing %zu device(s) [%s]",
                   weechat_prefix("network"), bare_jid.c_str(), devices.size(),
                   devicelist_str.empty() ? "(empty)" : devicelist_str.c_str());
    
    store_string(*this, key_for_devicelist(bare_jid), devicelist_str);

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

// Build and send an OMEMO:2 KeyTransportElement to `peer_jid` for `device_id`.
// A key-transport message establishes the Signal session from our side without
// sending any plaintext body.  Per XEP-0384 §7.3, it is a <message> stanza
// carrying <encrypted> with a <header> (keys) but NO <payload> element.
// This allows the remote party to learn our device_id and start encrypting
// future messages to us.
static void send_key_transport(omemo &self,
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

    const auto transport = encrypt_transport_key(self, peer_jid, remote_device_id, *ep);
    if (!transport)
    {
        print_error(buffer, fmt::format(
            "OMEMO: key-transport encrypt failed for {}/{}",
            peer_jid, remote_device_id));
        return;
    }

    const auto encoded_transport = base64_encode(*account.context,
                                                 transport->first.data(),
                                                 transport->first.size());

    // Build <encrypted xmlns='urn:xmpp:omemo:2'>
    xmpp_stanza_t *encrypted = xmpp_stanza_new(*account.context);
    xmpp_stanza_set_name(encrypted, "encrypted");
    xmpp_stanza_set_ns(encrypted, kOmemoNs.data());

    // <header sid='our_device_id'>
    xmpp_stanza_t *header = xmpp_stanza_new(*account.context);
    xmpp_stanza_set_name(header, "header");
    xmpp_stanza_set_attribute(header, "sid", fmt::format("{}", self.device_id).c_str());

    // <keys jid='peer_jid'><key rid='remote_device_id' [kex='true']>...</key></keys>
    xmpp_stanza_t *keys = xmpp_stanza_new(*account.context);
    xmpp_stanza_set_name(keys, "keys");
    xmpp_stanza_set_attribute(keys, "jid", peer_jid);

    xmpp_stanza_t *key_elem = xmpp_stanza_new(*account.context);
    xmpp_stanza_set_name(key_elem, "key");
    xmpp_stanza_set_attribute(key_elem, "rid", fmt::format("{}", remote_device_id).c_str());
    if (transport->second)
        xmpp_stanza_set_attribute(key_elem, "kex", "true");

    xmpp_stanza_t *key_text = xmpp_stanza_new(*account.context);
    xmpp_stanza_set_text(key_text, encoded_transport.c_str());
    xmpp_stanza_add_child(key_elem, key_text);
    xmpp_stanza_release(key_text);

    xmpp_stanza_add_child(keys, key_elem);
    xmpp_stanza_release(key_elem);
    xmpp_stanza_add_child(header, keys);
    xmpp_stanza_release(keys);
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
        peer_jid, remote_device_id, transport->second ? "true" : "false"));

    account.connection.send(message);
    xmpp_stanza_release(message);
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

    if (db_env && jid)
    {
        if (const auto bundle = extract_bundle_from_items(items))
        {
            store_bundle(*this, jid, remote_device_id, *bundle);
            if (!is_own_device)
            {
                const bool had_session_before = has_session(jid, remote_device_id);
                try
                {
                    (void)establish_session_from_bundle(
                        *this, account ? *account->context : nullptr, jid, remote_device_id);
                }
                catch (const std::exception &ex)
                {
                    if (buffer)
                        print_error(buffer, fmt::format(
                            "OMEMO session setup failed for {}/{}: {}",
                            jid, remote_device_id, ex.what()));
                }
                const bool session_is_fresh = !had_session_before && has_session(jid, remote_device_id);
                if ((session_is_fresh || needs_key_transport) && account && buffer)
                {
                    send_key_transport(*this, *account, buffer, jid, remote_device_id);
                }
            }
        }
    }

    if (buffer && jid)
    {
        const auto bundle = db_env ? load_bundle(*this, jid, remote_device_id) : std::nullopt;
        const auto prekey_count = bundle ? bundle->prekeys.size() : 0U;
        if (!is_own_device)
            print_info(buffer, fmt::format(
                "OMEMO received bundle for {}/{} ({} prekeys).",
                jid, remote_device_id, prekey_count));
    }

    // After successfully building a session with a remote contact's device,
    // auto-enable OMEMO on the corresponding PM channel if it has no transport
    // set yet. This ensures that the next message the user sends is encrypted
    // even if they haven't manually run /omemo on.
    if (!is_own_device && account && jid && has_session(jid, remote_device_id))
    {
        auto ch_it = account->channels.find(jid);
        if (ch_it != account->channels.end())
        {
            auto &ch = ch_it->second;
            if (ch.type == weechat::channel::chat_type::PM
                && !ch.omemo.enabled
                && ch.transport == weechat::channel::transport::PLAIN)
            {
                weechat_printf(ch.buffer,
                               "%sAuto-enabling OMEMO (OMEMO session established with %s)",
                               weechat_prefix("network"), jid);
                ch.omemo.enabled = 1;
                ch.set_transport(weechat::channel::transport::OMEMO, 0);
            }

            if (ch.type == weechat::channel::chat_type::PM)
                ch.flush_pending_omemo_messages();
        }

    }
}

