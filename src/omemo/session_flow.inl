static struct t_gui_buffer *key_transport_buffer_for_peer(weechat::account &account,
                                                        struct t_gui_buffer *fallback,
                                                        std::string_view peer_bare_jid);

static void resolve_key_transport_route(weechat::account &account,
                                      struct t_gui_buffer *buffer,
                                      std::string_view peer_bare_jid,
                                      std::string &dest,
                                      std::string &msg_type);

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
        failed_session_bootstrap.erase({std::string(bare_jid), remote_device_id});

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
            struct t_gui_buffer *kt_buf = key_transport_buffer_for_peer(
                account, buffer ? buffer : account.buffer, bare_jid);
            send_key_transport(*this, account, kt_buf, bare_jid.c_str(), remote_device_id);
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
    return peers_with_observed_traffic.contains(bare_jid);
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
        print_error(account ? account->buffer : nullptr,
                    fmt::format("omemo: handle_axolotl_devicelist: invalid args (jid={})",
                                jid ? jid : "(null)"));
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
        const ::xmpp::StanzaView items_view(items);
        const ::xmpp::StanzaView list = items_view.child("item").child("list", kLegacyOmemoNs);

        for (const ::xmpp::StanzaView device : list)
        {
            if (!stanza_attr_iequals(device.name(), "device"))
                continue;

            const auto parsed_device_id = parse_uint32(device.attr_string("id"));
            if (!parsed_device_id || !is_valid_omemo_device_id(*parsed_device_id))
                continue;

            devices.emplace_back(fmt::format("{}", *parsed_device_id));
        }
    }

    // Multi-client PEP is often incomplete: one client (e.g. Converse) republishes
    // a list that omits another (e.g. Psi+). A full replace would drop devices we
    // already have a Signal session with (learned from inbound sid), so outbound
    // encrypt would no longer include a key for that client — Psi→Xepher works,
    // Xepher→Psi fails. Union PEP devices with any previous device that still has
    // a live session.
    if (auto prev = load_axolotl_devicelist(*this, bare_jid))
    {
        for (const auto &dev : split(*prev, ';'))
        {
            const auto id = parse_uint32(dev);
            if (!id || !is_valid_omemo_device_id(*id))
                continue;
            const std::string id_str = fmt::format("{}", *id);
            if (std::ranges::find(devices, id_str) != devices.end())
                continue;
            if (has_session(bare_jid.c_str(), *id))
            {
                devices.push_back(id_str);
                XDEBUG("omemo: keeping {}/{} (live session) missing from PEP list",
                       bare_jid, *id);
            }
        }
    }

    const auto devicelist_str = join(devices, ";");
    missing_axolotl_devicelist.erase(bare_jid);
    store_axolotl_devicelist(*this, bare_jid, devicelist_str);

    // Sibling devices on our own account must be BLIND-trusted so encode includes
    // keys for carbon-copy delivery to other clients (BTBV UNDECIDED would skip them).
    if (account)
    {
        std::string account_bare = ::jid(nullptr, account->jid().data()).bare;
        if (account_bare.empty())
            account_bare = account->jid();
        if (bare_jid == account_bare)
        {
            for (const auto &dev : devices)
            {
                const auto sibling_id = parse_uint32(dev);
                if (!sibling_id || !is_valid_omemo_device_id(*sibling_id)
                    || *sibling_id == device_id)
                {
                    continue;
                }
                const auto trust = load_tofu_trust(*this, bare_jid, *sibling_id);
                if (!trust || *trust == omemo_trust::UNDECIDED)
                    store_tofu_trust(*this, bare_jid, *sibling_id, omemo_trust::BLIND);
            }
        }
    }

    XDEBUG("omemo: handle_axolotl_devicelist for {}: storing {} device(s) [{}]",
           bare_jid,
           devices.size(),
           devicelist_str.empty() ? "(empty)" : devicelist_str);

    if (!account)
        return;

    // XEP-0384 §5.8.2: prefetch bundles for MUC OMEMO recipients.
    if (account->omemo_muc_occupant_in_eligible_room(bare_jid))
    {
        note_peer_traffic(account->context, bare_jid);
        for (const auto &dev : devices)
        {
            const auto remote_device_id = parse_uint32(dev);
            if (!remote_device_id || !is_valid_omemo_device_id(*remote_device_id))
                continue;

            for (auto &[_, ch] : account->channels)
            {
                if (ch.type == weechat::channel::chat_type::MUC
                    && ch.omemo_recipient_jids.contains(bare_jid))
                {
                    ch.mark_omemo_bundle_pending(bare_jid, *remote_device_id);
                }
            }
            request_axolotl_bundle(*account, bare_jid, *remote_device_id);
        }
    }

    if (auto ch_it = account->channels.find(bare_jid);
        ch_it != account->channels.end())
    {
        auto &[_, ch] = *ch_it;
        if (ch.type == weechat::channel::chat_type::PM
            && !ch.pending_omemo_messages.empty())
        {
            const bool has_any_session = std::ranges::any_of(devices, [&](const auto &dev) {
                const auto remote_device_id = parse_uint32(dev);
                return remote_device_id
                    && is_valid_omemo_device_id(*remote_device_id)
                    && has_session(bare_jid.c_str(), *remote_device_id);
            });
            if (has_any_session)
                ch.flush_pending_omemo_messages();
        }
    }
}

