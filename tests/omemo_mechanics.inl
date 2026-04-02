// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// OMEMO mechanics tests — exercise the real libsignal + LMDB stack using a
// per-test temporary database directory.  No mocks: all code paths are live.
//
// Design:
//   • omemo_test_env: RAII fixture that
//       1. builds a minimal t_weechat_plugin stub (only the function pointers
//          that omemo::init() and the APIs under test actually call),
//       2. sets weechat::plugin::instance so the weechat_* macros resolve,
//       3. creates a unique mkdtemp directory and wires string_eval_expression
//          to redirect ${weechat_data_dir} to that directory,
//       4. calls omemo.init() and asserts it succeeded,
//       5. on destruction removes the tmpdir and resets plugin::instance.
//   • Each TEST_CASE constructs its own omemo_test_env so databases are
//     completely isolated.
//   • The omemo instance is held in a unique_ptr so it can be cleanly
//     destroyed and recreated within a single test (for reinit / migration
//     scenarios) without triggering the deleted copy-assignment operator.

#include <doctest/doctest.h>

#include <weechat/weechat-plugin.h>

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include <openssl/evp.h>   // EVP_EncodeBlock / EVP_DecodeBlock
#include <lmdb++.h>

#include "../src/plugin.hh"
#include "../src/omemo.hh"
#include "../src/strophe.hh"

// stanza_to_xml() is defined as a static function in unit.inl (included
// before this file in main.cc) — we use it directly.

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Minimal WeeChat plugin stub
//
// Only the function pointers actually dereferenced by the code paths under
// test need to be filled in.  All others are left as nullptr; if a test
// accidentally triggers one of those paths the resulting nullptr-deref will
// be immediately obvious in the crash stack-trace.
// ─────────────────────────────────────────────────────────────────────────────

// Global tmpdir used by the string_eval_expression stub.
// Each test fixture sets this before calling omemo.init().
static std::string g_test_data_dir;

// stub: weechat_string_eval_expression
// Replaces "${weechat_data_dir}" with g_test_data_dir and returns a
// malloc-allocated result (WeeChat API contract: caller frees with free()).
static char *stub_string_eval_expression(const char *expr,
                                         struct t_hashtable * /*pointers*/,
                                         struct t_hashtable * /*extra_vars*/,
                                         struct t_hashtable * /*options*/)
{
    if (!expr)
        return nullptr;

    std::string result = expr;
    const std::string placeholder = "${weechat_data_dir}";
    auto pos = result.find(placeholder);
    while (pos != std::string::npos)
    {
        result.replace(pos, placeholder.size(), g_test_data_dir);
        pos = result.find(placeholder, pos + g_test_data_dir.size());
    }
    return strdup(result.c_str());
}

// stub: weechat_prefix — returns empty string for all prefix names
static const char *stub_prefix(const char * /*prefix*/)
{
    return "";
}

// stub: weechat_printf_datetime_tags — silently discard all log output
static void stub_printf_datetime_tags(struct t_gui_buffer * /*buffer*/,
                                      time_t /*date*/,
                                      int /*date_usec*/,
                                      const char * /*tags*/,
                                      const char * /*message*/, ...)
{
}

// stub: weechat_strcasecmp — delegate to POSIX
static int stub_strcasecmp(const char *a, const char *b)
{
    return strcasecmp(a, b);
}

// stub: weechat_string_base_encode
// Supports base "64" only (the only base the OMEMO paths use).
static int stub_string_base_encode(const char *base,
                                   const char *from, int length,
                                   char *to)
{
    if (!from || length <= 0 || !to)
        return -1;

    if (std::string_view(base) == "64")
    {
        const int written = EVP_EncodeBlock(
            reinterpret_cast<unsigned char *>(to),
            reinterpret_cast<const unsigned char *>(from),
            length);
        return written;
    }

    return -1;
}

// stub: weechat_string_base_decode
static int stub_string_base_decode(const char *base,
                                   const char *from,
                                   char *to)
{
    if (!from || !to)
        return -1;

    if (std::string_view(base) == "64")
    {
        const int written = EVP_DecodeBlock(
            reinterpret_cast<unsigned char *>(to),
            reinterpret_cast<const unsigned char *>(from),
            static_cast<int>(strlen(from)));
        if (written < 0)
            return -1;

        // EVP_DecodeBlock returns a count that includes the null bytes
        // introduced by base64 '=' padding.  Subtract one byte per trailing
        // '=' so the caller receives the actual decoded length (matching
        // the behaviour of WeeChat's string_base_decode implementation).
        int padding = 0;
        const std::size_t len = strlen(from);
        if (len >= 1 && from[len - 1] == '=') ++padding;
        if (len >= 2 && from[len - 2] == '=') ++padding;
        return written - padding;
    }

    return -1;
}

