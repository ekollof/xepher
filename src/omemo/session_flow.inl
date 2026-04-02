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
    const bool in_axolotl_list = devicelist_contains_device(self, key_for_axolotl_devicelist(bare_jid), device_id);

    if (in_omemo2_list && !in_axolotl_list)
        return omemo::peer_mode::omemo2;
    if (in_axolotl_list && !in_omemo2_list)
        return omemo::peer_mode::axolotl;

    if (preferred != omemo::peer_mode::unknown)
        return preferred;

    if (stored_mode)
        return *stored_mode;

    if (account.peer_has_legacy_axolotl_only(bare_jid))
        return omemo::peer_mode::axolotl;

    if (in_omemo2_list)
        return omemo::peer_mode::omemo2;
    if (in_axolotl_list)
        return omemo::peer_mode::axolotl;

    return omemo::peer_mode::unknown;
}

}

static void send_key_transport(omemo &self,
                               weechat::account &account,
                               struct t_gui_buffer *buffer,
                               const char *peer_jid,
                               std::uint32_t remote_device_id);

// XEP-0450 §4 + XEP-0420 §4.3: encrypt a <trust-message> inside an SCE
// envelope using OMEMO:2, then send it to every device in recipient_devicelist
// (semicolon-separated device IDs for recipient_jid).  The <encrypted> stanza
// is wrapped in a <message type='chat' to='recipient_jid'>.
// Returns true if the stanza was sent to at least one device.
static bool send_atm_trust_message_omemo2(
    omemo &self,
    weechat::account &account,
    const std::string &trust_message_xml,  // pre-serialised <trust-message>...</trust-message>
    const std::string &recipient_jid,
    const std::string &recipient_devicelist)
{
    if (!account.context || recipient_jid.empty() || recipient_devicelist.empty())
        return false;

    const std::string own_bare_jid = [&]{
        auto b = ::jid(nullptr, account.jid().data()).bare;
        return b.empty() ? std::string(account.jid()) : b;
    }();

    // SCE-wrap the <trust-message> (no <body>, no MUC <to/>)
    const auto sce_envelope = sce_wrap_content(*account.context, account, trust_message_xml);

    // OMEMO:2 symmetric encryption
    const auto ep = omemo2_encrypt(sce_envelope);
    if (!ep)
        return false;

    auto mk = [&]() {
        return std::shared_ptr<xmpp_stanza_t> { xmpp_stanza_new(*account.context), xmpp_stanza_release };
    };

    xmpp_stanza_t *encrypted = xmpp_stanza_new(*account.context);
    xmpp_stanza_set_name(encrypted, "encrypted");
    xmpp_stanza_set_ns(encrypted, kOmemoNs.data());

    auto header = mk();
    xmpp_stanza_set_name(header.get(), "header");
    xmpp_stanza_set_attribute(header.get(), "sid", fmt::format("{}", self.device_id).c_str());

    // Build <keys jid='recipient_jid'>  one <key rid='…'> per device
    auto keys = mk();
    xmpp_stanza_set_name(keys.get(), "keys");
    xmpp_stanza_set_attribute(keys.get(), "jid", recipient_jid.c_str());
    int key_count = 0;
    for (const auto &dev : split(recipient_devicelist, ';'))
    {
        const auto remote_id = parse_uint32(dev);
        if (!remote_id || !is_valid_omemo_device_id(*remote_id))
            continue;
        // Skip our own current device
        if (own_bare_jid == recipient_jid && *remote_id == self.device_id)
            continue;
        if (self.failed_session_bootstrap.count({recipient_jid, *remote_id}) > 0)
            continue;
        if (!self.has_session(recipient_jid.c_str(), *remote_id))
        {
            if (!establish_session_from_bundle(self, *account.context, recipient_jid, *remote_id))
                continue;
        }
        const auto transport = encrypt_transport_key(self, recipient_jid, *remote_id, *ep);
        if (!transport)
            continue;
        const auto enc_key = base64_encode(*account.context,
                                           transport->first.data(), transport->first.size());
        auto key_st = mk();
        xmpp_stanza_set_name(key_st.get(), "key");
        xmpp_stanza_set_attribute(key_st.get(), "rid", fmt::format("{}", *remote_id).c_str());
        if (transport->second)
            xmpp_stanza_set_attribute(key_st.get(), "kex", "true");
        auto key_text = mk();
        xmpp_stanza_set_text(key_text.get(), enc_key.c_str());
        xmpp_stanza_add_child(key_st.get(), key_text.get());
        xmpp_stanza_add_child(keys.get(), key_st.get());
        ++key_count;
    }
    if (key_count == 0)
    {
        xmpp_stanza_release(encrypted);
        return false;
    }
    xmpp_stanza_add_child(header.get(), keys.get());

    // <payload>
    auto payload = mk();
    xmpp_stanza_set_name(payload.get(), "payload");
    const auto enc_payload = base64_encode(*account.context,
                                           ep->payload.data(), ep->payload.size());
    auto payload_text = mk();
    xmpp_stanza_set_text(payload_text.get(), enc_payload.c_str());
    xmpp_stanza_add_child(payload.get(), payload_text.get());

    xmpp_stanza_add_child(encrypted, header.get());
    xmpp_stanza_add_child(encrypted, payload.get());

    // Wrap in <message type='chat' to='recipient_jid'>
    auto message = mk();
    xmpp_stanza_set_name(message.get(), "message");
    xmpp_stanza_set_type(message.get(), "chat");
    xmpp_stanza_set_attribute(message.get(), "to", recipient_jid.c_str());
    xmpp_stanza_set_id(message.get(), stanza::uuid(*account.context).c_str());
    xmpp_stanza_add_child(message.get(), encrypted);

    auto store_hint = mk();
    xmpp_stanza_set_name(store_hint.get(), "store");
    xmpp_stanza_set_ns(store_hint.get(), "urn:xmpp:hints");
    xmpp_stanza_add_child(message.get(), store_hint.get());

    account.connection.send(message.get());
    xmpp_stanza_release(encrypted);
    return true;
}

