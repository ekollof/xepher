// dump_mam_db.cpp — Standalone C++23 inspector for the MAM LMDB database.
//
// Usage:
//   dump_mam_db [--db <path>] [--table <name>] [--filter <str>] [--limit <N>]
//
// Default db path:
//   $HOME/.local/share/weechat/xmpp/mam_$XMPP_ACCOUNT.db
//   Set XMPP_ACCOUNT in the environment or pass --db explicitly.
//
// Tables in mam_<account>.db:
//   messages        key: <channel_jid>:<timestamp_20d>:<msg_id>
//                   val: <from>|<timestamp>|<body>
//   timestamps      key: <channel_jid>
//                   val: time_t (8 bytes, little-endian)
//   retractions     key: <channel_jid>:<msg_id>
//                   val: time_t (8 bytes) — when retracted
//   cursors         key: "global" | "pm_open:<jid>" | <channel_jid>
//                   val: RSM <last> cursor string or empty marker
//   omemo_plaintext key: <channel_jid>:<msg_id>
//                   val: decrypted plaintext body
//   capabilities    key: <node>#<ver>
//                   val: newline-separated feature strings
//
// Build via tools/Makefile (not the root Makefile).

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
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
// RAII wrappers — no weechat/lmdbxx headers needed
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
    explicit mdb_txn_guard(MDB_env *env, unsigned int flags = MDB_RDONLY)
    {
        if (mdb_txn_begin(env, nullptr, flags, &txn) != 0)
            txn = nullptr;
    }
    ~mdb_txn_guard() { if (txn) mdb_txn_abort(txn); }
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
// Helpers
// ---------------------------------------------------------------------------

static auto is_printable(std::string_view sv) -> bool
{
    for (const unsigned char c : sv)
        if (c < 0x20 || c > 0x7e)
            return false;
    return true;
}

static auto to_hex(std::string_view sv) -> std::string
{
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (const unsigned char c : sv)
        oss << std::setw(2) << static_cast<unsigned>(c);
    return oss.str();
}

static auto format_value(std::string_view sv) -> std::string
{
    if (sv.empty()) return "(empty)";
    if (is_printable(sv)) return std::string(sv);
    return std::format("(binary {} bytes) {}", sv.size(), to_hex(sv));
}

// Format a raw time_t value (stored as native binary in timestamps/retractions).
static auto format_time_t(std::string_view sv) -> std::string
{
    if (sv.size() != sizeof(time_t))
        return std::format("(bad time_t size {})", sv.size());
    time_t t = 0;
    std::memcpy(&t, sv.data(), sizeof(t));
    if (t == static_cast<time_t>(-1))
        return "-1 (closed/suppressed)";
    char buf[32];
    struct tm *lt = localtime(&t);
    if (!lt) return std::format("{} (invalid)", t);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", lt);
    return std::format("{} ({})", t, buf);
}

// ---------------------------------------------------------------------------
// Per-table dump
// ---------------------------------------------------------------------------

enum class table_id { messages, timestamps, retractions, cursors, omemo_plaintext, capabilities };

static const char *table_name(table_id t)
{
    switch (t) {
        case table_id::messages:        return "messages";
        case table_id::timestamps:      return "timestamps";
        case table_id::retractions:     return "retractions";
        case table_id::cursors:         return "cursors";
        case table_id::omemo_plaintext: return "omemo_plaintext";
        case table_id::capabilities:    return "capabilities";
    }
    return "unknown";
}

