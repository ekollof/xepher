// dump_omemo_db.cpp — Standalone C++23 inspector for the OMEMO LMDB database.
//
// Usage:
//   dump_omemo_db [--db <path>] [--prefix <str>]
//
// Defaults:
//   --db  ~/.local/share/weechat/xmpp/omemo_<account>.db
//         (no default account; --db is required if the default path is not used)
//   The default path tries $HOME/.local/share/weechat/xmpp/omemo_<account>.db
//   where <account> is taken from the environment variable XMPP_ACCOUNT.
//   Pass --db explicitly to override.
//
// Build via `make tools` or `make -C tools` (CMake).

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <lmdb.h>

// ---------------------------------------------------------------------------
// Key-prefix catalogue — mirrors internal_prelude.inl constants exactly.
// ---------------------------------------------------------------------------
static constexpr std::string_view kPfxSession        = "session:";
static constexpr std::string_view kPfxIdentity       = "identity:";
static constexpr std::string_view kPfxPrekey         = "prekey:";
static constexpr std::string_view kPfxSignedPrekey   = "signed-prekey:";
static constexpr std::string_view kPfxBundle         = "bundle:";
static constexpr std::string_view kPfxLegacyBundle   = "legacy_bundle:";
static constexpr std::string_view kPfxDeviceMode     = "device_mode:";
static constexpr std::string_view kPfxDevicelist     = "devicelist:";
// "legacy_devicelist:" will become "axolotl_devicelist:" after the rename
// migration, but we recognise both so the tool works on old and new databases.
static constexpr std::string_view kPfxLegacyDL       = "legacy_devicelist:";
static constexpr std::string_view kPfxAxolotlDL      = "axolotl_devicelist:";
static constexpr std::string_view kPfxAtmTrust       = "atm_trust:";
static constexpr std::string_view kPfxSenderKey      = "senderkey:";

// Fixed own-key constants (no prefix — exact key match)
static constexpr std::string_view kIdentityPublic    = "identity:public";
static constexpr std::string_view kIdentityPrivate   = "identity:private";
static constexpr std::string_view kSignedPreKeyId    = "signed_pre_key:id";
static constexpr std::string_view kSignedPreKeyRecord = "signed_pre_key:record";
static constexpr std::string_view kSignedPreKeyPublic = "signed_pre_key:public";
static constexpr std::string_view kSignedPreKeySig   = "signed_pre_key:signature";
static constexpr std::string_view kPrekeys           = "prekeys";
static constexpr std::string_view kDeviceIdKey       = "device_id";
static constexpr std::string_view kRegistrationId    = "registration_id";

// ---------------------------------------------------------------------------
// Utility helpers
// ---------------------------------------------------------------------------

static auto to_hex(std::span<const std::uint8_t> bytes) -> std::string
{
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (const auto b : bytes)
        oss << std::setw(2) << static_cast<unsigned>(b);
    return oss.str();
}

static auto to_hex(std::string_view sv) -> std::string
{
    const auto *p = reinterpret_cast<const std::uint8_t *>(sv.data());
    return to_hex(std::span<const std::uint8_t>(p, sv.size()));
}

static auto is_printable(std::string_view sv) -> bool
{
    for (const unsigned char c : sv)
        if (c < 0x20 || c > 0x7e)
            return false;
    return true;
}

static auto format_value(std::string_view sv) -> std::string
{
    if (sv.empty())
        return "(empty)";
    if (is_printable(sv))
        return std::string(sv);
    // Show both hex and a best-effort printable representation.
    const auto hex = to_hex(sv);
    return std::format("(binary {} bytes) {}", sv.size(), hex);
}

// ---------------------------------------------------------------------------
// Category classification
// ---------------------------------------------------------------------------
enum class key_category {
    own_identity_public,
    own_identity_private,
    own_signed_prekey_id,
    own_signed_prekey_record,
    own_signed_prekey_public,
    own_signed_prekey_sig,
    own_prekeys_index,
    own_device_id,
    own_registration_id,
    session,
    identity,
    prekey,
    signed_prekey,
    bundle,
    legacy_bundle,
    devicelist,
    legacy_devicelist,
    axolotl_devicelist,
    device_mode,
    atm_trust,
    sender_key,
    unknown,
};

struct classified_key {
    key_category category;
    std::string  label;  // human-readable category name
    std::string  suffix; // the part after the prefix
};