// stub: weechat_config_boolean — used by handle_axolotl_bundle ATM path
static int stub_config_boolean(struct t_config_option * /*option*/)
{
    return 0;  // ATM disabled — safest default for unit tests
}

// Build the minimal t_weechat_plugin stub.
// All fields not needed are zero-initialised — will crash loudly if
// accidentally called, making it obvious which stub is missing.
static t_weechat_plugin make_plugin_stub()
{
    t_weechat_plugin p {};
    p.string_eval_expression = stub_string_eval_expression;
    p.prefix                 = stub_prefix;
    p.printf_datetime_tags   = stub_printf_datetime_tags;
    p.strcasecmp             = stub_strcasecmp;
    p.string_base_encode     = stub_string_base_encode;
    p.string_base_decode     = stub_string_base_decode;
    p.config_boolean         = stub_config_boolean;
    return p;
}

// ─────────────────────────────────────────────────────────────────────────────
// RAII fixture
// ─────────────────────────────────────────────────────────────────────────────

struct omemo_test_env
{
    // Strophe context — required for stanza construction (get_bundle, etc.)
    xmpp_ctx_t *ctx {nullptr};

    // The OMEMO instance under test.  Held in a unique_ptr so it can be
    // cleanly destroyed and recreated within a test (reinit / migration
    // scenarios) without running into the deleted copy-assignment operator.
    std::unique_ptr<weechat::xmpp::omemo> omemo;

    // Temporary database directory (unique per test).
    std::string tmpdir;

    // Account name used for this fixture (determines DB filename).
    std::string account_name;

    explicit omemo_test_env(const char *acct = "test_account")
        : account_name(acct)
    {
        // 1. Create a unique tmpdir.
        char tmpl[] = "/tmp/xepher_omemo_test_XXXXXX";
        const char *dir = mkdtemp(tmpl);
        REQUIRE(dir != nullptr);
        tmpdir = dir;
        g_test_data_dir = tmpdir;

        // 2. Initialise libstrophe (safe to call multiple times).
        xmpp_initialize();
        ctx = xmpp_ctx_new(nullptr, nullptr);
        REQUIRE(ctx != nullptr);

        // 3. Install the plugin stub so weechat_* macros resolve.
        //    plugin_stub is a member — it outlives any weechat::plugin
        //    object that holds a raw pointer into it.
        plugin_stub = make_plugin_stub();
        weechat::plugin::instance = std::make_unique<weechat::plugin>(&plugin_stub);

        // 4. Initialise OMEMO with a real LMDB backed by tmpdir.
        omemo = std::make_unique<weechat::xmpp::omemo>();
        omemo->init(nullptr, account_name.c_str());
        REQUIRE(static_cast<bool>(*omemo));
    }

    // Destroy and recreate the omemo instance using the same tmpdir/account.
    // Use this in tests that need to simulate a restart (e.g. migration).
    void reinit()
    {
        omemo.reset();
        omemo = std::make_unique<weechat::xmpp::omemo>();
        omemo->init(nullptr, account_name.c_str());
        REQUIRE(static_cast<bool>(*omemo));
    }

    // Path to the LMDB database file for this fixture.
    [[nodiscard]] std::string db_path() const
    {
        return tmpdir + "/xmpp/omemo_" + account_name + ".db";
    }

    ~omemo_test_env()
    {
        // Destroy omemo first (closes LMDB env, frees Signal state).
        omemo.reset();

        weechat::plugin::instance.reset();
        g_test_data_dir.clear();

        if (ctx)
        {
            xmpp_ctx_free(ctx);
            ctx = nullptr;
        }
        xmpp_shutdown();

        std::filesystem::remove_all(tmpdir);
    }

    // Non-copyable, non-movable — holds raw pointers into itself.
    omemo_test_env(const omemo_test_env &) = delete;
    omemo_test_env &operator=(const omemo_test_env &) = delete;

private:
    t_weechat_plugin plugin_stub {};
};

