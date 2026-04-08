static void send_key_transport(omemo &self,
                               weechat::account &account,
                               struct t_gui_buffer *buffer,
                               const char *peer_jid,
                               std::uint32_t remote_device_id);

void weechat::xmpp::omemo::request_axolotl_devicelist(weechat::account &account, std::string_view jid)
{
    ::request_axolotl_devicelist(account, jid);
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

    request_axolotl_devicelist(account, bare_jid);

    if (device_id)
    {
        if (!is_valid_omemo_device_id(*device_id))
        {
            print_error(buffer ? buffer : account.buffer,
                        fmt::format("OMEMO: invalid device id {}.", *device_id));
            return;
        }

        request_axolotl_bundle(account, bare_jid, *device_id);
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

    collect_devices(load_string(*this, key_for_axolotl_devicelist(bare_jid)));

    for (const auto known_device_id : known_devices)
    {
        request_axolotl_bundle(account, bare_jid, known_device_id);
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

        collect_devices(load_string(*this, key_for_axolotl_devicelist(bare_jid)));
    }

    if (target_devices.empty())
    {
        request_axolotl_devicelist(account, bare_jid);
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
        request_axolotl_bundle(account, bare_jid, remote_device_id);
        ++queued;
    }

    print_info(buffer ? buffer : account.buffer,
               fmt::format("OMEMO: force-kex for {}: sent now={}, queued after bundle fetch={}",
                           bare_jid, sent_now, queued));
}

XMPP_TEST_EXPORT void weechat::xmpp::omemo::note_peer_traffic(xmpp_ctx_t *context, std::string_view jid)
{
    if (jid.empty())
        return;

    peers_with_observed_traffic.insert(normalize_bare_jid(context, jid));
}

XMPP_TEST_EXPORT auto weechat::xmpp::omemo::has_peer_traffic(xmpp_ctx_t *context,
                                            std::string_view jid) const -> bool
{
    if (jid.empty())
        return false;

    const std::string bare_jid = normalize_bare_jid(context, jid);
    return peers_with_observed_traffic.count(bare_jid) != 0;
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


XMPP_TEST_EXPORT void weechat::xmpp::omemo::handle_axolotl_devicelist(weechat::account *account,
                                                    const char *jid,
                                                    xmpp_stanza_t *items)
{
    if (!db_env || !jid)
    {
        weechat_printf(nullptr,
                       "%somemo: handle_axolotl_devicelist: invalid args (jid=%s)",
                       weechat_prefix("error"),
                       jid ? jid : "(null)");
        return;
    }

    std::string bare_jid = jid;
    if (account && account->context)
    {
        auto b = ::jid(nullptr, jid).bare;
        if (!b.empty())
            bare_jid = std::move(b);
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
    missing_axolotl_devicelist.erase(bare_jid);
    store_string(*this, key_for_axolotl_devicelist(bare_jid), devicelist_str);

    XDEBUG("omemo: handle_axolotl_devicelist for {}: storing {} device(s) [{}]",
           bare_jid,
           devices.size(),
           devicelist_str.empty() ? "(empty)" : devicelist_str);

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

static void send_key_transport(omemo &self,
                               weechat::account &account,
                               struct t_gui_buffer *buffer,
                               const char *peer_jid,
                               std::uint32_t remote_device_id)
{
    if (!peer_jid)
        return;

    // Per XEP-0384 §5.5.3 / Conversations empty OMEMO message: for a
    // KeyTransportElement the innerKey(16) and authTag(16) SHOULD be zero bytes.
    // IV is also zero (no payload to decrypt with it).
    const axolotl_omemo_payload ep{};  // all-zero key, iv, authtag, empty payload

    const auto encoded_iv = base64_encode(*account.context,
                                          ep.iv.data(),
                                          ep.iv.size());

    const std::string own_bare_jid = [&]{
        auto b = ::jid(nullptr, account.jid().data()).bare;
        return b.empty() ? std::string(account.jid()) : b;
    }();

    stanza::xep0384::axolotl_header header_spec(fmt::format("{}", self.device_id));
    bool peer_has_keys = false;
    bool own_has_keys = false;

    // Legacy flat layout: add <key> directly under <header> (no <keys jid=...> wrapper).
    // This is the format nbxmpp/Gajim and Conversations expect.
    auto add_axolotl_key = [&](const std::string &target_jid,
                               std::uint32_t target_device_id) -> bool
    {
        if (!is_valid_omemo_device_id(target_device_id))
            return false;

        const auto target_transport = encrypt_axolotl_transport_key(self, target_jid, target_device_id, ep);
        if (!target_transport)
            return false;

        const auto target_encoded_transport = base64_encode(*account.context,
                                                            target_transport->first.data(),
                                                            target_transport->first.size());
        header_spec.add_key(stanza::xep0384::axolotl_key(
            fmt::format("{}", target_device_id),
            target_encoded_transport,
            target_transport->second));
        return true;
    };

    if (add_axolotl_key(peer_jid, remote_device_id))
        peer_has_keys = true;

    const auto own_legacy_devicelist = load_string(self, key_for_axolotl_devicelist(own_bare_jid));
    if (own_legacy_devicelist && !own_legacy_devicelist->empty())
    {
        for (const auto &dev : split(*own_legacy_devicelist, ';'))
        {
            const auto own_device = parse_uint32(dev);
            if (!own_device || *own_device == self.device_id)
                continue;

            if (self.failed_session_bootstrap.count({own_bare_jid, *own_device}) > 0)
                continue;

            if (!self.has_session(own_bare_jid.c_str(), *own_device)
                && !establish_session_from_bundle(self, *account.context, own_bare_jid, *own_device))
            {
                request_axolotl_bundle(account, own_bare_jid, *own_device);
                continue;
            }

            if (add_axolotl_key(own_bare_jid, *own_device))
                own_has_keys = true;
        }
    }

    if (!peer_has_keys && !own_has_keys)
    {
        print_error(buffer, fmt::format(
            "OMEMO: key-transport encrypt failed for {}/{}",
            peer_jid, remote_device_id));
        return;
    }

    header_spec.add_iv(stanza::xep0384::axolotl_iv(encoded_iv));

    stanza::xep0384::axolotl_encrypted enc_spec;
    enc_spec.add_header(header_spec);

    print_info(buffer, fmt::format(
        "OMEMO: sending key-transport to {}/{}",
        peer_jid, remote_device_id));

    auto msg_sp = stanza::message()
        .type("chat")
        .to(peer_jid)
        .id(stanza::uuid(*account.context))
        .omemo_axolotl_encrypted(enc_spec)
        .omemo_store_hint(stanza::xep0384::store_hint{})
        .build(*account.context);

    account.connection.send(msg_sp.get());
}



// Identical logic to handle_bundle() but uses extract_legacy_bundle_from_items()
// which parses the Conversations/axolotl stanza format.
XMPP_TEST_EXPORT void weechat::xmpp::omemo::handle_axolotl_bundle(weechat::account *account,
                                                struct t_gui_buffer *buffer,
                                                const char *jid,
                                                std::uint32_t remote_device_id,
                                                xmpp_stanza_t *items)
{
    std::string bare_jid = jid ? jid : "";
    std::string own_bare_jid;
    if (account && account->context && jid)
    {
        auto b = ::jid(nullptr, jid).bare;
        if (!b.empty())
            bare_jid = std::move(b);

        auto ob = ::jid(nullptr, account->jid().data()).bare;
        if (!ob.empty())
            own_bare_jid = std::move(ob);
        else
            own_bare_jid = account->jid();
    }
    const bool is_own_device = (account && !bare_jid.empty() && own_bare_jid == bare_jid)
                                && (remote_device_id == device_id);

    // Bootstrap-race guard: same as handle_bundle() — mark attempted before
    // clearing pending sets so a concurrent decode() cannot re-enqueue a
    // bundle fetch for the same pair while we are processing this result.
    if (!is_own_device && !bare_jid.empty())
        key_transport_bootstrap_attempted.insert({bare_jid, remote_device_id});

    pending_bundle_fetch.erase({jid ? jid : "", remote_device_id});
    const bool needs_key_transport = pending_key_transport.count({jid ? jid : "", remote_device_id}) > 0;
    pending_key_transport.erase({jid ? jid : "", remote_device_id});

    if (db_env && !bare_jid.empty())
    {
        if (const auto bundle = extract_legacy_bundle_from_items(items))
        {
            // Store under the legacy key prefix so we know it came from the
            // axolotl namespace; the Signal session is shared with OMEMO:2.
            store_string(*this, key_for_axolotl_bundle(bare_jid, remote_device_id),
                         serialize_bundle(*bundle));
            // Also store under the canonical bundle key so establish_session_from_bundle()
            // can find it without knowing which namespace it came from.
            store_bundle(*this, bare_jid, remote_device_id, *bundle);

            if (!is_own_device)
            {
                failed_session_bootstrap.erase({bare_jid, remote_device_id});
                const bool had_session_before = has_session(bare_jid.c_str(), remote_device_id);
                // Only bootstrap a new session when no session exists yet.
                // Never overwrite an existing established session: doing so resets
                // the Signal ratchet and causes the remote peer to receive a second
                // PreKeySignalMessage that desynchronises its session state
                // (the peer initialises from the new prekey bundle while we continue
                // ratcheting from the original session, leading to undecryptable messages).
                const bool should_bootstrap = !had_session_before;
                if (should_bootstrap)
                {
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
                }
                const bool session_is_fresh = !had_session_before
                    && has_session(bare_jid.c_str(), remote_device_id);

                if ((session_is_fresh || needs_key_transport) && account && buffer)
                {
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

    XDEBUG("omemo: MAM catchup complete \xe2\x80\x94 sending {} deferred key-transport(s)",
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

// Publish a single OMEMO:2 + legacy axolotl bundle pair if the republish was
// deferred during global MAM catchup (bundle_republish_pending == true).
// Called immediately after process_postponed_key_transports() at every
// MAM <fin> flush point in iq_handler.inl.
void weechat::xmpp::omemo::process_postponed_bundle_republish(weechat::account &account)
{
    if (!bundle_republish_pending)
        return;

    bundle_republish_pending = false;

    XDEBUG("omemo: MAM catchup complete \xe2\x80\x94 sending deferred bundle republish");

    struct t_gui_buffer *buf = account.buffer;

    if (std::shared_ptr<xmpp_stanza_t> lbs {
            get_axolotl_bundle(*account.context, nullptr, nullptr), xmpp_stanza_release })
    {
        account.connection.send(lbs.get());
        print_info(buf, "OMEMO: republished bundle after MAM catchup (deferred consumed pre-key replacement)");
    }
}