// Pick the buffer whose channel context should govern key-transport routing
// when the caller only has the account buffer (e.g. bundle IQ handler).
static struct t_gui_buffer *key_transport_buffer_for_peer(weechat::account &account,
                                                        struct t_gui_buffer *fallback,
                                                        std::string_view peer_bare_jid)
{
    if (peer_bare_jid.empty())
        return fallback;

    const std::string normalized = ::jid(nullptr, std::string(peer_bare_jid)).bare;
    const std::string_view peer = normalized.empty() ? peer_bare_jid : std::string_view(normalized);

    if (auto pm_it = account.channels.find(std::string(peer));
        pm_it != account.channels.end()
        && pm_it->second.type == weechat::channel::chat_type::PM)
    {
        return pm_it->second.buffer;
    }

    for (auto &[_, ch] : account.channels)
    {
        if (ch.type == weechat::channel::chat_type::MUC
            && ch.omemo_recipient_jids.contains(std::string(peer)))
        {
            return ch.buffer;
        }
    }

    return fallback;
}

// PM buffer context always wins over shared MUC membership: a contact may be in
// a room with us while we recover a 1:1 session from their PM buffer.
static void resolve_key_transport_route(weechat::account &account,
                                      struct t_gui_buffer *buffer,
                                      std::string_view peer_bare_jid,
                                      std::string &dest,
                                      std::string &msg_type)
{
    const std::string normalized = ::jid(nullptr, std::string(peer_bare_jid)).bare;
    const std::string peer = normalized.empty() ? std::string(peer_bare_jid) : normalized;

    dest = peer;
    msg_type = "chat";

    const weechat::channel *ctx_channel = nullptr;
    if (buffer)
    {
        for (const auto &[_, ch] : account.channels)
        {
            if (ch.buffer == buffer)
            {
                ctx_channel = &ch;
                break;
            }
        }
    }

    if (ctx_channel)
    {
        if (ctx_channel->type == weechat::channel::chat_type::MUC)
        {
            dest = ctx_channel->id;
            msg_type = "groupchat";
        }
        return;
    }

    // Account buffer or unknown buffer: prefer an open PM channel for this peer.
    if (auto pm_it = account.channels.find(peer);
        pm_it != account.channels.end()
        && pm_it->second.type == weechat::channel::chat_type::PM)
    {
        return;
    }

    for (const auto &[room_id, ch] : account.channels)
    {
        if (ch.type == weechat::channel::chat_type::MUC
            && ch.omemo_recipient_jids.contains(peer))
        {
            dest = room_id;
            msg_type = "groupchat";
            return;
        }
    }
}

