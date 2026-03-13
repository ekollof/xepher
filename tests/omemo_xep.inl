#include <doctest/doctest.h>

#include <strophe.h>

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>

namespace {

struct strophe_env {
    xmpp_ctx_t *ctx {nullptr};

    strophe_env()
    {
        xmpp_initialize();
        ctx = xmpp_ctx_new(nullptr, nullptr);
    }

    ~strophe_env()
    {
        if (ctx)
            xmpp_ctx_free(ctx);
        xmpp_shutdown();
    }
};

auto parse_device_id(const char *value) -> bool
{
    if (!value || !*value)
        return false;

    char *end = nullptr;
    errno = 0;
    unsigned long parsed = std::strtoul(value, &end, 10);
    if (errno != 0 || !end || *end != '\0')
        return false;
    if (parsed == 0 || parsed > static_cast<unsigned long>(std::numeric_limits<std::int32_t>::max()))
        return false;
    return true;
}

auto has_non_empty_text(xmpp_stanza_t *stanza) -> bool
{
    if (!stanza)
        return false;

    char *text = xmpp_stanza_get_text(stanza);
    if (!text)
        return false;

    const bool ok = *text != '\0';
    xmpp_free(xmpp_stanza_get_context(stanza), text);
    return ok;
}

auto validate_omemo2_encrypted_envelope(xmpp_ctx_t *ctx,
                                        std::string_view xml,
                                        std::string_view own_bare_jid,
                                        std::uint32_t own_device_id,
                                        bool allow_key_transport) -> bool
{
    xmpp_stanza_t *root = xmpp_stanza_new_from_string(ctx, std::string(xml).c_str());
    if (!root)
        return false;

    xmpp_stanza_t *encrypted = xmpp_stanza_get_child_by_name_and_ns(root, "encrypted", "urn:xmpp:omemo:2");
    if (!encrypted)
        return false;

    xmpp_stanza_t *header = xmpp_stanza_get_child_by_name(encrypted, "header");
    if (!header)
        return false;

    if (!parse_device_id(xmpp_stanza_get_attribute(header, "sid")))
        return false;

    bool found_keys_for_own_jid = false;
    bool found_key_for_own_device = false;
    bool found_kex_key = false;
    for (xmpp_stanza_t *child = xmpp_stanza_get_children(header); child; child = xmpp_stanza_get_next(child))
    {
        const char *name = xmpp_stanza_get_name(child);
        if (!name || std::strcmp(name, "keys") != 0)
            continue;

        const char *keys_jid = xmpp_stanza_get_attribute(child, "jid");
        if (!keys_jid || std::strchr(keys_jid, '/'))
            return false;

        if (own_bare_jid == keys_jid)
            found_keys_for_own_jid = true;

        bool keys_has_any_key = false;
        for (xmpp_stanza_t *key = xmpp_stanza_get_children(child); key; key = xmpp_stanza_get_next(key))
        {
            const char *key_name = xmpp_stanza_get_name(key);
            if (!key_name || std::strcmp(key_name, "key") != 0)
                continue;

            keys_has_any_key = true;
            const char *rid = xmpp_stanza_get_attribute(key, "rid");
            if (!parse_device_id(rid))
                return false;
            if (!has_non_empty_text(key))
                return false;
            const char *kex = xmpp_stanza_get_attribute(key, "kex");
            if (kex && std::strcmp(kex, "true") == 0)
                found_kex_key = true;

            if (own_bare_jid == keys_jid && static_cast<std::uint32_t>(std::strtoul(rid, nullptr, 10)) == own_device_id)
                found_key_for_own_device = true;
        }

        if (!keys_has_any_key)
            return false;
    }

    if (!found_keys_for_own_jid || !found_key_for_own_device)
        return false;

    xmpp_stanza_t *payload = xmpp_stanza_get_child_by_name(encrypted, "payload");
    if (payload)
        return has_non_empty_text(payload);

    if (allow_key_transport && !found_kex_key)
        return false;

    return allow_key_transport;
}

auto validate_legacy_bundle(xmpp_ctx_t *ctx, std::string_view xml) -> bool
{
    xmpp_stanza_t *root = xmpp_stanza_new_from_string(ctx, std::string(xml).c_str());
    if (!root)
        return false;

    xmpp_stanza_t *bundle = xmpp_stanza_get_child_by_name_and_ns(
        root, "bundle", "eu.siacs.conversations.axolotl");
    if (!bundle)
        return false;

    xmpp_stanza_t *spk = xmpp_stanza_get_child_by_name(bundle, "signedPreKeyPublic");
    if (!spk)
        return false;
    if (!parse_device_id(xmpp_stanza_get_attribute(spk, "signedPreKeyId")))
        return false;
    if (!has_non_empty_text(spk))
        return false;

    xmpp_stanza_t *spks = xmpp_stanza_get_child_by_name(bundle, "signedPreKeySignature");
    if (!spks || !has_non_empty_text(spks))
        return false;

    xmpp_stanza_t *ik = xmpp_stanza_get_child_by_name(bundle, "identityKey");
    if (!ik || !has_non_empty_text(ik))
        return false;

    xmpp_stanza_t *prekeys = xmpp_stanza_get_child_by_name(bundle, "prekeys");
    if (!prekeys)
        return false;

    bool has_prekey = false;
    for (xmpp_stanza_t *pk = xmpp_stanza_get_children(prekeys); pk; pk = xmpp_stanza_get_next(pk))
    {
        const char *name = xmpp_stanza_get_name(pk);
        if (!name || std::strcmp(name, "preKeyPublic") != 0)
            continue;

        if (!parse_device_id(xmpp_stanza_get_attribute(pk, "preKeyId")))
            return false;
        if (!has_non_empty_text(pk))
            return false;
        has_prekey = true;
    }

    return has_prekey;
}

auto validate_legacy_encrypted_envelope(xmpp_ctx_t *ctx,
                                        std::string_view xml,
                                        bool allow_key_transport) -> bool
{
    xmpp_stanza_t *root = xmpp_stanza_new_from_string(ctx, std::string(xml).c_str());
    if (!root)
        return false;

    xmpp_stanza_t *encrypted = xmpp_stanza_get_child_by_name_and_ns(
        root, "encrypted", "eu.siacs.conversations.axolotl");
    if (!encrypted)
        return false;

    xmpp_stanza_t *header = xmpp_stanza_get_child_by_name(encrypted, "header");
    if (!header)
        return false;
    if (!parse_device_id(xmpp_stanza_get_attribute(header, "sid")))
        return false;

    bool has_key = false;
    for (xmpp_stanza_t *child = xmpp_stanza_get_children(header); child; child = xmpp_stanza_get_next(child))
    {
        const char *name = xmpp_stanza_get_name(child);
        if (!name)
            continue;

        if (std::strcmp(name, "key") == 0)
        {
            has_key = true;
            if (!parse_device_id(xmpp_stanza_get_attribute(child, "rid")))
                return false;
            if (!has_non_empty_text(child))
                return false;
        }
    }
    if (!has_key)
        return false;

    xmpp_stanza_t *payload = xmpp_stanza_get_child_by_name(encrypted, "payload");
    if (payload)
        return has_non_empty_text(payload);

    xmpp_stanza_t *iv = xmpp_stanza_get_child_by_name(header, "iv");
    if (!iv || !has_non_empty_text(iv))
        return false;

    return allow_key_transport;
}

}  // namespace