static auto classify(std::string_view key) -> classified_key
{
    // Check fixed own-key constants first (exact matches).
    if (key == kIdentityPublic)
        return {key_category::own_identity_public, "OWN_IDENTITY_PUBLIC", {}};
    if (key == kIdentityPrivate)
        return {key_category::own_identity_private, "OWN_IDENTITY_PRIVATE", {}};
    if (key == kSignedPreKeyId)
        return {key_category::own_signed_prekey_id, "OWN_SIGNED_PREKEY_ID", {}};
    if (key == kSignedPreKeyRecord)
        return {key_category::own_signed_prekey_record, "OWN_SIGNED_PREKEY_RECORD", {}};
    if (key == kSignedPreKeyPublic)
        return {key_category::own_signed_prekey_public, "OWN_SIGNED_PREKEY_PUBLIC", {}};
    if (key == kSignedPreKeySig)
        return {key_category::own_signed_prekey_sig, "OWN_SIGNED_PREKEY_SIG", {}};
    if (key == kPrekeys)
        return {key_category::own_prekeys_index, "OWN_PREKEYS_INDEX", {}};
    if (key == kDeviceIdKey)
        return {key_category::own_device_id, "OWN_DEVICE_ID", {}};
    if (key == kRegistrationId)
        return {key_category::own_registration_id, "OWN_REGISTRATION_ID", {}};

    // Prefix-based classification.
    auto prefix_match = [&](std::string_view prefix) -> bool {
        return key.starts_with(prefix);
    };

    if (prefix_match(kPfxSession))
        return {key_category::session, "SESSION", std::string(key.substr(kPfxSession.size()))};
    // identity:public / identity:private were already matched above.
    // identity:<jid>:<devid> — starts_with("identity:") but has a second colon.
    if (prefix_match(kPfxIdentity))
        return {key_category::identity, "IDENTITY", std::string(key.substr(kPfxIdentity.size()))};
    if (prefix_match(kPfxSignedPrekey))
        return {key_category::signed_prekey, "SIGNED_PREKEY_RECORD", std::string(key.substr(kPfxSignedPrekey.size()))};
    if (prefix_match(kPfxPrekey))
        return {key_category::prekey, "PREKEY_RECORD", std::string(key.substr(kPfxPrekey.size()))};
    if (prefix_match(kPfxLegacyBundle))
        return {key_category::legacy_bundle, "LEGACY_BUNDLE", std::string(key.substr(kPfxLegacyBundle.size()))};
    if (prefix_match(kPfxBundle))
        return {key_category::bundle, "BUNDLE", std::string(key.substr(kPfxBundle.size()))};
    if (prefix_match(kPfxLegacyDL))
        return {key_category::legacy_devicelist, "LEGACY_DEVICELIST", std::string(key.substr(kPfxLegacyDL.size()))};
    if (prefix_match(kPfxAxolotlDL))
        return {key_category::axolotl_devicelist, "AXOLOTL_DEVICELIST", std::string(key.substr(kPfxAxolotlDL.size()))};
    if (prefix_match(kPfxDevicelist))
        return {key_category::devicelist, "DEVICELIST", std::string(key.substr(kPfxDevicelist.size()))};
    if (prefix_match(kPfxDeviceMode))
        return {key_category::device_mode, "DEVICE_MODE", std::string(key.substr(kPfxDeviceMode.size()))};
    if (prefix_match(kPfxAtmTrust))
        return {key_category::atm_trust, "ATM_TRUST", std::string(key.substr(kPfxAtmTrust.size()))};
    if (prefix_match(kPfxSenderKey))
        return {key_category::sender_key, "SENDER_KEY", std::string(key.substr(kPfxSenderKey.size()))};

    return {key_category::unknown, "UNKNOWN", std::string(key)};
}

// ---------------------------------------------------------------------------
// RAII wrappers for MDB handles (no lmdb++, no weechat headers)
// ---------------------------------------------------------------------------

struct mdb_env_guard {
    MDB_env *env = nullptr;
    mdb_env_guard() { mdb_env_create(&env); }
    ~mdb_env_guard() { if (env) mdb_env_close(env); }
    mdb_env_guard(const mdb_env_guard &) = delete;
    mdb_env_guard &operator=(const mdb_env_guard &) = delete;
};

struct mdb_txn_guard {
    MDB_txn *txn = nullptr;
    bool committed = false;
    explicit mdb_txn_guard(MDB_env *env, unsigned int flags = MDB_RDONLY)
    {
        if (mdb_txn_begin(env, nullptr, flags, &txn) != 0)
            txn = nullptr;
    }
    ~mdb_txn_guard()
    {
        if (txn && !committed)
            mdb_txn_abort(txn);
    }
    void abort() { if (txn) { mdb_txn_abort(txn); txn = nullptr; } }
    mdb_txn_guard(const mdb_txn_guard &) = delete;
    mdb_txn_guard &operator=(const mdb_txn_guard &) = delete;
};

struct mdb_cursor_guard {
    MDB_cursor *cursor = nullptr;
    explicit mdb_cursor_guard(MDB_txn *txn, MDB_dbi dbi)
    {
        mdb_cursor_open(txn, dbi, &cursor);
    }
    ~mdb_cursor_guard() { if (cursor) mdb_cursor_close(cursor); }
    mdb_cursor_guard(const mdb_cursor_guard &) = delete;
    mdb_cursor_guard &operator=(const mdb_cursor_guard &) = delete;
};

// ---------------------------------------------------------------------------
// Main dump routine
// ---------------------------------------------------------------------------