// XEP-0450 §4: send ATM trust message using legacy OMEMO encryption
// (eu.siacs.conversations.axolotl namespace, AES-128-GCM, no SCE).
// NOTE: legacy OMEMO has no SCE layer — the <trust-message> is the plaintext.
static bool send_atm_trust_message_axolotl(
    omemo &self,
    weechat::account &account,
    const std::string &trust_message_xml,
    const std::string &recipient_jid,
    const std::string &recipient_devicelist)
{
    if (!account.context || recipient_jid.empty() || recipient_devicelist.empty())
        return false;

    const std::string own_bare_jid = [&]{
        auto b = ::jid(nullptr, account.jid().data()).bare;
        return b.empty() ? std::string(account.jid()) : b;
    }();

    // Legacy encrypts the raw plaintext (no SCE wrapper)
    const auto ep = axolotl_omemo_encrypt(std::string_view(trust_message_xml));
    if (!ep)
        return false;

    auto mk = [&]() {
        return std::shared_ptr<xmpp_stanza_t> { xmpp_stanza_new(*account.context), xmpp_stanza_release };
    };

    xmpp_stanza_t *encrypted = xmpp_stanza_new(*account.context);
    xmpp_stanza_set_name(encrypted, "encrypted");
    xmpp_stanza_set_ns(encrypted, kLegacyOmemoNs.data());

    auto header = mk();
    xmpp_stanza_set_name(header.get(), "header");
    xmpp_stanza_set_attribute(header.get(), "sid", fmt::format("{}", self.device_id).c_str());

    auto keys = mk();
    xmpp_stanza_set_name(keys.get(), "keys");
    xmpp_stanza_set_attribute(keys.get(), "jid", recipient_jid.c_str());
    int key_count = 0;
    for (const auto &dev : split(recipient_devicelist, ';'))
    {
        const auto remote_id = parse_uint32(dev);
        if (!remote_id || !is_valid_omemo_device_id(*remote_id))
            continue;
        if (own_bare_jid == recipient_jid && *remote_id == self.device_id)
            continue;
        if (self.failed_session_bootstrap.count({recipient_jid, *remote_id}) > 0)
            continue;
        if (!self.has_session(recipient_jid.c_str(), *remote_id))
        {
            if (!establish_session_from_bundle(self, *account.context, recipient_jid, *remote_id))
                continue;
        }
        const auto transport = encrypt_axolotl_transport_key(self, recipient_jid, *remote_id, *ep);
        if (!transport)
            continue;
        const auto enc_key = base64_encode(*account.context,
                                           transport->first.data(), transport->first.size());
        auto key_st = mk();
        xmpp_stanza_set_name(key_st.get(), "key");
        xmpp_stanza_set_attribute(key_st.get(), "rid", fmt::format("{}", *remote_id).c_str());
        if (transport->second)
            xmpp_stanza_set_attribute(key_st.get(), "prekey", "true");
        auto key_text = mk();
        xmpp_stanza_set_text(key_text.get(), enc_key.c_str());
        xmpp_stanza_add_child(key_st.get(), key_text.get());
        xmpp_stanza_add_child(keys.get(), key_st.get());
        ++key_count;
    }
    if (key_count == 0)
    {
        xmpp_stanza_release(encrypted);
        return false;
    }
    xmpp_stanza_add_child(header.get(), keys.get());

    const auto enc_iv = base64_encode(*account.context, ep->iv.data(), ep->iv.size());
    auto iv_st = mk();
    xmpp_stanza_set_name(iv_st.get(), "iv");
    auto iv_text = mk();
    xmpp_stanza_set_text(iv_text.get(), enc_iv.c_str());
    xmpp_stanza_add_child(iv_st.get(), iv_text.get());
    xmpp_stanza_add_child(header.get(), iv_st.get());

    xmpp_stanza_add_child(encrypted, header.get());

    const auto enc_payload = base64_encode(*account.context, ep->payload.data(), ep->payload.size());
    auto payload = mk();
    xmpp_stanza_set_name(payload.get(), "payload");
    auto payload_text = mk();
    xmpp_stanza_set_text(payload_text.get(), enc_payload.c_str());
    xmpp_stanza_add_child(payload.get(), payload_text.get());
    xmpp_stanza_add_child(encrypted, payload.get());

    auto message = mk();
    xmpp_stanza_set_name(message.get(), "message");
    xmpp_stanza_set_type(message.get(), "chat");
    xmpp_stanza_set_attribute(message.get(), "to", recipient_jid.c_str());
    xmpp_stanza_set_id(message.get(), stanza::uuid(*account.context).c_str());
    xmpp_stanza_add_child(message.get(), encrypted);

    auto store_hint = mk();
    xmpp_stanza_set_name(store_hint.get(), "store");
    xmpp_stanza_set_ns(store_hint.get(), "urn:xmpp:hints");
    xmpp_stanza_add_child(message.get(), store_hint.get());

    account.connection.send(message.get());
    xmpp_stanza_release(encrypted);
    return true;
}