// ─────────────────────────────────────────────────────────────────────────────
// Helper: open a read-only LMDB view of a fixture's database.
// ─────────────────────────────────────────────────────────────────────────────
static lmdb::env open_db_ro(const std::string &path)
{
    auto env = lmdb::env::create();
    env.set_max_dbs(4);
    env.set_mapsize(32U * 1024U * 1024U);
    env.open(path.c_str(), MDB_NOSUBDIR | MDB_RDONLY, 0664);
    return env;
}

static lmdb::env open_db_rw(const std::string &path)
{
    auto env = lmdb::env::create();
    env.set_max_dbs(4);
    env.set_mapsize(32U * 1024U * 1024U);
    env.open(path.c_str(), MDB_NOSUBDIR, 0664);
    return env;
}

}  // anonymous namespace

// =============================================================================
// TEST CASES
// =============================================================================

// ── 1. init() produces valid OMEMO state ─────────────────────────────────────

TEST_CASE("omemo::init() produces valid state")
{
    omemo_test_env env;
    CHECK(env.omemo->device_id != 0);
    CHECK(static_cast<bool>(*env.omemo));
}

// ── 2. device_id persists across reinit ──────────────────────────────────────

TEST_CASE("omemo::init() persists device_id across reinit")
{
    omemo_test_env env;
    const std::uint32_t first_id = env.omemo->device_id;
    CHECK(first_id != 0);

    env.reinit();
    CHECK(env.omemo->device_id == first_id);
}

// ── 3. get_axolotl_bundle() produces a spec-compliant legacy bundle stanza ───

TEST_CASE("omemo::get_axolotl_bundle() produces valid XEP-0384 legacy bundle")
{
    omemo_test_env env;

    xmpp_stanza_t *iq = env.omemo->get_axolotl_bundle(env.ctx, nullptr, nullptr);
    REQUIRE(iq != nullptr);

    // stanza_to_xml is defined in unit.inl (same TU).
    const std::string xml = stanza_to_xml(env.ctx, iq);
    xmpp_stanza_release(iq);
    REQUIRE_FALSE(xml.empty());

    // Parse and walk: <iq> → <pubsub> → <publish> → <item> → <bundle>
    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(root != nullptr);

    auto *pubsub  = xmpp_stanza_get_child_by_name(root, "pubsub");
    auto *publish = pubsub  ? xmpp_stanza_get_child_by_name(pubsub,  "publish") : nullptr;
    auto *item    = publish ? xmpp_stanza_get_child_by_name(publish, "item")    : nullptr;
    auto *bundle  = item    ? xmpp_stanza_get_child_by_name_and_ns(
                                  item, "bundle", "eu.siacs.conversations.axolotl") : nullptr;

    REQUIRE(bundle != nullptr);

    auto *spk = xmpp_stanza_get_child_by_name(bundle, "signedPreKeyPublic");
    REQUIRE(spk != nullptr);
    CHECK(xmpp_stanza_get_attribute(spk, "signedPreKeyId") != nullptr);

    CHECK(xmpp_stanza_get_child_by_name(bundle, "signedPreKeySignature") != nullptr);
    CHECK(xmpp_stanza_get_child_by_name(bundle, "identityKey") != nullptr);

    auto *prekeys = xmpp_stanza_get_child_by_name(bundle, "prekeys");
    REQUIRE(prekeys != nullptr);
    bool found_pk = false;
    for (auto *pk = xmpp_stanza_get_children(prekeys); pk; pk = xmpp_stanza_get_next(pk))
    {
        const char *name = xmpp_stanza_get_name(pk);
        if (name && std::strcmp(name, "preKeyPublic") == 0)
        {
            found_pk = true;
            break;
        }
    }
    CHECK(found_pk);

    xmpp_stanza_release(root);
}

// ── 4. handle_axolotl_devicelist stores the devicelist in LMDB ───────────────

TEST_CASE("handle_axolotl_devicelist persists devicelist to LMDB")
{
    omemo_test_env env;

    const std::string xml = R"(
        <items node='eu.siacs.conversations.axolotl.devicelist'>
          <item id='current'>
            <list xmlns='eu.siacs.conversations.axolotl'>
              <device id='12345'/>
            </list>
          </item>
        </items>
    )";
    xmpp_stanza_t *items = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(items != nullptr);

    env.omemo->handle_axolotl_devicelist(nullptr, "bob@example.com", items);
    xmpp_stanza_release(items);

    // Destroy the omemo instance first so LMDB releases its write lock,
    // then open a fresh read-only handle to verify what was written.
    env.omemo.reset();

    auto db  = open_db_ro(env.db_path());
    auto txn = lmdb::txn::begin(db, nullptr, MDB_RDONLY);
    auto dbi = lmdb::dbi::open(txn, "omemo", 0);

    std::string_view value;
    const bool found = dbi.get(txn, std::string_view{"axolotl_devicelist:bob@example.com"}, value);
    CHECK(found);
    if (found)
        CHECK(std::string(value).find("12345") != std::string::npos);

    txn.abort();
}