static int dump_db(const std::string &db_path, const std::string &prefix_filter)
{
    mdb_env_guard env_guard;
    if (!env_guard.env) {
        std::cerr << "error: mdb_env_create failed\n";
        return 1;
    }

    mdb_env_set_maxdbs(env_guard.env, 4);
    mdb_env_set_mapsize(env_guard.env, 32ULL * 1024 * 1024);

    int rc = mdb_env_open(env_guard.env, db_path.c_str(), MDB_NOSUBDIR | MDB_RDONLY, 0664);
    if (rc != 0) {
        std::cerr << std::format("error: cannot open '{}': {}\n", db_path, mdb_strerror(rc));
        return 1;
    }

    mdb_txn_guard txn_guard(env_guard.env, MDB_RDONLY);
    if (!txn_guard.txn) {
        std::cerr << "error: mdb_txn_begin failed\n";
        return 1;
    }

    MDB_dbi dbi = 0;
    rc = mdb_dbi_open(txn_guard.txn, "omemo", 0, &dbi);
    if (rc != 0) {
        std::cerr << std::format("error: cannot open 'omemo' DBI: {}\n", mdb_strerror(rc));
        return 1;
    }

    mdb_cursor_guard cursor_guard(txn_guard.txn, dbi);
    if (!cursor_guard.cursor) {
        std::cerr << "error: mdb_cursor_open failed\n";
        return 1;
    }

    MDB_val mdb_key {};
    MDB_val mdb_val {};
    rc = mdb_cursor_get(cursor_guard.cursor, &mdb_key, &mdb_val, MDB_FIRST);

    std::size_t total = 0;
    std::size_t shown = 0;

    while (rc == 0) {
        const std::string_view key_sv {static_cast<const char *>(mdb_key.mv_data), mdb_key.mv_size};
        const std::string_view val_sv {static_cast<const char *>(mdb_val.mv_data), mdb_val.mv_size};

        ++total;

        // Apply optional prefix filter.
        if (!prefix_filter.empty() && !key_sv.starts_with(prefix_filter)) {
            rc = mdb_cursor_get(cursor_guard.cursor, &mdb_key, &mdb_val, MDB_NEXT);
            continue;
        }

        ++shown;
        const auto ck = classify(key_sv);
        const auto val_str = format_value(val_sv);

        std::cout << std::format("[{}] key='{}' suffix='{}'\n    value={}\n",
                                 ck.label,
                                 std::string(key_sv),
                                 ck.suffix,
                                 val_str);

        rc = mdb_cursor_get(cursor_guard.cursor, &mdb_key, &mdb_val, MDB_NEXT);
    }

    if (rc != MDB_NOTFOUND) {
        std::cerr << std::format("warning: cursor iteration ended with: {}\n", mdb_strerror(rc));
    }

    std::cout << std::format("\n--- {} / {} entries shown (filter='{}')\n",
                             shown, total, prefix_filter.empty() ? "(none)" : prefix_filter);
    return 0;
}

// ---------------------------------------------------------------------------
// CLI parsing + entry point
// ---------------------------------------------------------------------------

static void print_usage(const char *argv0)
{
    std::cerr << std::format(
        "Usage: {} [--db <path>] [--prefix <key-prefix>]\n"
        "\n"
        "  --db <path>       Path to omemo_<account>.db (LMDB MDB_NOSUBDIR file)\n"
        "                    Default: $HOME/.local/share/weechat/xmpp/omemo_$XMPP_ACCOUNT.db\n"
        "  --prefix <str>    Only print keys that start with <str>\n"
        "\n"
        "Key prefixes in use:\n"
        "  session:          Signal session records (binary)\n"
        "  identity:         Identity key bytes (binary); identity:public/private for own keys\n"
        "  prekey:           Pre-key records (binary)\n"
        "  signed-prekey:    Signed pre-key records (binary)\n"
        "  bundle:           OMEMO:2 cached bundle (binary serialised)\n"
        "  legacy_bundle:    Axolotl cached bundle (binary serialised)\n"
        "  devicelist:       OMEMO:2 device ID list (text)\n"
        "  axolotl_devicelist: Axolotl device ID list after DB migration (text)\n"
        "  legacy_devicelist:  Axolotl device ID list before DB migration (text)\n"
        "  device_mode:      Per-device protocol mode (omemo2/legacy/unknown)\n"
        "  atm_trust:        XEP-0450 ATM trust decision (trusted/distrusted/undecided)\n"
        "  senderkey:        Group sender key (binary)\n",
        argv0);
}

int main(int argc, char *argv[])
{
    std::string db_path;
    std::string prefix_filter;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg {argv[i]};
        if ((arg == "--db" || arg == "-d") && i + 1 < argc) {
            db_path = argv[++i];
        } else if ((arg == "--prefix" || arg == "-p") && i + 1 < argc) {
            prefix_filter = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << std::format("unknown argument: {}\n", arg);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (db_path.empty()) {
        const char *home = std::getenv("HOME");
        const char *account = std::getenv("XMPP_ACCOUNT");
        if (!home || !account) {
            std::cerr << "error: --db not provided and HOME/XMPP_ACCOUNT env vars not set\n";
            print_usage(argv[0]);
            return 1;
        }
        db_path = std::format("{}/.local/share/weechat/xmpp/omemo_{}.db", home, account);
    }

    std::cout << std::format("Opening: {}\n\n", db_path);
    return dump_db(db_path, prefix_filter);
}
