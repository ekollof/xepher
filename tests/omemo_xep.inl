#include <doctest/doctest.h>

#include <strophe.h>

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>

#include "../src/xmpp/node.hh"

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
    return stanza && !stanza_element_text(stanza).empty();
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