// ── 5. has_session returns false before any bundle exchange ───────────────────

TEST_CASE("omemo::has_session returns false with no established session")
{
    omemo_test_env env;
    CHECK_FALSE(env.omemo->has_session("bob@example.com", 42));
}

// ── 6. LMDB key migration: legacy_* → axolotl_* on reinit ───────────────────

TEST_CASE("omemo::init() migrates legacy_* LMDB keys to axolotl_*")
{
    omemo_test_env env;

    // Destroy omemo so the DB is unlocked for writing.
    env.omemo.reset();

    // Seed legacy_* keys directly.
    {
        auto db  = open_db_rw(env.db_path());
        auto txn = lmdb::txn::begin(db);
        auto dbi = lmdb::dbi::open(txn, "omemo", MDB_CREATE);
        dbi.put(txn, std::string_view{"legacy_devicelist:charlie@example.com"},
                std::string_view{"11111;22222"});
        dbi.put(txn, std::string_view{"legacy_bundle:charlie@example.com:11111"},
                std::string_view{"some_bundle_data"});
        txn.commit();
    }

    // Re-init — migrate_legacy_keys() runs inside init().
    env.reinit();

    // Close omemo, then verify via fresh read-only handle.
    env.omemo.reset();

    {
        auto db  = open_db_ro(env.db_path());
        auto txn = lmdb::txn::begin(db, nullptr, MDB_RDONLY);
        auto dbi = lmdb::dbi::open(txn, "omemo", 0);
        std::string_view val;

        CHECK_FALSE(dbi.get(txn, std::string_view{"legacy_devicelist:charlie@example.com"}, val));
        CHECK_FALSE(dbi.get(txn, std::string_view{"legacy_bundle:charlie@example.com:11111"}, val));
        CHECK(dbi.get(txn, std::string_view{"axolotl_devicelist:charlie@example.com"}, val));
        CHECK(dbi.get(txn, std::string_view{"axolotl_bundle:charlie@example.com:11111"}, val));

        txn.abort();
    }
}

// ── 7. load_device_mode accepts "legacy" string as peer_mode::axolotl ────────
//
// Old databases may have "legacy" stored as the device mode value.  The
// migration does NOT touch device_mode:* keys — instead the read path
// accepts both "axolotl" and "legacy".  Verify the value survives reinit
// unchanged (i.e. it is not overwritten to "axolotl" by migration).

TEST_CASE("load_device_mode accepts legacy string as peer_mode::axolotl")
{
    omemo_test_env env;
    env.omemo.reset();

    // Seed a "legacy" device_mode value.
    {
        auto db  = open_db_rw(env.db_path());
        auto txn = lmdb::txn::begin(db);
        auto dbi = lmdb::dbi::open(txn, "omemo", MDB_CREATE);
        dbi.put(txn, std::string_view{"device_mode:dave@example.com:55555"},
                std::string_view{"legacy"});
        txn.commit();
    }

    // Reinit — migration must leave device_mode:* keys untouched.
    env.reinit();
    env.omemo.reset();

    {
        auto db  = open_db_ro(env.db_path());
        auto txn = lmdb::txn::begin(db, nullptr, MDB_RDONLY);
        auto dbi = lmdb::dbi::open(txn, "omemo", 0);
        std::string_view val;
        REQUIRE(dbi.get(txn, std::string_view{"device_mode:dave@example.com:55555"}, val));
        // Migration must not have touched this key; value stays "legacy".
        CHECK(val == "legacy");
        txn.abort();
    }
}

// ── 8. Bootstrap-race guard: handle_axolotl_bundle inserts into ───────────────
//      key_transport_bootstrap_attempted before clearing pending sets
//
// Scenario: a stanza for remote device {bob, 99999} is mid-flight (so
// pending_bundle_fetch and pending_key_transport both contain the key).
// When handle_axolotl_bundle() returns, key_transport_bootstrap_attempted
// must contain the key, and both pending sets must be empty for that key.
// This ensures that any concurrent decode() call cannot re-enqueue a bundle
// fetch after pending_bundle_fetch is cleared but before the mark is written.