// XEP-0434 §4: build a batched <trust-message> XML string from a list of
// (key_owner_jid, fingerprint_b64) pairs.  Entries with the same JID are
// grouped under a single <key-owner> element as the spec example shows.
// tag_name is "trust" or "distrust".  Returns empty string if pairs is empty.
static std::string build_trust_message_xml(
    std::string_view encryption_ns,
    std::string_view tag_name,
    const std::vector<std::pair<std::string,std::string>> &pairs)
{
    if (pairs.empty())
        return {};

    // Group fingerprints by key-owner JID (preserving first-seen order).
    std::vector<std::string> jid_order;
    std::unordered_map<std::string, std::vector<std::string>> by_jid;
    for (const auto &[jid, fp] : pairs)
    {
        if (by_jid.find(jid) == by_jid.end())
            jid_order.push_back(jid);
        by_jid[jid].push_back(fp);
    }

    std::string body;
    for (const auto &jid : jid_order)
    {
        body += fmt::format("<key-owner jid='{}'>", xml_escape(jid));
        for (const auto &fp : by_jid[jid])
            body += fmt::format("<{}>{}</{}>", tag_name, xml_escape(fp), tag_name);
        body += "</key-owner>";
    }

    return fmt::format(
        "<trust-message xmlns='urn:xmpp:tm:1' usage='urn:xmpp:atm:1'"
        " encryption='{}'>{}</trust-message>",
        encryption_ns, body);
}

