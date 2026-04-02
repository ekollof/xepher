weechat::xmpp::omemo::~omemo()
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
    ensure_prekeys(*this, context);

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

    // All intermediate nodes are shared_ptr (auto-released). The root iq is
    // returned raw with ownership transfer to the caller.
    auto mk = [&]() {
        return std::shared_ptr<xmpp_stanza_t> { xmpp_stanza_new(context), xmpp_stanza_release };
    };

    // <spk id='…'>base64</spk>  (XEP-0384 §4.3 wire format)
    auto spk_text = mk();
    xmpp_stanza_set_text(spk_text.get(), bundle->signed_pre_key.c_str());
    auto spk = mk();
    xmpp_stanza_set_name(spk.get(), "spk");
    xmpp_stanza_set_attribute(spk.get(), "id", bundle->signed_pre_key_id.c_str());
    xmpp_stanza_set_ns(spk.get(), kOmemoNs.data());
    xmpp_stanza_add_child(spk.get(), spk_text.get());

    // <spks>base64</spks>
    auto spks_text = mk();
    xmpp_stanza_set_text(spks_text.get(), bundle->signed_pre_key_signature.c_str());
    auto spks = mk();
    xmpp_stanza_set_name(spks.get(), "spks");
    xmpp_stanza_set_ns(spks.get(), kOmemoNs.data());
    xmpp_stanza_add_child(spks.get(), spks_text.get());

    // <ik>base64</ik>
    auto ik_text = mk();
    xmpp_stanza_set_text(ik_text.get(), bundle->identity_key.c_str());
    auto ik = mk();
    xmpp_stanza_set_name(ik.get(), "ik");
    xmpp_stanza_set_ns(ik.get(), kOmemoNs.data());
    xmpp_stanza_add_child(ik.get(), ik_text.get());

    // <prekeys> loop — each prekey is <pk id='…'>base64</pk>
    auto prekeys = mk();
    xmpp_stanza_set_name(prekeys.get(), "prekeys");
    xmpp_stanza_set_ns(prekeys.get(), kOmemoNs.data());
    for (const auto &[id, key] : bundle->prekeys)
    {
        auto pk_text = mk();
        xmpp_stanza_set_text(pk_text.get(), key.c_str());
        auto pk = mk();
        xmpp_stanza_set_name(pk.get(), "pk");
        xmpp_stanza_set_attribute(pk.get(), "id", id.c_str());
        xmpp_stanza_set_ns(pk.get(), kOmemoNs.data());
        xmpp_stanza_add_child(pk.get(), pk_text.get());
        xmpp_stanza_add_child(prekeys.get(), pk.get());
    }

    // <bundle xmlns='urn:xmpp:omemo:2'>
    auto bundle_stanza = mk();
    xmpp_stanza_set_name(bundle_stanza.get(), "bundle");
    xmpp_stanza_set_ns(bundle_stanza.get(), kOmemoNs.data());
    xmpp_stanza_add_child(bundle_stanza.get(), spk.get());
    xmpp_stanza_add_child(bundle_stanza.get(), spks.get());
    xmpp_stanza_add_child(bundle_stanza.get(), ik.get());
    xmpp_stanza_add_child(bundle_stanza.get(), prekeys.get());

    // <item id='device_id'>
    const auto item_id = fmt::format("{}", device_id);
    auto item = mk();
    xmpp_stanza_set_name(item.get(), "item");
    xmpp_stanza_set_attribute(item.get(), "id", item_id.c_str());
    xmpp_stanza_add_child(item.get(), bundle_stanza.get());

    // <publish node='…'>
    auto publish = mk();
    xmpp_stanza_set_name(publish.get(), "publish");
    xmpp_stanza_set_attribute(publish.get(), "node", kBundlesNode.data());
    xmpp_stanza_add_child(publish.get(), item.get());

    // <pubsub xmlns='…'>
    auto pubsub = mk();
    xmpp_stanza_set_name(pubsub.get(), "pubsub");
    xmpp_stanza_set_ns(pubsub.get(), "http://jabber.org/protocol/pubsub");
    xmpp_stanza_add_child(pubsub.get(), publish.get());

    // Add publish-options (access_model=open) so the server allows contacts
    // to fetch our bundle.  Without this some servers default to a restricted
    // access model and silently reject the publish or make the bundle invisible.
    {
        auto make_field = [&](const char *var, const char *val, const char *type_attr = nullptr) {
            auto field = mk();
            xmpp_stanza_set_name(field.get(), "field");
            xmpp_stanza_set_attribute(field.get(), "var", var);
            if (type_attr) xmpp_stanza_set_attribute(field.get(), "type", type_attr);
            auto value = mk();
            xmpp_stanza_set_name(value.get(), "value");
            auto txt = mk();
            xmpp_stanza_set_text(txt.get(), val);
            xmpp_stanza_add_child(value.get(), txt.get());
            xmpp_stanza_add_child(field.get(), value.get());
            return field;
        };

        auto x = mk();
        xmpp_stanza_set_name(x.get(), "x");
        xmpp_stanza_set_ns(x.get(), "jabber:x:data");
        xmpp_stanza_set_attribute(x.get(), "type", "submit");

        auto f1 = make_field("FORM_TYPE",
            "http://jabber.org/protocol/pubsub#publish-options", "hidden");
        auto f2 = make_field("pubsub#max_items", "max");
        auto f3 = make_field("pubsub#access_model", "open");
        auto f4 = make_field("pubsub#persist_items", "true");

        xmpp_stanza_add_child(x.get(), f1.get());
        xmpp_stanza_add_child(x.get(), f2.get());
        xmpp_stanza_add_child(x.get(), f3.get());
        xmpp_stanza_add_child(x.get(), f4.get());

        auto publish_options = mk();
        xmpp_stanza_set_name(publish_options.get(), "publish-options");
        xmpp_stanza_add_child(publish_options.get(), x.get());

        xmpp_stanza_add_child(pubsub.get(), publish_options.get());
    }

    // Build <iq type='set' id='omemo-bundle'> — returned raw (caller owns)
    xmpp_stanza_t *iq = xmpp_stanza_new(context);
    xmpp_stanza_set_name(iq, "iq");
    xmpp_stanza_set_type(iq, "set");
    xmpp_stanza_set_id(iq, "omemo-bundle");
    if (from) xmpp_stanza_set_attribute(iq, "from", from);
    if (to)   xmpp_stanza_set_attribute(iq, "to", to);
    xmpp_stanza_add_child(iq, pubsub.get());

    return iq;
}