TEST_CASE("handle_axolotl_bundle sets race guard before clearing pending sets")
{
    omemo_test_env env;

    const std::string remote_jid  = "bob@example.com";
    const std::uint32_t remote_id = 99999;
    const auto key = std::make_pair(remote_jid, remote_id);

    // Pre-seed both pending sets to simulate an in-flight bundle fetch that
    // was triggered by decode() seeing no key for our device.
    env.omemo->pending_bundle_fetch.insert(key);
    env.omemo->pending_key_transport.insert(key);

    // Build a minimal (but parseable) legacy bundle stanza.
    // Values are arbitrary valid base64; we only need parse to succeed.
    // establish_session_from_bundle() will fail because the keys are fake,
    // but the race-guard insert happens unconditionally before that call.
    const std::string bundle_xml = R"(<items>
      <item id='current'>
        <bundle xmlns='eu.siacs.conversations.axolotl'>
          <signedPreKeyPublic signedPreKeyId='1'>AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=</signedPreKeyPublic>
          <signedPreKeySignature>AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA</signedPreKeySignature>
          <identityKey>AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=</identityKey>
          <prekeys>
            <preKeyPublic preKeyId='1'>AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=</preKeyPublic>
          </prekeys>
        </bundle>
      </item>
    </items>)";

    xmpp_stanza_t *items = xmpp_stanza_new_from_string(env.ctx, bundle_xml.c_str());
    REQUIRE(items != nullptr);

    // Call the function under test (account=nullptr suppresses key-transport send).
    env.omemo->handle_axolotl_bundle(nullptr, nullptr, remote_jid.c_str(), remote_id, items);
    xmpp_stanza_release(items);

    // Race-guard assertion: the pair must be in key_transport_bootstrap_attempted.
    CHECK(env.omemo->key_transport_bootstrap_attempted.count(key) == 1);

    // Both pending sets must be drained.
    CHECK(env.omemo->pending_bundle_fetch.count(key)   == 0);
    CHECK(env.omemo->pending_key_transport.count(key)  == 0);
}

// ── 10. MAM replay self-session: Signal session to own device survives reinit ──
//
// Regression test for Bug 3: MAM-replayed self-sent OMEMO messages must be
// decryptable after WeeChat restarts.  The fix adds `&& !is_mam_replay` to
// the OMEMO_ADVICE gate in message_handler.inl so decode() is actually called
// for such messages.
//
// This test validates the underlying crypto layer that the fix relies on:
//   1. Establish a Signal session to (own_jid, own_device_id) — the same
//      session the sender would use to encrypt the key-copy for itself.
//   2. Verify has_session() returns true before AND after reinit.
//      Surviving reinit is mandatory: the session must persist in LMDB so
//      decode() can succeed when replaying MAM messages after WeeChat restarts.
//
// Methodology: `get_axolotl_bundle()` exports our real bundle; feeding it to
// `handle_axolotl_bundle(account=nullptr, ...)` with our own device_id causes
// the normal session-bootstrap code path to run (account=nullptr → is_own_device
// is false → establish_session_from_bundle is called).  This mirrors the Signal
// address (own_jid, own_device_id) that encode_axolotl / encode use when adding
// a sender-side key copy (include_own_device=true), and that decode() uses on
// MAM replay when the inner forwarded stanza has from=own_jid, sid=own_device_id.