// XEP-0450 §4: send trust messages for a batch of (key_owner_jid, fingerprint)
// pairs. Sends one encrypted message per namespace (OMEMO:2 and legacy) to:
//   - All own devices (so other clients learn the decision)
//   - All of each key owner's devices (XEP-0450 §4.1.1.2)
// All key owners' devices that share the same target JID receive a single
// stanza containing all <key-owner> elements for that JID (XEP-0434 §4 batching).
static void send_atm_trust_message(omemo &self,
                                   weechat::account &account,
                                   const std::vector<std::pair<std::string,std::string>> &pairs)
{
    if (!account.context || pairs.empty())
        return;

    const std::string own_bare_jid = [&]{
        auto b = ::jid(nullptr, account.jid().data()).bare;
        return b.empty() ? std::string(account.jid()) : b;
    }();

    const auto tm_omemo2  = build_trust_message_xml("urn:xmpp:omemo:2", "trust", pairs);
    const auto tm_legacy  = build_trust_message_xml("eu.siacs.conversations.axolotl", "trust", pairs);

    // Collect the set of target JIDs: own + all key-owner JIDs.
    std::set<std::string> targets;
    targets.insert(own_bare_jid);
    for (const auto &[jid, _] : pairs)
        targets.insert(jid);

    for (const auto &target_jid : targets)
    {
        const auto dl2 = load_string(self, key_for_devicelist(target_jid));
        if (dl2 && !dl2->empty())
            send_atm_trust_message_omemo2(self, account, tm_omemo2, target_jid, *dl2);

        const auto dll = load_string(self, key_for_axolotl_devicelist(target_jid));
        if (dll && !dll->empty())
            send_atm_trust_message_axolotl(self, account, tm_legacy, target_jid, *dll);
    }
}

// Convenience overload for the common single-key case.
static void send_atm_trust_message(omemo &self,
                                   weechat::account &account,
                                   const std::string &key_owner_jid,
                                   const std::string &key_b64)
{
    send_atm_trust_message(self, account,
        std::vector<std::pair<std::string,std::string>>{{key_owner_jid, key_b64}});
}

// XEP-0450 §4.2: send <distrust> messages for a batch of (key_owner_jid, fp) pairs.
// Mirrors send_atm_trust_message but uses the <distrust> element.
static void send_atm_distrust_message(omemo &self,
                                      weechat::account &account,
                                      const std::vector<std::pair<std::string,std::string>> &pairs)
{
    if (!account.context || pairs.empty())
        return;

    const std::string own_bare_jid = [&]{
        auto b = ::jid(nullptr, account.jid().data()).bare;
        return b.empty() ? std::string(account.jid()) : b;
    }();

    const auto tm_omemo2 = build_trust_message_xml("urn:xmpp:omemo:2", "distrust", pairs);
    const auto tm_legacy = build_trust_message_xml("eu.siacs.conversations.axolotl", "distrust", pairs);

    std::set<std::string> targets;
    targets.insert(own_bare_jid);
    for (const auto &[jid, _] : pairs)
        targets.insert(jid);

    for (const auto &target_jid : targets)
    {
        const auto dl2 = load_string(self, key_for_devicelist(target_jid));
        if (dl2 && !dl2->empty())
            send_atm_trust_message_omemo2(self, account, tm_omemo2, target_jid, *dl2);

        const auto dll = load_string(self, key_for_axolotl_devicelist(target_jid));
        if (dll && !dll->empty())
            send_atm_trust_message_axolotl(self, account, tm_legacy, target_jid, *dll);
    }
}

