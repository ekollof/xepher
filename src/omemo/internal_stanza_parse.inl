void signal_log_emit(int level, const char *message, std::size_t length, void *user_data)
{
    auto *self = static_cast<omemo *>(user_data);
    std::string text {message, length};

    // libsignal logs "No session for: <name>:<device>" using signal_protocol_address
    // fields that can be stale or dangling during inner SignalMessage attempts.
    // Rewrite with the peer pinned by signal_store_peer_scope when available.
    if (text.starts_with("No session for: ") && self
        && self->signal_store_peer_depth_ > 0
        && is_plausible_signal_jid(self->signal_store_peer_jid_)
        && is_valid_omemo_device_id(self->signal_store_peer_device_id_))
    {
        text = fmt::format("No session for: {}/{}",
                           self->signal_store_peer_jid_, self->signal_store_peer_device_id_);
        if (level <= SG_LOG_WARNING)
            XDEBUG("omemo: {} (expected while establishing session)", text);
        else
            XDEBUG("omemo: {}", text);
        return;
    }

    if (level <= SG_LOG_WARNING)
        print_error(nullptr, fmt::format("omemo: {}", text));
    else
        XDEBUG("omemo: {}", text);
}

// Parse an axolotl OMEMO bundle (<bundle xmlns="eu.siacs.conversations.axolotl">).
//   <signedPreKeyPublic signedPreKeyId="N">base64</signedPreKeyPublic>
//   <signedPreKeySignature>base64</signedPreKeySignature>
//   <identityKey>base64</identityKey>
//   <prekeys xmlns="eu.siacs.conversations.axolotl">
//     <preKeyPublic preKeyId="N">base64</preKeyPublic> ...
//   </prekeys>
[[nodiscard]] auto parse_legacy_bundle(::xmpp::StanzaView bundle)
    -> std::optional<bundle_metadata>
{
    if (!bundle.valid())
        return std::nullopt;

    bundle_metadata result;

    const ::xmpp::StanzaView spk = bundle.child("signedPreKeyPublic");
    if (!spk.valid())
        return std::nullopt;
    const auto spk_id = spk.attr("signedPreKeyId");
    if (!spk_id)
        return std::nullopt;
    result.signed_pre_key_id = std::string(*spk_id);
    result.signed_pre_key = spk.text();

    const ::xmpp::StanzaView spks = bundle.child("signedPreKeySignature");
    if (!spks.valid())
        return std::nullopt;
    result.signed_pre_key_signature = spks.text();

    const ::xmpp::StanzaView ik = bundle.child("identityKey");
    if (!ik.valid())
        return std::nullopt;
    result.identity_key = ik.text();

    const ::xmpp::StanzaView prekeys = bundle.child("prekeys");
    if (!prekeys.valid())
        return std::nullopt;

    for (const ::xmpp::StanzaView pk : prekeys)
    {
        if (!stanza_attr_iequals(pk.name(), "preKeyPublic"))
            continue;
        const auto pk_id = pk.attr("preKeyId");
        if (!pk_id)
            continue;
        result.prekeys.emplace_back(std::string(*pk_id), pk.text());
    }

    if (result.signed_pre_key.empty() || result.signed_pre_key_signature.empty()
        || result.identity_key.empty() || result.prekeys.empty())
        return std::nullopt;

    return result;
}

// Extract a legacy bundle from a pubsub <items> result.
[[maybe_unused]] [[nodiscard]] auto extract_legacy_bundle_from_items(xmpp_stanza_t *items)
    -> std::optional<bundle_metadata>
{
    if (!items)
        return std::nullopt;

    const ::xmpp::StanzaView items_view(items);
    const ::xmpp::StanzaView item = items_view.child("item");
    if (!item.valid())
        return std::nullopt;

    const ::xmpp::StanzaView bundle = item.child("bundle", kLegacyOmemoNs);
    return parse_legacy_bundle(bundle);
}