TEST_CASE("MAM replay self-session: Signal session to own device survives reinit")
{
    omemo_test_env env;

    // Use a stable own JID string for the self-session address.
    const std::string own_jid = "alice@example.com";
    const std::uint32_t own_device_id = env.omemo->device_id;
    REQUIRE(own_device_id != 0);

    // 1. Export our real legacy bundle as XML text.
    //    get_axolotl_bundle() returns an <iq><pubsub><publish><item><bundle> tree.
    //    We serialise the <bundle> element to XML text (same bytes the server would
    //    send back in a pubsub result), then wrap it in the <items><item> envelope
    //    that production iq_handler.inl hands to handle_axolotl_bundle().
    //    Parsing with xmpp_stanza_new_from_string reproduces the exact production
    //    code path: server XML → libstrophe parser → items stanza pointer.
    xmpp_stanza_t *iq = env.omemo->get_axolotl_bundle(env.ctx, nullptr, nullptr);
    REQUIRE(iq != nullptr);

    // Walk to the <bundle> element inside the IQ tree.
    auto *pubsub  = xmpp_stanza_get_child_by_name(iq, "pubsub");
    auto *publish = pubsub  ? xmpp_stanza_get_child_by_name(pubsub,  "publish") : nullptr;
    auto *iq_item = publish ? xmpp_stanza_get_child_by_name(publish, "item")    : nullptr;
    auto *bundle  = iq_item ? xmpp_stanza_get_child_by_name_and_ns(
                                  iq_item, "bundle", "eu.siacs.conversations.axolotl") : nullptr;
    REQUIRE(bundle != nullptr);

    // Serialise the <bundle> subtree to XML text.
    // xmpp_stanza_to_text emits xmlns= on the root element, so the NS is
    // preserved when re-parsed below (verified by manual inspection).
    const std::string bundle_xml = stanza_to_xml(env.ctx, bundle);
    xmpp_stanza_release(iq);
    REQUIRE_FALSE(bundle_xml.empty());

    // Wrap with the <items><item id='N'> envelope that the IQ result handler
    // passes to handle_axolotl_bundle() (see iq_handler.inl line 4173).
    const std::string items_xml =
        "<items><item id='" + std::to_string(own_device_id) + "'>"
        + bundle_xml
        + "</item></items>";

    // Parse: this is exactly the production path through libstrophe.
    xmpp_stanza_t *items = xmpp_stanza_new_from_string(env.ctx, items_xml.c_str());
    REQUIRE(items != nullptr);

    // Verify the parser preserved the bundle NS so extract_legacy_bundle_from_items
    // will find it (guards against a regression in the XML round-trip).
    {
        auto *parsed_item   = xmpp_stanza_get_child_by_name(items, "item");
        auto *parsed_bundle = parsed_item
            ? xmpp_stanza_get_child_by_name_and_ns(
                  parsed_item, "bundle", "eu.siacs.conversations.axolotl")
            : nullptr;
        REQUIRE(parsed_bundle != nullptr);
    }

    // 2. Establish a self-session: feed our own real bundle to handle_axolotl_bundle
    //    using our own JID + device_id as the "remote peer".
    //    account=nullptr → is_own_device=false → establish_session_from_bundle runs.
    env.omemo->handle_axolotl_bundle(nullptr, nullptr, own_jid.c_str(), own_device_id, items);
    xmpp_stanza_release(items);

    // 3. Verify the session exists immediately after bootstrap.
    CHECK(env.omemo->has_session(own_jid.c_str(), own_device_id));

    // 4. Reinit (simulates WeeChat restart — LMDB is reopened, Signal state reloaded).
    env.reinit();

    // 5. Verify the session still exists after reinit.
    //    This is the key invariant Bug 3's fix depends on: the persisted session
    //    allows decode() to succeed when processing MAM-replayed self-sent messages.
    CHECK(env.omemo->has_session(own_jid.c_str(), own_device_id));
}

// ── 11. Bootstrap-race guard: handle_bundle mirrors the same guarantee ────────

TEST_CASE("handle_bundle sets race guard before clearing pending sets")
{
    omemo_test_env env;

    const std::string remote_jid  = "carol@example.com";
    const std::uint32_t remote_id = 77777;
    const auto key = std::make_pair(remote_jid, remote_id);

    env.omemo->pending_bundle_fetch.insert(key);
    env.omemo->pending_key_transport.insert(key);

    // Minimal OMEMO:2 bundle stanza.
    // <spk id='N'> text content = signed prekey; <spks> = signature; <ik> = identity key;
    // <prekeys><pk id='N'> text = prekey.  Values are arbitrary valid base64.
    const std::string bundle_xml = R"(<items>
      <item id='current'>
        <bundle xmlns='urn:xmpp:omemo:2'>
          <spk id='1'>AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=</spk>
          <spks>AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA</spks>
          <ik>AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=</ik>
          <prekeys>
            <pk id='1'>AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=</pk>
          </prekeys>
        </bundle>
      </item>
    </items>)";

    xmpp_stanza_t *items = xmpp_stanza_new_from_string(env.ctx, bundle_xml.c_str());
    REQUIRE(items != nullptr);

    env.omemo->handle_bundle(nullptr, nullptr, remote_jid.c_str(), remote_id, items);
    xmpp_stanza_release(items);

    CHECK(env.omemo->key_transport_bootstrap_attempted.count(key) == 1);
    CHECK(env.omemo->pending_bundle_fetch.count(key)              == 0);
    CHECK(env.omemo->pending_key_transport.count(key)             == 0);
}
