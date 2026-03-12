        const char *name = xmpp_stanza_get_name(child);
        if (!name)
            continue;

        if (weechat_strcasecmp(name, "spk") == 0)
        {
            const char *id = xmpp_stanza_get_attribute(child, "id");
            if (id)
                bundle.signed_pre_key_id = id;
            bundle.signed_pre_key = stanza_text(child);
        }
        else if (weechat_strcasecmp(name, "spks") == 0)
        {
            bundle.signed_pre_key_signature = stanza_text(child);
        }
        else if (weechat_strcasecmp(name, "ik") == 0)
        {
            bundle.identity_key = stanza_text(child);
        }
        else if (weechat_strcasecmp(name, "prekeys") == 0)
        {
            for (xmpp_stanza_t *prekey = xmpp_stanza_get_children(child);
                 prekey;
                 prekey = xmpp_stanza_get_next(prekey))
            {
                const char *prekey_name = xmpp_stanza_get_name(prekey);
                if (!prekey_name || weechat_strcasecmp(prekey_name, "pk") != 0)
                    continue;

                const char *id = xmpp_stanza_get_attribute(prekey, "id");
                if (!id)
                    continue;

                const auto parsed_id = parse_uint32(id).value_or(0);
                if (!is_valid_omemo_device_id(parsed_id))
                {
                    weechat_printf(nullptr,
                                   "%somemo: bundle parse failed: invalid prekey id '%s'",
                                   weechat_prefix("error"),
                                   id);
                    return std::nullopt;
                }

                const bool duplicate_id = std::ranges::any_of(
                    bundle.prekeys,
                    [&](const auto &existing) {
                        return existing.first == id;
                    });
                if (duplicate_id)
                {
                    weechat_printf(nullptr,
                                   "%somemo: bundle parse failed: duplicate prekey id '%s'",
                                   weechat_prefix("error"),
                                   id);
                    return std::nullopt;
                }

                bundle.prekeys.emplace_back(id, stanza_text(prekey));
            }
        }
    }

    if (!is_valid_omemo_device_id(parse_uint32(bundle.signed_pre_key_id).value_or(0)))
    {
        weechat_printf(nullptr,
                       "%somemo: bundle parse failed: invalid signed prekey id '%s'",
                       weechat_prefix("error"),
                       bundle.signed_pre_key_id.c_str());
        return std::nullopt;
    }

    if (bundle.signed_pre_key_id.empty() || bundle.signed_pre_key.empty()
        || bundle.signed_pre_key_signature.empty() || bundle.identity_key.empty())
    {
        weechat_printf(nullptr,
                       "%somemo: bundle parse failed: missing required spk/spks/ik fields",
                       weechat_prefix("error"));
        return std::nullopt;
    }

    if (bundle.prekeys.empty())
    {
        weechat_printf(nullptr,
                       "%somemo: bundle parse failed: bundle has no prekeys",
                       weechat_prefix("error"));
        return std::nullopt;
    }

    const bool has_empty_prekey = std::ranges::any_of(
        bundle.prekeys,
        [](const auto &prekey) {
            return prekey.first.empty() || prekey.second.empty();
        });
    if (has_empty_prekey)
    {
        weechat_printf(nullptr,
                       "%somemo: bundle parse failed: bundle contains empty prekey entries",
                       weechat_prefix("error"));
        return std::nullopt;
    }

    return bundle;
}

void signal_log_emit(int level, const char *message, std::size_t length, void *user_data)
{
    (void) user_data;

    const char *prefix = weechat_prefix(level <= SG_LOG_WARNING ? "error" : "network");
    std::string text {message, length};
    weechat_printf(nullptr, "%somemo: %s", prefix, text.c_str());
}

// Parse a legacy OMEMO bundle (<bundle xmlns="eu.siacs.conversations.axolotl">).
// Conversations element names differ from OMEMO:2 (<spk>/<ik>/<pk>):
//   <signedPreKeyPublic signedPreKeyId="N">base64</signedPreKeyPublic>
//   <signedPreKeySignature>base64</signedPreKeySignature>
//   <identityKey>base64</identityKey>
//   <prekeys xmlns="eu.siacs.conversations.axolotl">
//     <preKeyPublic preKeyId="N">base64</preKeyPublic> ...
//   </prekeys>
// Maps to the same bundle_metadata struct used by OMEMO:2.
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
[[nodiscard]] auto extract_legacy_bundle_from_items(xmpp_stanza_t *items)
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