TEST_CASE("OMEMO XEP-0384 envelope compliance")
{
    strophe_env env;
    REQUIRE(env.ctx != nullptr);

    static constexpr std::string_view own_bare = "alice@example.org";
    static constexpr std::uint32_t own_device_id = 1111111U;

    SUBCASE("accepts valid OMEMO:2 encrypted message")
    {
        static constexpr const char *xml =
            "<message from='bob@example.org/phone' to='alice@example.org' type='chat'>"
            "<encrypted xmlns='urn:xmpp:omemo:2'>"
            "<header sid='2222222'>"
            "<keys jid='alice@example.org'>"
            "<key rid='1111111' kex='true'>QUJDRA==</key>"
            "</keys>"
            "</header>"
            "<payload>RkFLRV9QTEFJTlRFWFQ=</payload>"
            "</encrypted>"
            "</message>";

        CHECK(validate_omemo2_encrypted_envelope(env.ctx, xml, own_bare, own_device_id, true));
    }

    SUBCASE("accepts valid KeyTransportElement without payload")
    {
        static constexpr const char *xml =
            "<message from='bob@example.org/phone' to='alice@example.org' type='chat'>"
            "<encrypted xmlns='urn:xmpp:omemo:2'>"
            "<header sid='2222222'>"
            "<keys jid='alice@example.org'>"
            "<key rid='1111111' kex='true'>QUJDRA==</key>"
            "</keys>"
            "</header>"
            "</encrypted>"
            "</message>";

        CHECK(validate_omemo2_encrypted_envelope(env.ctx, xml, own_bare, own_device_id, true));
    }

    SUBCASE("rejects payloadless OMEMO:2 key transport without kex key")
    {
        static constexpr const char *xml =
            "<message from='bob@example.org/phone' to='alice@example.org' type='chat'>"
            "<encrypted xmlns='urn:xmpp:omemo:2'>"
            "<header sid='2222222'>"
            "<keys jid='alice@example.org'>"
            "<key rid='1111111'>QUJDRA==</key>"
            "</keys>"
            "</header>"
            "</encrypted>"
            "</message>";

        CHECK_FALSE(validate_omemo2_encrypted_envelope(env.ctx, xml, own_bare, own_device_id, true));
    }

    SUBCASE("rejects keys jid containing a resource")
    {
        static constexpr const char *xml =
            "<message from='bob@example.org/phone' to='alice@example.org' type='chat'>"
            "<encrypted xmlns='urn:xmpp:omemo:2'>"
            "<header sid='2222222'>"
            "<keys jid='alice@example.org/laptop'>"
            "<key rid='1111111'>QUJDRA==</key>"
            "</keys>"
            "</header>"
            "<payload>RkFLRV9QTEFJTlRFWFQ=</payload>"
            "</encrypted>"
            "</message>";

        CHECK_FALSE(validate_omemo2_encrypted_envelope(env.ctx, xml, own_bare, own_device_id, true));
    }

    SUBCASE("rejects missing keys wrapper")
    {
        static constexpr const char *xml =
            "<message from='bob@example.org/phone' to='alice@example.org' type='chat'>"
            "<encrypted xmlns='urn:xmpp:omemo:2'>"
            "<header sid='2222222'>"
            "<iv>AAECAwQ=</iv>"
            "</header>"
            "<payload>RkFLRV9QTEFJTlRFWFQ=</payload>"
            "</encrypted>"
            "</message>";

        CHECK_FALSE(validate_omemo2_encrypted_envelope(env.ctx, xml, own_bare, own_device_id, true));
    }

    SUBCASE("rejects missing sid")
    {
        static constexpr const char *xml =
            "<message from='bob@example.org/phone' to='alice@example.org' type='chat'>"
            "<encrypted xmlns='urn:xmpp:omemo:2'>"
            "<header>"
            "<keys jid='alice@example.org'>"
            "<key rid='1111111'>QUJDRA==</key>"
            "</keys>"
            "</header>"
            "<payload>RkFLRV9QTEFJTlRFWFQ=</payload>"
            "</encrypted>"
            "</message>";

        CHECK_FALSE(validate_omemo2_encrypted_envelope(env.ctx, xml, own_bare, own_device_id, true));
    }

    SUBCASE("rejects key element missing rid")
    {
        static constexpr const char *xml =
            "<message from='bob@example.org/phone' to='alice@example.org' type='chat'>"
            "<encrypted xmlns='urn:xmpp:omemo:2'>"
            "<header sid='2222222'>"
            "<keys jid='alice@example.org'>"
            "<key>QUJDRA==</key>"
            "</keys>"
            "</header>"
            "<payload>RkFLRV9QTEFJTlRFWFQ=</payload>"
            "</encrypted>"
            "</message>";

        CHECK_FALSE(validate_omemo2_encrypted_envelope(env.ctx, xml, own_bare, own_device_id, true));
    }
}

