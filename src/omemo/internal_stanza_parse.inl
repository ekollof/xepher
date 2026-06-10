void signal_log_emit(int level, const char *message, std::size_t length, void *user_data)
{
    (void) user_data;

    std::string text {message, length};
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
[[nodiscard]] auto parse_legacy_bundle(xmpp_stanza_t *bundle)
    -> std::optional<bundle_metadata>
{
    if (!bundle)
        return std::nullopt;

    bundle_metadata result;

    xmpp_stanza_t *spk = xmpp_stanza_get_child_by_name(bundle, "signedPreKeyPublic");
    if (!spk)
        return std::nullopt;
    const char *spk_id = xmpp_stanza_get_attribute(spk, "signedPreKeyId");
    if (!spk_id)
        return std::nullopt;
    result.signed_pre_key_id = spk_id;
    result.signed_pre_key = stanza_text(spk);

    xmpp_stanza_t *spks = xmpp_stanza_get_child_by_name(bundle, "signedPreKeySignature");
    if (!spks)
        return std::nullopt;
    result.signed_pre_key_signature = stanza_text(spks);

    xmpp_stanza_t *ik = xmpp_stanza_get_child_by_name(bundle, "identityKey");
    if (!ik)
        return std::nullopt;
    result.identity_key = stanza_text(ik);

    xmpp_stanza_t *prekeys = xmpp_stanza_get_child_by_name(bundle, "prekeys");
    if (!prekeys)
        return std::nullopt;

    for (xmpp_stanza_t *pk = xmpp_stanza_get_children(prekeys);
         pk; pk = xmpp_stanza_get_next(pk))
    {
        const char *name = xmpp_stanza_get_name(pk);
        if (!name || weechat_strcasecmp(name, "preKeyPublic") != 0)
            continue;
        const char *pk_id = xmpp_stanza_get_attribute(pk, "preKeyId");
        if (!pk_id)
            continue;
        result.prekeys.emplace_back(pk_id, stanza_text(pk));
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

    xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(items, "item");
    if (!item)
        return std::nullopt;

    xmpp_stanza_t *bundle = xmpp_stanza_get_child_by_name_and_ns(
        item, "bundle", kLegacyOmemoNs.data());
    if (!bundle)
        return std::nullopt;

    return parse_legacy_bundle(bundle);
}

