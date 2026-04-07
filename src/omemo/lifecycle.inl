XMPP_TEST_EXPORT weechat::xmpp::omemo::~omemo()
{
    // Teardown order matters for libsignal ref-counted objects.
    // Drop identity first, then store context, then signal context.
    // Remove side-store state only after libsignal objects are gone so any
    // destroy path cannot observe missing callbacks/user_data.
    identity = {};
    store_context = {};
    context = {};
    g_signal_store_states.erase(this);
}

xmpp_stanza_t *weechat::xmpp::omemo::get_bundle(xmpp_ctx_t *context, char *from, char *to)
{
    (void) from;
    (void) to;

    OMEMO_ASSERT(context != nullptr, "xmpp context must be present when publishing our OMEMO bundle");

    if (!*this)
        return nullptr;

    ensure_local_identity(*this);
    ensure_registration_id(*this);
    (void)ensure_prekeys(*this, context);  // return value intentionally discarded: caller is already publishing

    const auto bundle = make_local_bundle_metadata(*this);
    if (!bundle)
        return nullptr;

    const auto signed_pre_key_id = parse_uint32(bundle->signed_pre_key_id).value_or(0);
    if (!is_valid_omemo_device_id(device_id))
    {
        weechat_printf(nullptr,
                       "%somemo: refusing to publish bundle: invalid local device id %u",
                       weechat_prefix("error"), device_id);
        return nullptr;
    }
    if (signed_pre_key_id == 0)
    {
        weechat_printf(nullptr,
                       "%somemo: refusing to publish bundle for device %u: invalid signed prekey id '%s'",
                       weechat_prefix("error"), device_id, bundle->signed_pre_key_id.c_str());
        return nullptr;
    }
    if (bundle->prekeys.size() < kMinPreKeyCount)
    {
        weechat_printf(nullptr,
                       "%somemo: refusing to publish bundle for device %u: only %zu prekeys available (minimum %u required by XEP-0384)",
                       weechat_prefix("error"),
                       device_id,
                       bundle->prekeys.size(),
                       kMinPreKeyCount);
        return nullptr;
    }

    XDEBUG("omemo: publishing bundle for device {} with {} prekeys",
           device_id, bundle->prekeys.size());

    // Build <bundle xmlns='urn:xmpp:omemo:2'>
    stanza::xep0384::prekeys prekeys_spec;
    for (const auto &[id, key] : bundle->prekeys)
        prekeys_spec.add_pk(stanza::xep0384::pk(id, key));

    stanza::xep0384::bundle bundle_spec;
    bundle_spec.add_spk(stanza::xep0384::spk(bundle->signed_pre_key_id, bundle->signed_pre_key))
               .add_spks(stanza::xep0384::spks(bundle->signed_pre_key_signature))
               .add_ik(stanza::xep0384::ik(bundle->identity_key))
               .add_prekeys(prekeys_spec);

    // Build publish-options xdata form (access_model=open)
    stanza::xep0384::xdata_form form;
    form.add_field(stanza::xep0384::xdata_field(
             "FORM_TYPE",
             "http://jabber.org/protocol/pubsub#publish-options",
             "hidden"))
        .add_field(stanza::xep0384::xdata_field("pubsub#max_items", "max"))
        .add_field(stanza::xep0384::xdata_field("pubsub#access_model", "open"))
        .add_field(stanza::xep0384::xdata_field("pubsub#persist_items", "true"));

    stanza::xep0060::publish_options pub_opts;
    pub_opts.child_spec(form);

    const auto item_id = fmt::format("{}", device_id);
    stanza::xep0060::item item_spec;
    item_spec.id(item_id).payload(bundle_spec);

    stanza::xep0060::pubsub pubsub_spec;
    pubsub_spec.publish(stanza::xep0060::publish(kBundlesNode)
                            .item(item_spec))
               .publish_options(pub_opts);

    auto iq_sp = stanza::iq()
        .type("set")
        .id("omemo-bundle")
        .pubsub(pubsub_spec)
        .build(context);

    if (from) xmpp_stanza_set_attribute(iq_sp.get(), "from", from);
    if (to)   xmpp_stanza_set_attribute(iq_sp.get(), "to", to);

    xmpp_stanza_clone(iq_sp.get());  // bump refcount; shared_ptr dtor will release its ref
    return iq_sp.get();              // caller owns one ref; must call xmpp_stanza_release()
}