// Convenience overload for the common single-key distrust case (unused externally,
// kept for potential future callers).
[[maybe_unused]]
static void send_atm_distrust_message(omemo &self,
                                      weechat::account &account,
                                      const std::string &key_owner_jid,
                                      const std::string &key_b64)
{
    send_atm_distrust_message(self, account,
        std::vector<std::pair<std::string,std::string>>{{key_owner_jid, key_b64}});
}

// XEP-0450 §5.1: drain any trust decisions that were deferred while sender_jid
// was not yet ATM-trusted.  Called after sender_jid's first device becomes
// trusted (i.e. right after store_atm_trust() records a "trusted" entry).
static void drain_pending_atm_trust(omemo &self, const std::string &sender_jid)
{
    auto it = self.pending_atm_trust_from_unauthenticated.find(sender_jid);
    if (it == self.pending_atm_trust_from_unauthenticated.end())
        return;

    for (const auto &[ko_jid, fp, level] : it->second)
        store_atm_trust(self, ko_jid, fp, level);

    self.pending_atm_trust_from_unauthenticated.erase(it);
}

void weechat::xmpp::omemo::request_devicelist(weechat::account &account, std::string_view jid)
{
    const std::string bare_jid = normalize_bare_jid(account.context, jid);
    // Probe both namespaces up front. Device support is resolved per-device
    // during bundle bootstrap rather than assumed peer-wide.
    ::request_devicelist(account, bare_jid);
    ::request_axolotl_devicelist(account, bare_jid);
}

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

    collect_devices(load_string(*this, key_for_devicelist(bare_jid)));
    collect_devices(load_string(*this, key_for_axolotl_devicelist(bare_jid)));

    for (const auto known_device_id : known_devices)
    {
        request_bundle(account, bare_jid, known_device_id);
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

        collect_devices(load_string(*this, key_for_devicelist(bare_jid)));
        collect_devices(load_string(*this, key_for_axolotl_devicelist(bare_jid)));
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
        request_axolotl_bundle(account, bare_jid, remote_device_id);
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
    const auto axolotl_list = load_string(*this, key_for_axolotl_devicelist(bare_jid));

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
    collect_devices(axolotl_list);

    std::size_t known_omemo2_devices = 0;
    std::size_t known_axolotl_devices = 0;
    for (const auto device_id : all_devices)
    {
        const auto resolved_mode = resolve_device_mode(*this, account, bare_jid, device_id);
        if (resolved_mode == peer_mode::omemo2)
            ++known_omemo2_devices;
        else if (resolved_mode == peer_mode::axolotl)
            ++known_axolotl_devices;
    }

    if (known_omemo2_devices != 0 && known_axolotl_devices == 0)
        return peer_mode::omemo2;
    if (known_axolotl_devices != 0 && known_omemo2_devices == 0)
        return peer_mode::axolotl;

    const bool has_omemo2 = omemo2_list && !omemo2_list->empty();
    const bool has_axolotl = axolotl_list && !axolotl_list->empty();

    if (has_omemo2 && !has_axolotl)
        return peer_mode::omemo2;
    if (has_axolotl && !has_omemo2)
        return peer_mode::axolotl;
    if (has_omemo2 && has_axolotl)
    {
        if (account.peer_has_legacy_axolotl_only(bare_jid))
            return peer_mode::axolotl;
        return peer_mode::omemo2;
    }

    if (account.peer_has_legacy_axolotl_only(bare_jid))
        return peer_mode::axolotl;

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
        auto b = ::jid(nullptr, jid).bare;
        if (!b.empty())
            bare_jid = std::move(b);
    }

    const auto devices = extract_devices_from_items(items);
    const auto devicelist_str = join(devices, ";");

    missing_omemo2_devicelist.erase(bare_jid);
    
    XDEBUG("omemo: handle_devicelist for {}: storing {} device(s) [{}]",
           bare_jid,
           devices.size(),
           devicelist_str.empty() ? "(empty)" : devicelist_str);
    
    store_string(*this, key_for_devicelist(bare_jid), devicelist_str);
    for (const auto &dev : devices)
    {
        const auto remote_device_id = parse_uint32(dev);
        if (!remote_device_id || !is_valid_omemo_device_id(*remote_device_id))
            continue;
        store_device_mode(*this, bare_jid, *remote_device_id, peer_mode::omemo2);
    }

    // XEP-0384 §6: "If a client receives a device list update and its own
    // device ID is not present in the list, it MUST re-announce its device."
    // Only check when the update is for our own bare JID and we have a valid
    // device_id so we don't spuriously republish for other contacts' lists.
    if (account && is_valid_omemo_device_id(device_id))
    {
        const std::string own_bare_jid = [&]{
            auto b = ::jid(nullptr, account->jid().data()).bare;
            return b.empty() ? std::string(account->jid()) : b;
        }();

        if (bare_jid == own_bare_jid)
        {
            const bool self_present = std::any_of(devices.begin(), devices.end(),
                [&](const std::string &dev) {
                    const auto id = parse_uint32(dev);
                    return id && *id == device_id;
                });

            if (!self_present)
            {
                XDEBUG("omemo: own device_id {} absent from received devicelist — republishing",
                       device_id);
                weechat_printf(nullptr,
                               "%somemo: own device %u missing from server device list — re-announcing",
                               weechat_prefix("network"), device_id);

                if (auto dl = account->get_devicelist())
                    account->connection.send(dl.get());
                if (auto bundle = get_bundle(*account->context, nullptr, nullptr))
                {
                    auto bs = std::shared_ptr<xmpp_stanza_t> { bundle, xmpp_stanza_release };
                    account->connection.send(bs.get());
                }
            }
        }
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
    for (const auto &dev : devices)
    {
        const auto remote_device_id = parse_uint32(dev);
        if (!remote_device_id || !is_valid_omemo_device_id(*remote_device_id))
            continue;
        store_device_mode(*this, bare_jid, *remote_device_id, peer_mode::axolotl);
    }

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
    auto mk_o2 = [&]() {
        return std::shared_ptr<xmpp_stanza_t> { xmpp_stanza_new(*account.context), xmpp_stanza_release };
    };

    auto encrypted = mk_o2();
    xmpp_stanza_set_name(encrypted.get(), "encrypted");
    xmpp_stanza_set_ns(encrypted.get(), kOmemoNs.data());

    // <header sid='our_device_id'>
    auto header = mk_o2();
    xmpp_stanza_set_name(header.get(), "header");
    xmpp_stanza_set_attribute(header.get(), "sid", fmt::format("{}", self.device_id).c_str());

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

        auto keys = mk_o2();
        xmpp_stanza_set_name(keys.get(), "keys");
        xmpp_stanza_set_attribute(keys.get(), "jid", target_jid.c_str());

        auto key_elem = mk_o2();
        xmpp_stanza_set_name(key_elem.get(), "key");
        xmpp_stanza_set_attribute(key_elem.get(), "rid", fmt::format("{}", target_device_id).c_str());
        if (target_transport->second)
            xmpp_stanza_set_attribute(key_elem.get(), "kex", "true");

        auto key_text = mk_o2();
        xmpp_stanza_set_text(key_text.get(), target_encoded_transport.c_str());
        xmpp_stanza_add_child(key_elem.get(), key_text.get());

        xmpp_stanza_add_child(keys.get(), key_elem.get());
        xmpp_stanza_add_child(header.get(), keys.get());
        return true;
    };

    // Recipient-side key transport target.
    bool added_any_key = add_omemo2_key(peer_jid, remote_device_id);

    // Also include our other own devices so carbon-copied stanzas can be
    // decrypted on sibling clients (e.g., Gajim), avoiding spurious warnings.
    const std::string own_bare_jid = [&]{
        auto b = ::jid(nullptr, account.jid().data()).bare;
        return b.empty() ? std::string(account.jid()) : b;
    }();
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
        print_error(buffer, fmt::format(
            "OMEMO: key-transport encrypt failed for {}/{}",
            peer_jid, remote_device_id));
        return;
    }

    xmpp_stanza_add_child(encrypted.get(), header.get());

    // Wrap in a <message type='chat' to='peer_jid'>
    auto message_o2 = mk_o2();
    xmpp_stanza_set_name(message_o2.get(), "message");
    xmpp_stanza_set_type(message_o2.get(), "chat");
    xmpp_stanza_set_attribute(message_o2.get(), "to", peer_jid);
    xmpp_stanza_set_id(message_o2.get(), stanza::uuid(*account.context).c_str());
    xmpp_stanza_add_child(message_o2.get(), encrypted.get());

    // Add <store xmlns='urn:xmpp:hints'/> so the server archives it and
    // Conversations can pick up our session even if it's offline.
    auto store_hint_o2 = mk_o2();
    xmpp_stanza_set_name(store_hint_o2.get(), "store");
    xmpp_stanza_set_ns(store_hint_o2.get(), "urn:xmpp:hints");
    xmpp_stanza_add_child(message_o2.get(), store_hint_o2.get());

    print_info(buffer, fmt::format(
        "OMEMO: sending key-transport to {}/{} (kex={})",
        peer_jid, remote_device_id, "mixed"));

    account.connection.send(message_o2.get());
}