static bool key_transport_routes_as_groupchat(weechat::account &account,
                                              struct t_gui_buffer *buffer,
                                              std::string_view peer_bare_jid)
{
    std::string dest;
    std::string msg_type;
    resolve_key_transport_route(account, buffer, peer_bare_jid, dest, msg_type);
    return msg_type == "groupchat";
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

    const auto encoded_iv = base64_encode(*account.context, ep.iv);

    const std::string own_bare_jid = [&]{
        auto b = ::jid(nullptr, account.jid().data()).bare;
        return b.empty() ? std::string(account.jid()) : b;
    }();

    std::string dest;
    std::string msg_type;
    resolve_key_transport_route(account, buffer, peer_jid, dest, msg_type);

    stanza::xep0384::axolotl_header header_spec(fmt::format("{}", self.device_id));
    bool any_keys = false;
    bool own_has_keys = false;

    // Legacy flat layout: add <key> directly under <header> (no <keys jid=...> wrapper).
    // This is the format nbxmpp/Gajim and Conversations expect.
    auto add_axolotl_key = [&](std::string_view target_jid,
                               std::uint32_t target_device_id) -> bool
    {
        if (!is_valid_omemo_device_id(target_device_id))
            return false;

        const auto target_transport = encrypt_axolotl_transport_key(self, target_jid, target_device_id, ep);
        if (!target_transport)
            return false;

        const auto& [tkey, is_kex] = *target_transport;
        const auto target_encoded_transport = base64_encode(*account.context, tkey);
        header_spec.add_key(stanza::xep0384::axolotl_key(
            fmt::format("{}", target_device_id),
            target_encoded_transport,
            is_kex));
        return true;
    };

    // docs/planning-muc-omemo.md §3.3: For MUC key transport, encrypt the (zero)
    // payload for every current occupant (multi-recipient), not just the single peer.
    if (msg_type == "groupchat")
    {
        // We are sending to a room — collect all known occupants' real JIDs
        // from the channel we are about to send to.
        if (auto ch_it = account.channels.find(dest); ch_it != account.channels.end())
        {
            auto& [_, ch] = *ch_it;
            if (ch.type == weechat::channel::chat_type::MUC)
            {
                for (const auto& [nick, m] : ch.members)
                {
                    if (m.real_jid && !m.real_jid->empty())
                    {
                        const std::string &occ_jid = *m.real_jid;
                        const auto dl = load_string(self, key_for_axolotl_devicelist(occ_jid));
                        if (dl && !dl->empty())
                        {
                            for (const auto &dev : split(*dl, ';'))
                            {
                                const auto did = parse_uint32(dev);
                                if (did && is_valid_omemo_device_id(*did))
                                {
                                    if (add_axolotl_key(occ_jid, *did))
                                        any_keys = true;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    else
    {
        // Normal 1:1 / PM key transport
        if (add_axolotl_key(peer_jid, remote_device_id))
            any_keys = true;
    }

    const auto own_legacy_devicelist = load_string(self, key_for_axolotl_devicelist(own_bare_jid));
    if (own_legacy_devicelist && !own_legacy_devicelist->empty())
    {
        for (const auto &dev : split(*own_legacy_devicelist, ';'))
        {
            const auto own_device = parse_uint32(dev);
            if (!own_device || *own_device == self.device_id)
                continue;

            if (self.failed_session_bootstrap.contains({std::string(own_bare_jid), *own_device}))
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

    if (!any_keys && !own_has_keys)
    {
        XDEBUG("OMEMO: key-transport encrypt failed for {}/{}",
               peer_jid, remote_device_id);
        return;
    }

    header_spec.add_iv(stanza::xep0384::axolotl_iv(encoded_iv));

    stanza::xep0384::axolotl_encrypted enc_spec;
    enc_spec.add_header(header_spec);

    XDEBUG("OMEMO: sending key-transport to {}/{}",
           peer_jid, remote_device_id);
    auto msg_sp = stanza::message()
        .type(msg_type)
        .to(dest)
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

    pending_bundle_fetch.erase({std::string(jid ? jid : ""), remote_device_id});
    const bool needs_key_transport = pending_key_transport.contains({std::string(jid ? jid : ""), remote_device_id});
    pending_key_transport.erase({std::string(jid ? jid : ""), remote_device_id});

    if (db_env && !bare_jid.empty())
    {
        if (const auto bundle = extract_legacy_bundle_from_items(items))
        {
            // Store under the legacy key prefix so we know it came from the
            // axolotl namespace bundle for session bootstrap.
            store_string(*this, key_for_axolotl_bundle(bare_jid, remote_device_id),
                         serialize_bundle(*bundle));
            // Also store under the canonical bundle key so establish_session_from_bundle()
            // can find it without knowing which namespace it came from.
            store_bundle(*this, bare_jid, remote_device_id, *bundle);

            if (!is_own_device)
            {
                failed_session_bootstrap.erase({std::string(bare_jid), remote_device_id});
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
                        failed_session_bootstrap.insert({std::string(bare_jid), remote_device_id});
                }
                const bool session_is_fresh = !had_session_before
                    && has_session(bare_jid.c_str(), remote_device_id);

                if ((session_is_fresh || needs_key_transport) && account)
                {
                    // MUC OMEMO: passive bundle prefetch must not blast groupchat
                    // key-transport stanzas into the room (issue #6). Only send when
                    // explicitly queued (decode recovery, force-kex).
                    const bool passive_muc_prefetch = session_is_fresh
                        && !needs_key_transport
                        && key_transport_routes_as_groupchat(*account, buffer, bare_jid);
                    if (passive_muc_prefetch)
                    {
                        XDEBUG("omemo: skipping passive key-transport for MUC occupant {}/{}",
                               bare_jid, remote_device_id);
                    }
                    else if (global_mam_catchup)
                    {
                        postponed_key_transports.insert({bare_jid, remote_device_id});
                    }
                    else
                    {
                        struct t_gui_buffer *kt_buf = key_transport_buffer_for_peer(
                            *account, buffer, bare_jid);
                        send_key_transport(*this, *account, kt_buf, bare_jid.c_str(), remote_device_id);
                    }
                }
            }

            if (!bare_jid.empty() && !is_own_device)
            {
                const auto &lpks = bundle->prekeys;
                XDEBUG("OMEMO (legacy) received bundle for {}/{} ({} prekeys).",
                       bare_jid, remote_device_id, lpks.size());
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

    // MUC OMEMO: bundle fetch completed (success, parse failure, or empty).
    if (account && !bare_jid.empty() && !is_own_device)
    {
        for (auto &[_, ch] : account->channels)
        {
            if (ch.type == weechat::channel::chat_type::MUC
                && ch.omemo_recipient_jids.contains(bare_jid))
            {
                ch.clear_omemo_bundle_pending(bare_jid, remote_device_id);
            }
        }
    }

    // After successfully building a session, flush queued PM messages.
    if (!is_own_device
        && account
        && !bare_jid.empty()
        && has_session(bare_jid.c_str(), remote_device_id))
    {
        if (auto ch_it = account->channels.find(bare_jid); ch_it != account->channels.end())
        {
            auto& [_, ch] = *ch_it;
            if (ch.type == weechat::channel::chat_type::PM && ch.omemo.enabled)
                ch.flush_pending_omemo_messages();
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
        struct t_gui_buffer *buf = key_transport_buffer_for_peer(
            account, account.buffer, bare_jid);
        send_key_transport(*this, account, buf, bare_jid.c_str(), device_id);
    }
    postponed_key_transports.clear();
}

void weechat::xmpp::omemo::process_deferred_live_key_transports(weechat::account &account)
{
    if (deferred_live_key_transports.empty())
        return;

    const auto pending = std::exchange(deferred_live_key_transports, {});
    for (const auto &[bare_jid, device_id] : pending)
    {
        struct t_gui_buffer *buf = key_transport_buffer_for_peer(
            account, account.buffer, bare_jid);
        send_key_transport(*this, account, buf, bare_jid.c_str(), device_id);
    }
}

// Republish the axolotl bundle if deferred during global MAM catchup
// (bundle_republish_pending == true).
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
        if (send_within_stanza_byte_limit(
                account.connection, lbs.get(),
                k_proxy_safe_stanza_bytes, "OMEMO bundle republish"))
            print_info(buf, "OMEMO: republished bundle after MAM catchup (deferred consumed pre-key replacement)");
    }
}