XMPP_TEST_EXPORT xmpp_stanza_t *weechat::xmpp::omemo::get_axolotl_bundle(xmpp_ctx_t *context, char *from, char *to)
{
    (void) from;
    (void) to;

    OMEMO_ASSERT(context != nullptr, "xmpp context must be present when publishing our legacy OMEMO bundle");

    if (!*this)
        return nullptr;

    ensure_local_identity(*this);
    ensure_registration_id(*this);
    (void)ensure_prekeys(*this, context);  // return value intentionally discarded: caller is already publishing

    const auto bundle = make_local_bundle_metadata(*this);
    if (!bundle)
        return nullptr;

    const auto signed_pre_key_id = parse_uint32(bundle->signed_pre_key_id).value_or(0);
    if (!is_valid_omemo_device_id(device_id))
    {
        weechat_printf(nullptr,
                       "%somemo: refusing to publish legacy bundle: invalid local device id %u",
                       weechat_prefix("error"), device_id);
        return nullptr;
    }
    if (signed_pre_key_id == 0)
    {
        weechat_printf(nullptr,
                       "%somemo: refusing to publish legacy bundle for device %u: invalid signed prekey id '%s'",
                       weechat_prefix("error"), device_id, bundle->signed_pre_key_id.c_str());
        return nullptr;
    }
    if (bundle->prekeys.size() < kMinPreKeyCount)
    {
        weechat_printf(nullptr,
                       "%somemo: refusing to publish legacy bundle for device %u: only %zu prekeys available (minimum %u required by XEP-0384)",
                       weechat_prefix("error"),
                       device_id,
                       bundle->prekeys.size(),
                       kMinPreKeyCount);
        return nullptr;
    }

    XDEBUG("omemo: publishing legacy bundle for device {} with {} prekeys",
           device_id, bundle->prekeys.size());

    // Build <bundle xmlns='eu.siacs.conversations.axolotl'>
    stanza::xep0384::axolotl_prekeys prekeys_spec;
    for (const auto &[id, key] : bundle->prekeys)
        prekeys_spec.add_pk(stanza::xep0384::axolotl_pk(id, key));

    stanza::xep0384::axolotl_bundle bundle_spec;
    bundle_spec.add_spk(stanza::xep0384::axolotl_spk(bundle->signed_pre_key_id, bundle->signed_pre_key))
               .add_spks(stanza::xep0384::axolotl_spks(bundle->signed_pre_key_signature))
               .add_ik(stanza::xep0384::axolotl_ik(bundle->identity_key))
               .add_prekeys(prekeys_spec);

    // Build publish-options xdata form (access_model=open)
    stanza::xep0384::xdata_form form;
    form.add_field(stanza::xep0384::xdata_field(
             "FORM_TYPE",
             "http://jabber.org/protocol/pubsub#publish-options",
             "hidden"))
        .add_field(stanza::xep0384::xdata_field("pubsub#max_items", "max"))
        .add_field(stanza::xep0384::xdata_field("pubsub#access_model", "open"))
        .add_field(stanza::xep0384::xdata_field("pubsub#persist_items", "true"));

    stanza::xep0060::publish_options pub_opts;
    pub_opts.child_spec(form);

    const auto item_id = fmt::format("{}", device_id);
    stanza::xep0060::item item_spec;
    item_spec.id(item_id).payload(bundle_spec);

    const auto legacy_node = fmt::format("{}{}", kLegacyBundlesNodePrefix, device_id);
    stanza::xep0060::pubsub pubsub_spec;
    pubsub_spec.publish(stanza::xep0060::publish(legacy_node)
                            .item(item_spec))
               .publish_options(pub_opts);

    auto iq_sp = stanza::iq()
        .type("set")
        .id("omemo-legacy-bundle")
        .pubsub(pubsub_spec)
        .build(context);

    if (from) xmpp_stanza_set_attribute(iq_sp.get(), "from", from);
    if (to)   xmpp_stanza_set_attribute(iq_sp.get(), "to", to);

    xmpp_stanza_clone(iq_sp.get());  // bump refcount; shared_ptr dtor will release its ref
    return iq_sp.get();              // caller owns one ref; must call xmpp_stanza_release()
}

