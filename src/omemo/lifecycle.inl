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

    weechat_printf(nullptr,
                   "%somemo: publishing bundle for device %u with %zu prekeys",
                   weechat_prefix("network"), device_id, bundle->prekeys.size());

    xmpp_stanza_t *text = nullptr;
    xmpp_stanza_t *spk = nullptr;
    xmpp_stanza_t *spks = nullptr;
    xmpp_stanza_t *ik = nullptr;
    xmpp_stanza_t *prekeys = nullptr;
    xmpp_stanza_t *bundle_stanza = nullptr;
    xmpp_stanza_t *item = nullptr;
    xmpp_stanza_t *publish = nullptr;
    xmpp_stanza_t *pubsub = nullptr;
    xmpp_stanza_t *iq = nullptr;

    text = xmpp_stanza_new(context);
    xmpp_stanza_set_text(text, bundle->signed_pre_key.c_str());
    xmpp_stanza_t *spk_children[] = {text, nullptr};
    spk = stanza__iq_pubsub_publish_item_bundle_signedPreKeyPublic(
        context, nullptr, spk_children, with_noop(bundle->signed_pre_key_id.c_str()));

    text = xmpp_stanza_new(context);
    xmpp_stanza_set_text(text, bundle->signed_pre_key_signature.c_str());
    xmpp_stanza_t *spks_children[] = {text, nullptr};
    spks = stanza__iq_pubsub_publish_item_bundle_signedPreKeySignature(context, nullptr, spks_children);

    text = xmpp_stanza_new(context);
    xmpp_stanza_set_text(text, bundle->identity_key.c_str());
    xmpp_stanza_t *ik_children[] = {text, nullptr};
    ik = stanza__iq_pubsub_publish_item_bundle_identityKey(context, nullptr, ik_children);

    std::vector<xmpp_stanza_t *> prekey_stanzas;
    prekey_stanzas.reserve(bundle->prekeys.size() + 1);
    for (const auto &[id, key] : bundle->prekeys)
    {
        text = xmpp_stanza_new(context);
        xmpp_stanza_set_text(text, key.c_str());
        xmpp_stanza_t *pk_children[] = {text, nullptr};
        prekey_stanzas.push_back(
            stanza__iq_pubsub_publish_item_bundle_prekeys_preKeyPublic(
                context, nullptr, pk_children, with_noop(id.c_str())));
    }
    prekey_stanzas.push_back(nullptr);
    prekeys = stanza__iq_pubsub_publish_item_bundle_prekeys(context, nullptr, prekey_stanzas.data());

    xmpp_stanza_t *bundle_children[] = {spk, spks, ik, prekeys, nullptr};
    bundle_stanza = stanza__iq_pubsub_publish_item_bundle(context, nullptr, bundle_children,
                                                          with_noop(kOmemoNs.data()));
    xmpp_stanza_t *item_children[] = {bundle_stanza, nullptr};
    const auto item_id = fmt::format("{}", device_id);
    item = stanza__iq_pubsub_publish_item(context, nullptr, item_children, with_noop(item_id.c_str()));
    xmpp_stanza_t *publish_children[] = {item, nullptr};
    publish = stanza__iq_pubsub_publish(context, nullptr, publish_children, with_noop(kBundlesNode.data()));
    xmpp_stanza_t *pubsub_children[] = {publish, nullptr};
    pubsub = stanza__iq_pubsub(context, nullptr, pubsub_children, with_noop("http://jabber.org/protocol/pubsub"));

    // Add publish-options (access_model=open) so the server allows contacts
    // to fetch our bundle.  Without this some servers default to a restricted
    // access model and silently reject the publish or make the bundle invisible.
    {
        auto make_field = [&](const char *var, const char *val, const char *type_attr = nullptr) {
            xmpp_stanza_t *field = xmpp_stanza_new(context);
            xmpp_stanza_set_name(field, "field");
            xmpp_stanza_set_attribute(field, "var", var);
            if (type_attr) xmpp_stanza_set_attribute(field, "type", type_attr);
            xmpp_stanza_t *value = xmpp_stanza_new(context);
            xmpp_stanza_set_name(value, "value");
            xmpp_stanza_t *txt = xmpp_stanza_new(context);
            xmpp_stanza_set_text(txt, val);
            xmpp_stanza_add_child(value, txt);
            xmpp_stanza_release(txt);
            xmpp_stanza_add_child(field, value);
            xmpp_stanza_release(value);
            return field;
        };

        xmpp_stanza_t *x = xmpp_stanza_new(context);
        xmpp_stanza_set_name(x, "x");
        xmpp_stanza_set_ns(x, "jabber:x:data");
        xmpp_stanza_set_attribute(x, "type", "submit");

        xmpp_stanza_t *f1 = make_field("FORM_TYPE",
            "http://jabber.org/protocol/pubsub#publish-options", "hidden");
        xmpp_stanza_t *f2 = make_field("pubsub#max_items", "max");
        xmpp_stanza_t *f3 = make_field("pubsub#access_model", "open");
        xmpp_stanza_t *f4 = make_field("pubsub#persist_items", "true");

        xmpp_stanza_add_child(x, f1); xmpp_stanza_release(f1);
        xmpp_stanza_add_child(x, f2); xmpp_stanza_release(f2);
        xmpp_stanza_add_child(x, f3); xmpp_stanza_release(f3);
        xmpp_stanza_add_child(x, f4); xmpp_stanza_release(f4);

        xmpp_stanza_t *publish_options = xmpp_stanza_new(context);
        xmpp_stanza_set_name(publish_options, "publish-options");
        xmpp_stanza_add_child(publish_options, x);
        xmpp_stanza_release(x);

        xmpp_stanza_add_child(pubsub, publish_options);
        xmpp_stanza_release(publish_options);
    }

    xmpp_stanza_t *iq_children[] = {pubsub, nullptr};
    iq = stanza__iq(context, nullptr, iq_children, nullptr, "omemo-bundle", from, to, "set");

    return iq;
}

void weechat::xmpp::omemo::init(struct t_gui_buffer *buffer, const char *account_name)
{
    try
    {
        OMEMO_ASSERT(account_name != nullptr, "OMEMO init requires a non-null account name");

        // Clear in-flight state from any previous session to prevent stale
        // pending_bundle_fetch entries blocking bundle re-fetches on reconnect.
        pending_bundle_fetch.clear();
        pending_key_transport.clear();
        pending_iq_jid.clear();
        pending_configure_retry.clear();
        missing_omemo2_devicelist.clear();
        missing_legacy_devicelist.clear();

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