TEST_CASE("OMEMO legacy siacs compatibility envelope compliance")
{
    strophe_env env;
    REQUIRE(env.ctx != nullptr);

    SUBCASE("accepts valid legacy encrypted message with payload")
    {
        static constexpr const char *xml =
            "<message from='bob@example.org/phone' to='alice@example.org' type='chat'>"
            "<encrypted xmlns='eu.siacs.conversations.axolotl'>"
            "<header sid='2222222'>"
            "<key rid='1111111' prekey='true'>QUJDRA==</key>"
            "<iv>AAECAwQFBgcICQoL</iv>"
            "</header>"
            "<payload>RkFLRV9QTEFJTlRFWFQ=</payload>"
            "</encrypted>"
            "</message>";

        CHECK(validate_legacy_encrypted_envelope(env.ctx, xml, true));
    }

    SUBCASE("accepts legacy key transport without payload when iv is present")
    {
        static constexpr const char *xml =
            "<message from='bob@example.org/phone' to='alice@example.org' type='chat'>"
            "<encrypted xmlns='eu.siacs.conversations.axolotl'>"
            "<header sid='2222222'>"
            "<key rid='1111111' prekey='true'>QUJDRA==</key>"
            "<iv>AAECAwQFBgcICQoL</iv>"
            "</header>"
            "</encrypted>"
            "</message>";

        CHECK(validate_legacy_encrypted_envelope(env.ctx, xml, true));
    }

    SUBCASE("rejects legacy payloadless message without iv")
    {
        static constexpr const char *xml =
            "<message from='bob@example.org/phone' to='alice@example.org' type='chat'>"
            "<encrypted xmlns='eu.siacs.conversations.axolotl'>"
            "<header sid='2222222'>"
            "<key rid='1111111'>QUJDRA==</key>"
            "</header>"
            "</encrypted>"
            "</message>";

        CHECK_FALSE(validate_legacy_encrypted_envelope(env.ctx, xml, true));
    }
}