// One-time migration: rename legacy_devicelist:* → axolotl_devicelist:*
// and legacy_bundle:* → axolotl_bundle:* in the LMDB database.
// Called once on DB open; idempotent (old keys are deleted after being copied).
static void migrate_legacy_keys(omemo &self)
{
    if (!self.db_env)
        return;

    // Collect keys to migrate in a read-only pass first (cursor invalidation safety).
    std::vector<std::pair<std::string, std::string>> to_rename; // {old_key, new_key}
    {
        auto rtxn = lmdb::txn::begin(self.db_env, nullptr, MDB_RDONLY);
        auto cursor = lmdb::cursor::open(rtxn, self.dbi.omemo);
        std::string_view key;
        std::string_view value;
        for (bool found = cursor.get(key, value, MDB_FIRST); found;
             found = cursor.get(key, value, MDB_NEXT))
        {
            if (key.starts_with("legacy_devicelist:"))
            {
                const auto suffix = key.substr(std::string_view("legacy_devicelist:").size());
                to_rename.emplace_back(std::string(key), fmt::format("axolotl_devicelist:{}", suffix));
            }
            else if (key.starts_with("legacy_bundle:"))
            {
                const auto suffix = key.substr(std::string_view("legacy_bundle:").size());
                to_rename.emplace_back(std::string(key), fmt::format("axolotl_bundle:{}", suffix));
            }
        }
        // rtxn aborts on scope exit (read-only, nothing to commit)
    }

    if (to_rename.empty())
        return;

    // Write pass: copy under new key then delete old key.
    auto wtxn = lmdb::txn::begin(self.db_env);
    for (const auto &[old_key, new_key] : to_rename)
    {
        std::string_view value;
        if (self.dbi.omemo.get(wtxn, old_key, value))
        {
            self.dbi.omemo.put(wtxn, new_key, std::string(value));
            self.dbi.omemo.del(wtxn, old_key);
            XDEBUG("omemo: migrated LMDB key '{}' → '{}'", old_key, new_key);
        }
    }
    wtxn.commit();

    weechat_printf(nullptr,
                   "%somemo: migrated %zu LMDB key(s) from legacy_* to axolotl_* prefix",
                   weechat_prefix("network"),
                   to_rename.size());
}