xmpp_stanza_t *weechat::xmpp::omemo::get_legacy_bundle(xmpp_ctx_t *context, char *from, char *to)
{
    (void) from;
    (void) to;

    OMEMO_ASSERT(context != nullptr, "xmpp context must be present when publishing our legacy OMEMO bundle");

    if (!*this)
        return nullptr;

    ensure_local_identity(*this);
    ensure_registration_id(*this);
    ensure_prekeys(*this, context);

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

    auto mk_leg = [&]() {
        return std::shared_ptr<xmpp_stanza_t> { xmpp_stanza_new(context), xmpp_stanza_release };
    };

    auto make_text_node = [&](const char *name, const std::string &text,
                              const char *attr_name = nullptr,
                              const std::string *attr_value = nullptr) {
        auto node = mk_leg();
        xmpp_stanza_set_name(node.get(), name);
        if (attr_name && attr_value)
            xmpp_stanza_set_attribute(node.get(), attr_name, attr_value->c_str());
        auto txt = mk_leg();
        xmpp_stanza_set_text(txt.get(), text.c_str());
        xmpp_stanza_add_child(node.get(), txt.get());
        return node;
    };

    auto bundle_stanza = mk_leg();
    xmpp_stanza_set_name(bundle_stanza.get(), "bundle");
    xmpp_stanza_set_ns(bundle_stanza.get(), kLegacyOmemoNs.data());

    auto spk = make_text_node("signedPreKeyPublic",
                              bundle->signed_pre_key,
                              "signedPreKeyId",
                              &bundle->signed_pre_key_id);
    xmpp_stanza_add_child(bundle_stanza.get(), spk.get());

    auto spks = make_text_node("signedPreKeySignature",
                               bundle->signed_pre_key_signature);
    xmpp_stanza_add_child(bundle_stanza.get(), spks.get());

    auto ik = make_text_node("identityKey", bundle->identity_key);
    xmpp_stanza_add_child(bundle_stanza.get(), ik.get());

    auto prekeys = mk_leg();
    xmpp_stanza_set_name(prekeys.get(), "prekeys");
    xmpp_stanza_set_ns(prekeys.get(), kLegacyOmemoNs.data());
    for (const auto &[id, key] : bundle->prekeys)
    {
        auto pk = make_text_node("preKeyPublic", key, "preKeyId", &id);
        xmpp_stanza_add_child(prekeys.get(), pk.get());
    }
    xmpp_stanza_add_child(bundle_stanza.get(), prekeys.get());

    const auto item_id = fmt::format("{}", device_id);
    auto item = mk_leg();
    xmpp_stanza_set_name(item.get(), "item");
    xmpp_stanza_set_attribute(item.get(), "id", item_id.c_str());
    xmpp_stanza_add_child(item.get(), bundle_stanza.get());

    const auto node = fmt::format("{}{}", kLegacyBundlesNodePrefix, device_id);
    auto publish = mk_leg();
    xmpp_stanza_set_name(publish.get(), "publish");
    xmpp_stanza_set_attribute(publish.get(), "node", node.c_str());
    xmpp_stanza_add_child(publish.get(), item.get());

    auto pubsub = mk_leg();
    xmpp_stanza_set_name(pubsub.get(), "pubsub");
    xmpp_stanza_set_ns(pubsub.get(), "http://jabber.org/protocol/pubsub");
    xmpp_stanza_add_child(pubsub.get(), publish.get());

    // Keep legacy bundle node readable to interop clients.
    {
        auto make_field = [&](const char *var, const char *val, const char *type_attr = nullptr) {
            auto field = mk_leg();
            xmpp_stanza_set_name(field.get(), "field");
            xmpp_stanza_set_attribute(field.get(), "var", var);
            if (type_attr) xmpp_stanza_set_attribute(field.get(), "type", type_attr);
            auto value = mk_leg();
            xmpp_stanza_set_name(value.get(), "value");
            auto txt = mk_leg();
            xmpp_stanza_set_text(txt.get(), val);
            xmpp_stanza_add_child(value.get(), txt.get());
            xmpp_stanza_add_child(field.get(), value.get());
            return field;
        };

        auto x = mk_leg();
        xmpp_stanza_set_name(x.get(), "x");
        xmpp_stanza_set_ns(x.get(), "jabber:x:data");
        xmpp_stanza_set_attribute(x.get(), "type", "submit");

        auto f1 = make_field("FORM_TYPE",
            "http://jabber.org/protocol/pubsub#publish-options", "hidden");
        auto f2 = make_field("pubsub#max_items", "max");
        auto f3 = make_field("pubsub#access_model", "open");
        auto f4 = make_field("pubsub#persist_items", "true");

        xmpp_stanza_add_child(x.get(), f1.get());
        xmpp_stanza_add_child(x.get(), f2.get());
        xmpp_stanza_add_child(x.get(), f3.get());
        xmpp_stanza_add_child(x.get(), f4.get());

        auto publish_options = mk_leg();
        xmpp_stanza_set_name(publish_options.get(), "publish-options");
        xmpp_stanza_add_child(publish_options.get(), x.get());

        xmpp_stanza_add_child(pubsub.get(), publish_options.get());
    }

    // Build <iq type='set' id='omemo-legacy-bundle'> — returned raw (caller owns)
    xmpp_stanza_t *iq = xmpp_stanza_new(context);
    xmpp_stanza_set_name(iq, "iq");
    xmpp_stanza_set_type(iq, "set");
    xmpp_stanza_set_id(iq, "omemo-legacy-bundle");
    if (from) xmpp_stanza_set_attribute(iq, "from", from);
    if (to)   xmpp_stanza_set_attribute(iq, "to", to);
    xmpp_stanza_add_child(iq, pubsub.get());

    return iq;
}

void weechat::xmpp::omemo::init(struct t_gui_buffer *buffer, const char *account_name)
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
        missing_legacy_devicelist.clear();
        postponed_key_transports.clear();
        key_transport_bootstrap_attempted.clear();
        failed_session_bootstrap.clear();
        peers_with_observed_traffic.clear();
        global_mam_catchup = false;

        gcrypt::check_version();

        db_path = make_db_path(account_name ? account_name : "default");
        ensure_db_open(*this);

        if (const auto stored_device_id = load_string(*this, kDeviceIdKey))
        {
            device_id = parse_uint32(*stored_device_id).value_or(random_device_id());
        }
        else
        {
            device_id = random_device_id();
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