static void send_axolotl_key_transport(omemo &self,
                                      weechat::account &account,
                                      struct t_gui_buffer *buffer,
                                      const char *peer_jid,
                                      std::uint32_t remote_device_id)
{
    if (!self || !peer_jid)
        return;

    const auto ep = axolotl_omemo_encrypt(std::string_view("\x00", 1));
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

    auto mk_leg2 = [&]() {
        return std::shared_ptr<xmpp_stanza_t> { xmpp_stanza_new(*account.context), xmpp_stanza_release };
    };

    auto encrypted_leg = mk_leg2();
    xmpp_stanza_set_name(encrypted_leg.get(), "encrypted");
    xmpp_stanza_set_ns(encrypted_leg.get(), kLegacyOmemoNs.data());

    auto header_leg = mk_leg2();
    xmpp_stanza_set_name(header_leg.get(), "header");
    xmpp_stanza_set_attribute(header_leg.get(), "sid", fmt::format("{}", self.device_id).c_str());

    auto add_axolotl_key = [&](const std::string &target_jid,
                              std::uint32_t target_device_id) -> bool
    {
        if (!is_valid_omemo_device_id(target_device_id))
            return false;

        const auto target_transport = encrypt_axolotl_transport_key(self, target_jid, target_device_id, *ep);
        if (!target_transport)
            return false;

        const auto target_encoded_transport = base64_encode(*account.context,
                                                            target_transport->first.data(),
                                                            target_transport->first.size());

        auto key_elem = mk_leg2();
        xmpp_stanza_set_name(key_elem.get(), "key");
        xmpp_stanza_set_attribute(key_elem.get(), "rid", fmt::format("{}", target_device_id).c_str());
        if (target_transport->second)
            xmpp_stanza_set_attribute(key_elem.get(), "prekey", "true");

        auto key_text = mk_leg2();
        xmpp_stanza_set_text(key_text.get(), target_encoded_transport.c_str());
        xmpp_stanza_add_child(key_elem.get(), key_text.get());
        xmpp_stanza_add_child(header_leg.get(), key_elem.get());
        return true;
    };

    bool added_any_key = add_axolotl_key(peer_jid, remote_device_id);

    const std::string own_bare_jid = [&]{
        auto b = ::jid(nullptr, account.jid().data()).bare;
        return b.empty() ? std::string(account.jid()) : b;
    }();

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
                request_axolotl_bundle(account, own_bare_jid, *own_device);
                continue;
            }

            if (add_axolotl_key(own_bare_jid, *own_device))
                added_any_key = true;
        }
    };

    const auto own_legacy_devicelist = load_string(self, key_for_axolotl_devicelist(own_bare_jid));
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
        print_error(buffer, fmt::format(
            "OMEMO (legacy): key-transport encrypt failed for {}/{}",
            peer_jid, remote_device_id));
        return;
    }

    auto iv_stanza_leg = mk_leg2();
    xmpp_stanza_set_name(iv_stanza_leg.get(), "iv");
    auto iv_text_leg = mk_leg2();
    xmpp_stanza_set_text(iv_text_leg.get(), encoded_iv.c_str());
    xmpp_stanza_add_child(iv_stanza_leg.get(), iv_text_leg.get());
    xmpp_stanza_add_child(header_leg.get(), iv_stanza_leg.get());

    xmpp_stanza_add_child(encrypted_leg.get(), header_leg.get());

    auto message_leg = mk_leg2();
    xmpp_stanza_set_name(message_leg.get(), "message");
    xmpp_stanza_set_type(message_leg.get(), "chat");
    xmpp_stanza_set_attribute(message_leg.get(), "to", peer_jid);
    xmpp_stanza_set_id(message_leg.get(), stanza::uuid(*account.context).c_str());
    xmpp_stanza_add_child(message_leg.get(), encrypted_leg.get());

    auto store_hint_leg = mk_leg2();
    xmpp_stanza_set_name(store_hint_leg.get(), "store");
    xmpp_stanza_set_ns(store_hint_leg.get(), "urn:xmpp:hints");
    xmpp_stanza_add_child(message_leg.get(), store_hint_leg.get());

    print_info(buffer, fmt::format(
        "OMEMO (legacy): sending key-transport to {}/{} (prekey={})",
        peer_jid, remote_device_id, "mixed"));

    account.connection.send(message_leg.get());
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

    if (mode == omemo::peer_mode::axolotl)
    {
        send_axolotl_key_transport(self, account, buffer, peer_jid, remote_device_id);
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

                // XEP-0450: on first session establishment, record ATM trust and
                // propagate to own endpoints via an unencrypted trust message.
                if (session_is_fresh && account
                    && weechat::config::instance
                    && weechat_config_boolean(weechat::config::instance->look.omemo_atm))
                {
                    // Load the identity key stored by libsignal for this device.
                    const auto ik_bytes = load_bytes(*this,
                        key_for_identity(bare_jid, static_cast<std::int32_t>(remote_device_id)));
                    if (ik_bytes && !ik_bytes->empty())
                    {
                        const std::string fp = atm_fingerprint_b64(*ik_bytes);
                        if (!fp.empty())
                        {
                            // Only auto-trust if no explicit decision exists yet.
                            const auto existing = load_atm_trust(*this, bare_jid, fp);
                            if (!existing || *existing == "undecided")
                            {
                                store_atm_trust(*this, bare_jid, fp, "trusted");
                                send_atm_trust_message(*this, *account, bare_jid, fp);
                                // §5.1: drain deferred trust from this sender
                                drain_pending_atm_trust(*this, bare_jid);
                            }
                        }
                    }
                }

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
    void weechat::xmpp::omemo::handle_axolotl_bundle(weechat::account *account,
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
                store_device_mode(*this, bare_jid, remote_device_id, peer_mode::axolotl);

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

                    // XEP-0450: on first legacy session establishment, record ATM trust
                    // and propagate to own and peer endpoints via an encrypted trust message.
                    if (session_is_fresh && account
                        && weechat::config::instance
                        && weechat_config_boolean(weechat::config::instance->look.omemo_atm))
                    {
                        const auto ik_bytes = load_bytes(*this,
                            key_for_identity(bare_jid, static_cast<std::int32_t>(remote_device_id)));
                        if (ik_bytes && !ik_bytes->empty())
                        {
                            const std::string fp = atm_fingerprint_b64(*ik_bytes);
                            if (!fp.empty())
                            {
                                const auto existing = load_atm_trust(*this, bare_jid, fp);
                                if (!existing || *existing == "undecided")
                                {
                                    store_atm_trust(*this, bare_jid, fp, "trusted");
                                    send_atm_trust_message(*this, *account, bare_jid, fp);
                                    // §5.1: drain deferred trust from this sender
                                    drain_pending_atm_trust(*this, bare_jid);
                                }
                            }
                        }
                    }

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