static std::size_t dump_table(MDB_env *env, table_id tid,
                               const std::string &filter, std::size_t limit)
{
    mdb_txn_guard txn(env, MDB_RDONLY);
    if (!txn.txn) {
        std::cerr << "error: mdb_txn_begin failed\n";
        return 0;
    }

    MDB_dbi dbi = 0;
    int rc = mdb_dbi_open(txn.txn, table_name(tid), 0, &dbi);
    if (rc != 0) {
        std::cerr << std::format("  (table '{}' not found or empty: {})\n",
                                 table_name(tid), mdb_strerror(rc));
        return 0;
    }

    mdb_cursor_guard cur(txn.txn, dbi);
    if (!cur.cursor) {
        std::cerr << "  error: mdb_cursor_open failed\n";
        return 0;
    }

    MDB_val k{}, v{};
    rc = mdb_cursor_get(cur.cursor, &k, &v, MDB_FIRST);

    std::size_t shown = 0, skipped = 0;

    while (rc == 0) {
        const std::string_view ksv{static_cast<const char *>(k.mv_data), k.mv_size};
        const std::string_view vsv{static_cast<const char *>(v.mv_data), v.mv_size};

        if (!filter.empty() && !ksv.starts_with(filter)) {
            rc = mdb_cursor_get(cur.cursor, &k, &v, MDB_NEXT);
            ++skipped;
            continue;
        }

        if (limit > 0 && shown >= limit) {
            ++skipped;
            rc = mdb_cursor_get(cur.cursor, &k, &v, MDB_NEXT);
            continue;
        }

        // Table-specific pretty printing
        switch (tid) {
            case table_id::messages: {
                // key: channel_jid:timestamp_20d:msg_id
                // val: from|timestamp|body
                std::string val_str(vsv);
                auto p1 = val_str.find('|');
                auto p2 = p1 != std::string::npos ? val_str.find('|', p1 + 1) : std::string::npos;
                if (p1 != std::string::npos && p2 != std::string::npos) {
                    std::string from = val_str.substr(0, p1);
                    std::string ts_s = val_str.substr(p1 + 1, p2 - p1 - 1);
                    std::string body = val_str.substr(p2 + 1);
                    time_t ts = 0;
                    std::from_chars(ts_s.data(), ts_s.data() + ts_s.size(), ts);
                    char tbuf[32];
                    struct tm *lt = localtime(&ts);
                    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", lt ? lt : nullptr);
                    std::cout << std::format("  key: {}\n"
                                             "  from: {}  time: {} ({})\n"
                                             "  body: {}\n\n",
                                             std::string(ksv), from, ts, tbuf,
                                             body.size() > 120
                                                 ? body.substr(0, 120) + "…"
                                                 : body);
                } else {
                    std::cout << std::format("  key: {}\n  val(raw): {}\n\n",
                                             std::string(ksv), format_value(vsv));
                }
                break;
            }
            case table_id::timestamps:
            case table_id::retractions:
                std::cout << std::format("  key: {}\n  time: {}\n\n",
                                         std::string(ksv), format_time_t(vsv));
                break;
            case table_id::cursors:
            case table_id::omemo_plaintext:
                std::cout << std::format("  key: {}\n  val: {}\n\n",
                                         std::string(ksv), format_value(vsv));
                break;
            case table_id::capabilities: {
                // val: newline-separated feature strings
                std::string feats(vsv);
                std::cout << std::format("  key: {}\n", std::string(ksv));
                std::size_t pos = 0;
                while (pos < feats.size()) {
                    auto nl = feats.find('\n', pos);
                    auto feat = feats.substr(pos, nl == std::string::npos ? nl : nl - pos);
                    if (!feat.empty())
                        std::cout << std::format("    feat: {}\n", feat);
                    if (nl == std::string::npos) break;
                    pos = nl + 1;
                }
                std::cout << "\n";
                break;
            }
        }

        ++shown;
        rc = mdb_cursor_get(cur.cursor, &k, &v, MDB_NEXT);
    }

    if (rc != MDB_NOTFOUND)
        std::cerr << std::format("  warning: cursor ended with: {}\n", mdb_strerror(rc));

    std::cout << std::format("  --- {} entries shown, {} skipped ---\n", shown, skipped);
    return shown;
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

static const std::array<table_id, 6> all_tables = {
    table_id::messages,
    table_id::timestamps,
    table_id::retractions,
    table_id::cursors,
    table_id::omemo_plaintext,
    table_id::capabilities,
};

static void print_usage(const char *argv0)
{
    std::cerr << std::format(
        "Usage: {} [--db <path>] [--table <name>] [--filter <prefix>] [--limit <N>]\n"
        "\n"
        "  --db <path>       Path to mam_<account>.db  (MDB_NOSUBDIR file)\n"
        "                    Default: $HOME/.local/share/weechat/xmpp/mam_$XMPP_ACCOUNT.db\n"
        "  --table <name>    Dump only this table (default: all tables)\n"
        "  --filter <prefix> Only show keys starting with <prefix>\n"
        "  --limit <N>       Max entries per table (0 = unlimited, default 200)\n"
        "\n"
        "Tables:\n"
        "  messages        <channel>:<ts_20d>:<msg_id>  =>  <from>|<ts>|<body>\n"
        "  timestamps      <channel>                    =>  time_t (last MAM fetch)\n"
        "  retractions     <channel>:<msg_id>           =>  time_t (when retracted)\n"
        "  cursors         global | pm_open:<jid> | ... =>  RSM cursor string\n"
        "  omemo_plaintext <channel>:<msg_id>           =>  decrypted body\n"
        "  capabilities    <node>#<ver>                 =>  newline-separated features\n",
        argv0);
}

int main(int argc, char *argv[])
{
    std::string db_path;
    std::string table_filter;
    std::string key_filter;
    std::size_t limit = 200;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if ((arg == "--db" || arg == "-d") && i + 1 < argc) {
            db_path = argv[++i];
        } else if ((arg == "--table" || arg == "-t") && i + 1 < argc) {
            table_filter = argv[++i];
        } else if ((arg == "--filter" || arg == "-f") && i + 1 < argc) {
            key_filter = argv[++i];
        } else if ((arg == "--limit" || arg == "-n") && i + 1 < argc) {
            limit = static_cast<std::size_t>(std::stoul(argv[++i]));
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
        const char *home    = std::getenv("HOME");
        const char *account = std::getenv("XMPP_ACCOUNT");
        if (!home || !account) {
            std::cerr << "error: --db not provided and HOME/XMPP_ACCOUNT not set\n";
            print_usage(argv[0]);
            return 1;
        }
        db_path = std::format("{}/.local/share/weechat/xmpp/mam_{}.db", home, account);
    }

    std::cout << std::format("Opening: {}\n\n", db_path);

    mdb_env_guard env;
    if (!env.env) {
        std::cerr << "error: mdb_env_create failed\n";
        return 1;
    }
    mdb_env_set_maxdbs(env.env, 10);
    mdb_env_set_mapsize(env.env, 1024ULL * 1024 * 1024);  // 1 GB map view

    int rc = mdb_env_open(env.env, db_path.c_str(), MDB_NOSUBDIR | MDB_RDONLY, 0600);
    if (rc != 0) {
        std::cerr << std::format("error: cannot open '{}': {}\n", db_path, mdb_strerror(rc));
        return 1;
    }

    for (auto tid : all_tables) {
        const char *tname = table_name(tid);
        if (!table_filter.empty() && table_filter != tname)
            continue;
        std::cout << std::format("=== table: {} ===\n", tname);
        dump_table(env.env, tid, key_filter, limit);
        std::cout << "\n";
    }

    return 0;
}