XMPP_TEST_EXPORT void weechat::xmpp::omemo::init(struct t_gui_buffer *buffer, const char *account_name)
{
    try
    {
        OMEMO_ASSERT(account_name != nullptr, "OMEMO init requires a non-null account name");

        // Clear in-flight state from any previous session to prevent stale
        // pending_* entries blocking retries on reconnect.
        pending_bundle_fetch.clear();
        pending_key_transport.clear();
        pending_iq_jid.clear();
        pending_configure_retry.clear();
        missing_omemo2_devicelist.clear();
        missing_axolotl_devicelist.clear();
        postponed_key_transports.clear();
        key_transport_bootstrap_attempted.clear();
        failed_session_bootstrap.clear();
        peers_with_observed_traffic.clear();
        prekey_reply_sent.clear();
        global_mam_catchup = false;
        bundle_republish_pending = false;

        gcrypt::check_version();

        db_path = make_db_path(account_name ? account_name : "default");
        ensure_db_open(*this);
        migrate_legacy_keys(*this);

        if (const auto stored_device_id = load_string(*this, kDeviceIdKey))
        {
            device_id = parse_uint32(*stored_device_id).value_or(random_device_id());
        }
        else
        {
            // Generate a fresh device ID and ensure it does not collide with any
            // device ID already present in the locally cached device list.
            // The LMDB cache is open at this point so previously fetched lists
            // are visible.  The probability of collision is ~1/2^31 per attempt,
            // but the spec says we MUST check.
            const auto own_jid = account_name ? std::string(account_name) : std::string{};
            auto id_in_cached_list = [&](std::uint32_t candidate) -> bool {
                if (own_jid.empty())
                    return false;
                // Check both OMEMO:2 and axolotl device lists.
                for (const auto &dl_key : {key_for_devicelist(own_jid),
                                           key_for_axolotl_devicelist(own_jid)})
                {
                    const auto cached = load_string(*this, dl_key);
                    if (!cached || cached->empty())
                        continue;
                    for (const auto &entry : split(*cached, ';'))
                    {
                        if (parse_uint32(entry).value_or(0) == candidate)
                            return true;
                    }
                }
                return false;
            };

            device_id = random_device_id();
            constexpr int kMaxCollisionRetries = 16;
            for (int attempt = 0;
                 attempt < kMaxCollisionRetries && id_in_cached_list(device_id);
                 ++attempt)
            {
                weechat_printf(buffer,
                               "%somemo: generated device id %u already present in cached devicelist; retrying",
                               weechat_prefix("network"), device_id);
                device_id = random_device_id();
            }
            store_string(*this, kDeviceIdKey, fmt::format("{}", device_id));
        }

        context.create(nullptr);
        context.set_log_function(signal_log_emit);
        store_context.create(context);

        auto store_state = std::make_unique<signal_store_state>();

        store_state->crypto.random_func = crypto_random;
        store_state->crypto.hmac_sha256_init_func = crypto_hmac_sha256_init;
        store_state->crypto.hmac_sha256_update_func = crypto_hmac_sha256_update;
        store_state->crypto.hmac_sha256_final_func = crypto_hmac_sha256_final;
        store_state->crypto.hmac_sha256_cleanup_func = crypto_hmac_sha256_cleanup;
        store_state->crypto.sha512_digest_init_func = crypto_sha512_init;
        store_state->crypto.sha512_digest_update_func = crypto_sha512_update;
        store_state->crypto.sha512_digest_final_func = crypto_sha512_final;
        store_state->crypto.sha512_digest_cleanup_func = crypto_sha512_cleanup;
        store_state->crypto.encrypt_func = crypto_encrypt;
        store_state->crypto.decrypt_func = crypto_decrypt;
        store_state->crypto.user_data = this;

        context.set_crypto_provider(&store_state->crypto);
        context.set_locking_functions(crypto_lock, crypto_unlock);

        store_state->identity.get_identity_key_pair = identity_get_key_pair;
        store_state->identity.get_local_registration_id = identity_get_local_registration_id;
        store_state->identity.save_identity = identity_save;
        store_state->identity.is_trusted_identity = identity_is_trusted;
        store_state->identity.user_data = this;

        store_state->pre_key.load_pre_key = pre_key_load;
        store_state->pre_key.store_pre_key = pre_key_store_record;
        store_state->pre_key.contains_pre_key = pre_key_contains;
        store_state->pre_key.remove_pre_key = pre_key_remove;
        store_state->pre_key.user_data = this;

        store_state->signed_pre_key.load_signed_pre_key = signed_pre_key_load;
        store_state->signed_pre_key.store_signed_pre_key = signed_pre_key_store_record;
        store_state->signed_pre_key.contains_signed_pre_key = signed_pre_key_contains;
        store_state->signed_pre_key.remove_signed_pre_key = signed_pre_key_remove;
        store_state->signed_pre_key.user_data = this;

        store_state->session.load_session_func = session_load;
        store_state->session.get_sub_device_sessions_func = session_get_sub_devices;
        store_state->session.store_session_func = session_store_record;
        store_state->session.contains_session_func = session_contains;
        store_state->session.delete_session_func = session_delete;
        store_state->session.delete_all_sessions_func = session_delete_all;
        store_state->session.user_data = this;

        store_state->sender_key.store_sender_key = sender_key_store_record;
        store_state->sender_key.load_sender_key = sender_key_load;
        store_state->sender_key.user_data = this;

        store_context.set_identity_key_store(&store_state->identity);
        store_context.set_pre_key_store(&store_state->pre_key);
        store_context.set_signed_pre_key_store(&store_state->signed_pre_key);
        store_context.set_session_store(&store_state->session);
        store_context.set_sender_key_store(&store_state->sender_key);
        g_signal_store_states[this] = std::move(store_state);

        ensure_local_identity(*this);
        ensure_registration_id(*this);

        print_info(buffer, fmt::format(
            "OMEMO initialized for account '{}' (device {}).",
            account_name ? account_name : "?", device_id));
    }
    catch (const std::exception &exception)
    {
        context = {};
        store_context = {};
        identity = {};
        device_id = 0;
        db_env = nullptr;
        dbi.omemo = 0;
        print_error(buffer, fmt::format("OMEMO init failed: {}", exception.what()));
    }
}