TEST_CASE("OMEMO legacy siacs bundle compatibility compliance")
{
    strophe_env env;
    REQUIRE(env.ctx != nullptr);

    SUBCASE("accepts profanity/conversations style legacy bundle")
    {
        static constexpr const char *xml =
            "<item id='current'>"
            "<bundle xmlns='eu.siacs.conversations.axolotl'>"
            "<signedPreKeyPublic signedPreKeyId='1234'>QUJDRA==</signedPreKeyPublic>"
            "<signedPreKeySignature>Rk9PQkFS</signedPreKeySignature>"
            "<identityKey>SUtLRVk=</identityKey>"
            "<prekeys xmlns='eu.siacs.conversations.axolotl'>"
            "<preKeyPublic preKeyId='1001'>UEsx</preKeyPublic>"
            "<preKeyPublic preKeyId='1002'>UEsy</preKeyPublic>"
            "</prekeys>"
            "</bundle>"
            "</item>";

        CHECK(validate_legacy_bundle(env.ctx, xml));
    }

    SUBCASE("rejects legacy bundle with missing signedPreKeyId")
    {
        static constexpr const char *xml =
            "<item id='current'>"
            "<bundle xmlns='eu.siacs.conversations.axolotl'>"
            "<signedPreKeyPublic>QUJDRA==</signedPreKeyPublic>"
            "<signedPreKeySignature>Rk9PQkFS</signedPreKeySignature>"
            "<identityKey>SUtLRVk=</identityKey>"
            "<prekeys xmlns='eu.siacs.conversations.axolotl'>"
            "<preKeyPublic preKeyId='1001'>UEsx</preKeyPublic>"
            "</prekeys>"
            "</bundle>"
            "</item>";

        CHECK_FALSE(validate_legacy_bundle(env.ctx, xml));
    }

    SUBCASE("rejects legacy bundle with missing prekeys")
    {
        static constexpr const char *xml =
            "<item id='current'>"
            "<bundle xmlns='eu.siacs.conversations.axolotl'>"
            "<signedPreKeyPublic signedPreKeyId='1234'>QUJDRA==</signedPreKeyPublic>"
            "<signedPreKeySignature>Rk9PQkFS</signedPreKeySignature>"
            "<identityKey>SUtLRVk=</identityKey>"
            "</bundle>"
            "</item>";

        CHECK_FALSE(validate_legacy_bundle(env.ctx, xml));
    }
}