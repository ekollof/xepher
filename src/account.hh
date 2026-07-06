// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <atomic>
#include <ctime>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <functional>
#include <optional>
#include <expected>
#include <span>
#include <utility>
#include <lmdb++.h>

#include "fmt/core.h"
#include "util.hh"
#include "strophe.h"
#include "pgp.hh"
#include "omemo.hh"
#include "config.hh"
#include "channel.hh"
#include "xmpp/message_bob.hh"
#include "connection.hh"
#include "connection/strophe_stream_features.hh"
#include "weechat/connection_port.hh"
#include "user.hh"
#include "ui/picker.hh"
#include "xmpp/embed.hh"

namespace weechat
{
    class channel;
    class user;

    // Global flag to signal plugin is unloading (prevents crashes in timer callbacks)
    extern bool g_plugin_unloading;

    void log_emit(void *const userdata, const xmpp_log_level_t level,
                  const char *const area, const char *const msg);

    class account : public config_account
    {
    public:
        struct device
        {
            std::uint32_t id;
            std::string name;
            std::string label;
        };

        struct mam_query
        {
            std::string id;
            std::string with;
            std::optional<time_t> start;
            std::optional<time_t> end;
        };

        struct roster_item
        {
            std::string jid;
            std::string name;
            std::string subscription;
            std::vector<std::string> groups;
        };

        struct bookmark_item
        {
            std::string jid;
            std::string name;
            std::string nick;
            bool autojoin;
            // XEP-0492: per-chat notification preference ("always", "on-mention", "never").
            // Empty string means unset (default: "always" for direct/private, "on-mention" for public MUC).
            std::string notify_setting;
        };

        struct upload_request
        {
            std::string id;
            std::string filepath;         // Full path for opening the file
            std::string filename;         // Sanitized filename for the server
            std::string channel_id;
            std::string content_type;     // MIME type
            size_t file_size = 0;         // Size in bytes
            // XEP-0300 hash agility: (algo, base64) pairs.
            std::vector<std::pair<std::string, std::string>> hashes;
            bool is_muc = false;          // for correct message type (groupchat vs chat) in fallback sends
        };

    public:
        bool disconnected = false;

        std::unordered_map<std::uint32_t, device> devices;
        std::unordered_map<std::string, mam_query> mam_queries;
        std::unordered_map<std::string, roster_item> roster;
        std::unordered_map<std::string, bookmark_item> bookmarks;
        std::unordered_map<std::string, upload_request> upload_requests;

        // XEP-0363 async upload: pending completions waiting for main-thread callback.
        // Keyed by the read-end fd of a self-pipe; value is a shared struct filled by the
        // upload thread once the HTTP PUT completes.
        struct upload_completion
        {
            bool success = false;
            std::string get_url;        // Download URL to share in the message
            std::string channel_id;
            bool is_muc = false;          // remember for fallback rich send when local channel gone
            std::string filename;
            std::string local_path;       // original file path on disk (for /icat preview)
            std::string content_type;
            size_t file_size = 0;
            // XEP-0300 hash agility: (algo, base64) pairs.
            std::vector<std::pair<std::string, std::string>> hashes;
            size_t image_width  = 0;    // Image width  in pixels (0 = unknown/non-image)
            size_t image_height = 0;    // Image height in pixels (0 = unknown/non-image)
            std::string file_date;      // XEP-0446 <date> (UTC ISO-8601, e.g. 2026-06-03T09:28:00Z)
            long http_code = 0;
            std::string curl_error;
            int pipe_write_fd = -1;     // write end (closed by thread after writing)
            struct t_hook *hook = nullptr;           // weechat_hook_fd (filled after thread starts)
            std::thread worker;         // owns the background thread
            std::atomic<bool> worker_done{false};  // set by thread when upload completes
            // Non-empty when this upload belongs to a pending feed post (embed tag).
            // Set to the upload slot IQ id that originated this upload.
            std::string feed_post_upload_id;
            // XEP-0448: Encrypted File Sharing fields (set when channel has OMEMO active)
            bool encrypted = false;            // true when upload was AES-256-GCM encrypted
            std::string esfs_key_b64;          // Base64(32-byte AES-256 key)
            std::string esfs_iv_b64;           // Base64(12-byte GCM IV)
            std::string esfs_cipher_hash_b64;  // Base64(SHA-256 of ciphertext)
            std::string esfs_aesgcm_fragment;  // hex(iv_bytes) + hex(key_bytes) for aesgcm:// URL fragment
            size_t original_file_size = 0;     // plaintext file size for <file> metadata
        };
        // fd (read end) -> completion context
        std::unordered_map<int, std::shared_ptr<upload_completion>> pending_uploads;
        
        std::string upload_service;  // JID of upload service (discovered via disco)
        size_t upload_max_size = 0;  // Max file size in bytes

    private:
        bool is_connected = false;

        int current_retry = 0;
        int reconnect_delay = 0;
        int reconnect_start = 0;

        xmpp_mem_t memory = { nullptr };
        xmpp_log_t logger = { nullptr };

        std::string buffer_as_string;

        friend void log_emit(void *const userdata, const xmpp_log_level_t level,
                             const char *const area, const char *const msg);

        void disconnect_impl(int reconnect, bool immediate_reconnect);

    public:
        // Last top-level XEP-0198 / XEP-0352 stanza sent (for stream-error attribution).
        last_top_level_ext last_top_level_ext_sent = last_top_level_ext::none;

        // Client State Indication (XEP-0352)
        bool csi_available = true;    // Server accepts top-level <active/>/<inactive/>
        bool csi_active = true;
        time_t last_activity = 0;
        struct t_hook *idle_timer_hook = nullptr;
        struct t_hook *csi_activity_hooks[3] = {nullptr, nullptr, nullptr};  // Store activity signal hooks
        // XEP-0319: Last User Interaction in Presence — track whether idle <presence> was sent
        bool xep0319_idle_sent = false;

        // Stream Management (XEP-0198)
        bool sm_enabled = false;
        bool sm_available = true;       // Can we try to enable SM?
        bool sm_handlers_registered = false;  // Track if handlers are already added
        bool sm_awaiting_negotiation = false; // enable/resume sent; defer app stanzas
        bool sm_resume_attempted = false;     // current connect tried <resume/>
        bool sm_post_connect_done = false;    // run_post_connect_setup completed
        std::string sm_id;              // session ID for resumption
        std::string sm_reconnect_host;  // preferred host from <enabled location='…'/>
        std::uint16_t sm_reconnect_port = 0;
        uint32_t sm_h_inbound = 0;      // stanzas we've received and handled
        uint32_t sm_h_outbound = 0;     // stanzas we've sent
        uint32_t sm_last_ack = 0;       // last h value acknowledged by server
        time_t sm_last_ack_log = 0;     // throttle ack debug logging
        struct t_hook *sm_ack_timer_hook = nullptr;
        // Unacknowledged outbound stanzas: (outbound_seq, stanza_copy)
        // seq is 1-based (matches sm_h_outbound at time of send)
        std::deque<std::pair<uint32_t, std::shared_ptr<xmpp_stanza_t>>> sm_outqueue;
        // Stanzas to re-send after resume failure → fresh <enable/> (new h values).
        std::deque<std::shared_ptr<xmpp_stanza_t>> sm_pending_replay;

        std::string name;
        weechat::xmpp::pgp pgp;
        weechat::xmpp::omemo omemo;
        libstrophe::context context;
        weechat::connection connection;
        struct t_gui_buffer *buffer = nullptr;
        std::unordered_map<std::string, weechat::channel> channels;
        std::unordered_map<std::string, weechat::user,
                            transparent_string_hash, std::equal_to<>> users;

        std::unordered_map<std::string, struct t_config_option *> options;
        
        std::unordered_set<std::string> user_disco_queries;
        std::unordered_set<std::string> user_disco_items_queries;  // disco#items queries initiated by /disco items
        std::unordered_map<std::string, time_t> user_ping_queries;  // ping_id -> start_time
        std::unordered_map<std::string, std::string> caps_disco_queries;  // disco_id -> verification_hash
        std::unordered_map<std::string, std::string> upload_disco_queries;  // disco_id -> service_jid

        // Server disco#info issued on connect to gate optional IQ probes (MAM, MDS, …).
        std::optional<std::string> pending_server_disco_id;
        bool optional_server_probes_done = false;

        // XEP-0050: Ad-Hoc Commands query tracking
        struct adhoc_query_info {
            std::string target_jid;   // JID we queried
            struct t_gui_buffer *buffer; // buffer to print results into
            bool is_list;             // true = listing commands, false = executing one
            std::string node;         // command node being executed (for execute queries)
            std::string session_id;   // session id (for multi-step forms)
            weechat::ui::picker<std::string> *picker = nullptr;  // non-owning; picker owns itself
        };
        std::unordered_map<std::string, adhoc_query_info> adhoc_queries;  // iq_id -> info

        // XEP-0433: Extended Channel Search query tracking
        struct channel_search_query_info {
            std::string service_jid;        // search service JID
            std::string keywords;           // search keywords
            struct t_gui_buffer *buffer;    // buffer to print results into
            bool form_requested;            // true = waiting for form, false = waiting for results
            weechat::ui::picker<std::string> *picker = nullptr;  // non-owning; picker owns itself
        };
        std::unordered_map<std::string, channel_search_query_info> channel_search_queries;  // iq_id -> info

        // Follow-up disco#info queries for /list result enrichment.
        struct channel_search_disco_query_info {
            struct t_gui_buffer *buffer;    // buffer to print enriched info into
            std::string room_jid;           // room JID queried via disco#info
        };
        std::unordered_map<std::string, channel_search_disco_query_info> channel_search_disco_queries;  // iq_id -> info

        // MUC channel mode discovery: disco#info queries sent on join and on
        // status 104 (room configuration changed). Keyed by IQ id. The result
        // handler in iq_handler.inl extracts the muc_* features and the
        // muc#roominfo x-data form into channel::apply_muc_info().
        std::unordered_map<std::string, std::string> muc_modes_queries;  // iq_id -> room_jid
        // Idempotency: room JIDs for which a modes disco#info is already in
        // flight or has been answered. Cleared on disconnect.
        std::unordered_set<std::string> muc_modes_fetched;

        // XEP-0045 §10.2/§10.5/§10.7: muc#owner and muc#admin IQ tracking.
        // The "kind" field tells the result handler how to interpret the
        // response (parse a form on "config_get"/"config_set"; print outcome
        // on "aff_set"/"destroy"). "room_jid" is the destination (bare JID).
        // "buffer" is where to print outcome messages.
        enum class muc_owner_kind {
            config_get, config_set, destroy,
            aff_set, aff_list,
            register_get, register_set
        };
        struct muc_owner_query_info {
            std::string room_jid;
            struct t_gui_buffer *buffer;
            muc_owner_kind kind;
            std::string list_affiliation;   // aff_list filter (e.g. "member")
            std::string register_nick;        // pending nick for register_set after GET
        };
        std::unordered_map<std::string, muc_owner_query_info> muc_owner_queries;  // iq_id -> info

        // XEP-0045 §10.1: rooms created via /create --reserved. When a
        // status 201 self-presence arrives for one of these, the presence
        // handler skips the auto-empty config submit (instant-room unlock)
        // and leaves the room locked so the user can configure it via
        // /setmodes / /affiliation / /destroy. Cleared on disconnect.
        std::unordered_set<std::string> muc_reserved_pending;

        // XEP-0045 §7.8.2: pending mediated MUC invitations awaiting /decline.
        struct pending_mediated_invite {
            std::string room_jid;
            std::string inviter_bare;
            std::optional<std::string> password;
        };
        std::deque<pending_mediated_invite> pending_mediated_invites;

        // XEP-0054 / XEP-0292 vCard query tracking
        struct whois_query_info {
            struct t_gui_buffer *buffer;  // buffer to print vCard results into
            std::string target_jid;       // JID we queried
        };
        std::unordered_map<std::string, whois_query_info> whois_queries;  // iq_id -> info

        // XEP-0054 /setvcard read-merge: pending field change waiting for self-vCard fetch
        struct setvcard_query_info {
            struct t_gui_buffer *buffer;  // buffer for feedback
            std::string field;            // e.g. "fn", "email", …
            std::string value;            // new value to set
        };
        std::unordered_map<std::string, setvcard_query_info> setvcard_queries;  // iq_id -> info

        // XEP-0060: pending pubsub item-fetch IQs triggered by <retract> events or /feed.
        // Maps IQ id → fetch context.
        struct pubsub_fetch_info {
            std::string service;       // pubsub service JID
            std::string node;          // node name
            std::string before_cursor; // RSM cursor at send time (informational only; not used in result handler)
            int         max_items = 0; // max_items requested (0 = server default)
        };
        std::unordered_map<std::string, pubsub_fetch_info> pubsub_fetch_ids;

        // XEP-0442: Pubsub MAM — set of pubsub service JIDs known to support urn:xmpp:mam:2.
        // Populated by the disco#info handler when a pubsub service advertises the MAM feature.
        std::unordered_set<std::string> pubsub_mam_services;

        // XEP-0442: pending MAM queries against a pubsub node.
        // Maps IQ id → pubsub_fetch_info (same fields; service+node+max_items).
        std::unordered_map<std::string, pubsub_fetch_info> pubsub_mam_queries;

        // XEP-0442: disco#info queries sent to discover MAM support on pubsub services.
        // Maps IQ id → service_jid.  When the result arrives we record in pubsub_mam_services
        // and (if a feed restore was deferred) re-trigger restore_feed for that service.
        std::unordered_map<std::string, std::string> pubsub_mam_disco_queries;

        // XEP-0442: feed_keys deferred pending MAM-support disco for their pubsub service.
        // Maps service_jid → list of feed_keys ("service/node") waiting for the result.
        std::unordered_map<std::string, std::vector<std::string>> pubsub_mam_deferred_feeds;

        // XEP-0060: pending disco#items queries for PubSub node enumeration (/feed <service> --all).
        // Maps IQ id → service_jid
        std::unordered_map<std::string, std::string> pubsub_disco_queries;

        // XEP-0060: pending subscriptions queries (/feed <service>).
        // Maps IQ id → service_jid
        std::unordered_map<std::string, std::string> pubsub_subscriptions_queries;

        // XEP-0441: MAM Preferences — pending get/set IQ tracking.
        // Maps IQ id → buffer to print result into.
        std::unordered_map<std::string, struct t_gui_buffer *> mam_prefs_queries;

        // XEP-0280: pending carbons enable IQ id (connect-time negotiation).
        std::optional<std::string> pending_carbons_enable_iq_;

        // XEP-0490: pending MDS fetch IQ id (connect-time sync of displayed state).
        std::optional<std::string> pending_mds_fetch_iq_;

        // XEP-0231: BoB image fetches for XHTML-IM cid:…@bob.xmpp.org stickers.
        struct bob_icat_context {
            std::string cid;
            struct t_gui_buffer *buffer = nullptr;
            std::string channel_jid;
            std::string stable_id;
            std::string mime;
            bool mam_replay = false;
        };
        std::unordered_map<std::string, bob_icat_context> bob_fetch_queries;  // iq_id → ctx
        std::unordered_set<std::string> bob_inflight_cids;
        std::unordered_map<std::string, std::vector<bob_icat_context>> bob_deferred_icat;
        // Payloads we host for inbound BoB IQ-get (outbound XEP-0231 sends).
        std::unordered_map<std::string, ::xmpp::BobHostedPayload> bob_hosted;

        // XEP-0060: pending publish IQs (/feed post, /feed reply, /feed retract).
        // Maps IQ id → context used to report errors to the user.
        struct pubsub_publish_context {
            std::string service;
            std::string node;
            std::string item_id;
            struct t_gui_buffer *buffer = nullptr;  // buffer for error feedback
            bool is_retract = false;                // true → full node re-fetch on result
        };
        std::unordered_map<std::string, pubsub_publish_context> pubsub_publish_ids;

        // Embed template: pending feed posts waiting for XEP-0363 uploads.
        // Maps upload request IQ id → pending post state.
        std::unordered_map<std::string, xepher::pending_feed_post> pending_feed_posts;

        // Save a draft of a pending post to $weechat_data_dir/xmpp/drafts/ and
        // return the path. Used when an embed upload fails so the user doesn't
        // lose their work.
        std::string save_feed_draft(const xepher::pending_feed_post &post);

        // Build the Atom <entry> and publish it via XEP-0060 for a completed
        // pending_feed_post (all embed uploads done, get_url filled in).
        void build_and_publish_post(const xepher::pending_feed_post &post);

        // XEP-0060: pending subscribe/unsubscribe IQs.
        // Maps IQ id → {feed_key, originating_buffer}
        struct pubsub_subscribe_context {
            std::string feed_key;
            struct t_gui_buffer *buffer = nullptr;  // buffer for feedback (falls back to account.buffer)
        };
        std::unordered_map<std::string, pubsub_subscribe_context> pubsub_subscribe_queries;
        std::unordered_map<std::string, pubsub_subscribe_context> pubsub_unsubscribe_queries;

        // XEP-0060: PubSub service components discovered on our server at connect time.
        // Populated by the disco#info handler when a server component reports
        // identity category='pubsub'. Used by /feed discover and /feed (no args).
        std::vector<std::string> known_pubsub_services;

        // In-memory mirror of LMDB feed_open:* keys for fast gate checks on
        // live pubsub pushes and MAM replay (synced on register/unregister).
        std::unordered_set<std::string> feed_open_keys_;
        // In-memory mirror of LMDB feed_seen:* keys (synced on connect / mark_seen).
        std::unordered_set<std::string> feed_seen_keys_;
        // Deferred MAM-page LMDB writes (queued in RAM, one txn per page on MAM fin).
        struct mam_cache_pending_write {
            MDB_dbi dbi = 0;
            std::string key;
            std::string value;
        };
        std::vector<mam_cache_pending_write> mam_cache_write_queue_;

        // Runtime cache: map "feed_key + item_id" to Atom entry IDs so /feed reply
        // can reference the target entry's real atom:id in thr:in-reply-to@ref.
        std::unordered_map<std::string, std::string> feed_atom_ids;
        // Runtime cache: map "feed_key + item_id" to replies XMPP URI so
        // /feed comments can open/fetch the comments node for a post.
        std::unordered_map<std::string, std::string> feed_replies_links;
        // Short alias system: each displayed feed item gets a short #N label so
        // users can write "/feed reply #3 text" instead of spelling out the full
        // service/node/item-id triple.
        // alias_fwd: "feed_key\nN" → item_id_raw
        // alias_rev: "feed_key\nitem_id_raw" → N  (for display and round-trip)
        // alias_ctr: "feed_key" → next integer to assign
        std::unordered_map<std::string, std::string> feed_alias_fwd;
        std::unordered_map<std::string, std::string> feed_alias_rev;
        std::unordered_map<std::string, int>         feed_alias_ctr;

        // XEP-0191: Blocking Command — pending unblock picker (non-owning; picker owns itself)
        weechat::ui::picker<std::string> *blocklist_picker = nullptr;

        // Capability cache (XEP-0115)
        std::unordered_map<std::string, std::vector<std::string>> caps_cache;  // verification_hash -> features
        // Last seen disco features per peer bare JID.
        std::unordered_map<std::string,
                            std::unordered_set<std::string,
                                               transparent_string_hash, std::equal_to<>>,
                            transparent_string_hash, std::equal_to<>> peer_features;
        // Case-folded bare JID → canonical channels map key (lazy-filled).
        mutable std::unordered_map<std::string, std::string,
                                    transparent_string_hash, std::equal_to<>>
            channel_key_cache_;

        // Cached bare JID — populated on connect, cleared on disconnect.
        // Avoids re-parsing xmpp_conn_get_bound_jid() on every stanza.
        std::string jid_bare_cache_;

        // Last nick value successfully published via XEP-0172 PEP in this process.
        // Used to suppress redundant nick publishes on reconnect (avoids triggering
        // MAM catchup on other active clients via the PEP push notification).
        // Intentionally in-memory only: resets on full WeeChat restart, which is
        // acceptable because the nick virtually never changes between sessions.
        std::string last_published_nick_;

        // MAM cache database
        lmdb::env mam_db_env = nullptr;
        struct mam_dbi {
            lmdb::dbi messages = 0;
            lmdb::dbi timestamps = 0;
            lmdb::dbi capabilities = 0;    // XEP-0115 capability cache
            lmdb::dbi retractions = 0;     // XEP-0424 retracted message IDs
            lmdb::dbi cursors = 0;         // RSM cursor persistence for MAM queries
            lmdb::dbi feed_seen = 0;       // feed item dedup keys (feed_key:item_id)
            lmdb::dbi feed_open = 0;       // open feed buffer keys (feed_key)
            lmdb::dbi pm_open = 0;         // open PM buffer keys (bare jid)
            lmdb::dbi omemo_plaintext = 0; // decrypted OMEMO body cache (keyed channel_jid:msg_id)
            lmdb::dbi esfs_downloads = 0;  // ESFS downloaded file paths (keyed channel_jid:stable_id → saved_path)
            lmdb::dbi image_previews = 0;  // local image paths for icat MAM replay (keyed channel_jid:stable_id)
            lmdb::dbi og_previews = 0;     // XEP-0511 OG preview cache (keyed by URL)
            lmdb::dbi muc_titles = 0;      // MUC display names (keyed room bare JID)
        } mam_dbi;
        std::string mam_db_path;

        int reloading_from_config = 0;

    public:
        account(config_file& config_file, const std::string name);
        ~account();

        static bool search(account* &out,
                           const std::string name, bool casesensitive = false);
        static int timer_cb(const void *pointer, void *data, int remaining_calls);
        static int idle_timer_cb(const void *pointer, void *data, int remaining_calls);
        static int sm_ack_timer_cb(const void *pointer, void *data, int remaining_calls);
        static int upload_fd_cb(const void *pointer, void *data, int fd);
        static int activity_cb(const void *pointer, void *data,
                              const char *signal, const char *type_data,
                              void *signal_data);
        static void disconnect_all();

        bool connected() { return is_connected; }

        // XEP-0045 §7.8.2: pending mediated invites for /decline.
        void track_pending_mediated_invite(std::string_view room_jid,
                                           std::string_view inviter_bare,
                                           std::optional<std::string_view> password = std::nullopt);
        [[nodiscard]] std::optional<std::size_t> find_pending_mediated_invite(
            std::string_view room_jid, std::string_view inviter_bare) const;
        void erase_pending_mediated_invite(std::size_t index);

        bool search_device(device* out, std::uint32_t id);
        void add_device(device *device);
        void device_free_all();
        std::shared_ptr<xmpp_stanza_t> get_devicelist();

        void add_mam_query(const std::string id, const std::string with,
                           std::optional<time_t> start, std::optional<time_t> end);
        bool mam_query_search(mam_query* out, const std::string id);
        void mam_query_remove(const std::string id);
        void mam_query_free_all();

        // Global MAM concurrency limiter support
        bool try_acquire_mam_slot();
        void release_mam_slot();

        struct mam_page_defer {
            std::string channel_id;       // empty = global query
            std::string mam_query_id;     // original mam_query id (for lookup)
            std::optional<time_t> start;
            std::optional<time_t> end;
            std::string after;            // RSM <after> token
        };
        std::deque<mam_page_defer> mam_deferred_pages;
        struct t_hook *mam_defer_timer = nullptr;
        int mam_inflight = 0;  // global concurrent MAM fetches (for limiter)
        bool mam_jitter_next_initial = false; // used by #2 jitter logic
        void schedule_next_mam_page();
        static int process_deferred_mam_page_cb(const void *pointer, void *data, int remaining_calls);

        void disconnect(int reconnect);
        void disconnect_reconnect_immediate();
        void reset();
        int connect();
        int connect(bool manual);  // manual=true means user-initiated /connect
        
        [[nodiscard]] stream_ext_cache_caps stream_ext_cache_get(std::string_view domain) const;
        void stream_ext_cache_set(std::string_view domain, stream_ext ext, bool available);

        void mam_cache_init();
        void mam_cache_cleanup();
        void mam_cache_message(std::string_view channel_jid, std::string_view message_id,
                              std::string_view from, time_t timestamp, std::string_view body,
                              bool batched = false);
        void mam_cache_write_batch_commit();
        void mam_cache_retract_message(std::string_view channel_jid, std::string_view message_id);
        bool mam_cache_is_retracted(std::string_view channel_jid, std::string_view message_id);
        void mam_cache_load_messages(std::string_view channel_jid, struct t_gui_buffer *buffer);
        void mam_cache_clear_messages(std::string_view channel_jid);
        time_t mam_cache_get_last_timestamp(std::string_view channel_jid);
        void mam_cache_set_last_timestamp(std::string_view channel_jid, time_t timestamp);
        // OMEMO plaintext cache: store decrypted body on live delivery; look up on MAM replay
        void mam_cache_store_omemo_plaintext(std::string_view channel_jid, std::string_view msg_id,
                                             std::string_view body);
        void mam_cache_store_omemo_plaintext_ids(std::string_view channel_jid,
                                                 const std::vector<std::string> &msg_ids,
                                                 std::string_view body,
                                                 bool batched = false);
        std::expected<std::string, std::string> mam_cache_lookup_omemo_plaintext(std::string_view channel_jid,
                                                                     std::string_view msg_id);
        // ESFS download deduplication: record saved path by stable message ID; look up on MAM replay
        void mam_cache_store_esfs_download(std::string_view channel_jid, std::string_view stable_id,
                                           std::string_view saved_path);
        void mam_cache_store_download_paths(std::string_view channel_jid, std::string_view stable_id,
                                            std::string_view saved_path);
        std::expected<std::string, std::string> mam_cache_lookup_esfs_download(std::string_view channel_jid,
                                                                   std::string_view stable_id);
        void mam_cache_store_image_preview(std::string_view channel_jid, std::string_view stable_id,
                                           std::string_view local_path);
        std::expected<std::string, std::string> mam_cache_lookup_image_preview(std::string_view channel_jid,
                                                                   std::string_view stable_id);
        // PM buffer persistence across restarts (stored in pm_open LMDB table)
        void pm_open_register(std::string_view pm_jid);
        void pm_open_unregister(std::string_view pm_jid);
        std::vector<std::string> pm_open_list();
        std::string mam_cursor_get(std::string_view key);
        void mam_cursor_set(std::string_view key, std::string_view cursor_id);
        void mam_cursor_clear(std::string_view key);  // delete saved cursor to return to latest page
        // Feed item deduplication (stored in cursors LMDB table)
        bool feed_item_seen(std::string_view feed_key, std::string_view item_id);
        void feed_item_mark_seen(std::string_view feed_key, std::string_view item_id);
        // Feed buffer persistence across restarts (stored in feed_open LMDB table)
        void feed_open_register(std::string_view feed_key);
        void feed_open_unregister(std::string_view feed_key);
        [[nodiscard]] bool feed_is_open(std::string_view feed_key) const;
        void feed_open_sync_from_cache();
        void feed_seen_sync_from_cache();
        void migrate_feed_seen_from_cursors();
        void migrate_open_buffers_from_cursors();
        void feed_dismiss(std::string_view feed_key);
        std::vector<std::string> feed_open_list();

        // True when bare_jid is a registered OMEMO recipient in an eligible (non-anon) MUC.
        [[nodiscard]] bool omemo_muc_occupant_in_eligible_room(std::string_view bare_jid) const;
        void feed_atom_id_set(std::string_view feed_key, std::string_view item_id,
                              std::string_view atom_id)
        {
            if (feed_key.empty() || item_id.empty() || atom_id.empty())
                return;
            feed_atom_ids[fmt::format("{}\n{}", feed_key, item_id)] = std::string(atom_id);
        }
        std::string feed_atom_id_get(std::string_view feed_key, std::string_view item_id) const
        {
            auto it = feed_atom_ids.find(fmt::format("{}\n{}", feed_key, item_id));
            return it != feed_atom_ids.end() ? it->second : std::string();
        }
        void feed_replies_link_set(std::string_view feed_key, std::string_view item_id,
                                   std::string_view replies_uri)
        {
            if (feed_key.empty() || item_id.empty() || replies_uri.empty())
                return;
            feed_replies_links[fmt::format("{}\n{}", feed_key, item_id)] = std::string(replies_uri);
        }
        std::string feed_replies_link_get(std::string_view feed_key, std::string_view item_id) const
        {
            auto it = feed_replies_links.find(fmt::format("{}\n{}", feed_key, item_id));
            return it != feed_replies_links.end() ? it->second : std::string();
        }
        // Assign a short #N alias for an item and return N.
        // If the item already has an alias in this feed, return the existing one.
        int feed_alias_assign(std::string_view feed_key, std::string_view item_id)
        {
            if (feed_key.empty() || item_id.empty()) return -1;
            const std::string rev_key = fmt::format("{}\n{}", feed_key, item_id);
            if (auto rev_it = feed_alias_rev.find(rev_key); rev_it != feed_alias_rev.end())
            {
                if (auto n = parse_int64(rev_it->second); n)
                    return static_cast<int>(*n);
                return -1;
            }
            const std::string feed_key_str(feed_key);
            int n = ++feed_alias_ctr[feed_key_str];
            std::string ns = std::to_string(n);
            feed_alias_fwd[fmt::format("{}\n{}", feed_key, ns)] = std::string(item_id);
            feed_alias_rev[rev_key] = ns;
            return n;
        }
        // Resolve a short alias N (as string or "#N") for a feed_key → item_id.
        // Returns empty string if not found.
        std::string feed_alias_resolve(std::string_view feed_key, std::string_view alias) const
        {
            // Strip leading '#' if present.
            if (!alias.empty() && alias[0] == '#')
                alias.remove_prefix(1);
            auto it = feed_alias_fwd.find(fmt::format("{}\n{}", feed_key, alias));
            return it != feed_alias_fwd.end() ? it->second : std::string();
        }
        // Reverse lookup: item_id → alias number for a feed_key. Returns -1 if not found.
        int feed_alias_lookup(std::string_view feed_key, std::string_view item_id) const
        {
            auto it = feed_alias_rev.find(fmt::format("{}\n{}", feed_key, item_id));
            if (it == feed_alias_rev.end()) return -1;
            if (auto n = parse_int64(it->second); n)
                return static_cast<int>(*n);
            return -1;
        }
        void send_bookmarks();
        void retract_bookmark(std::string_view jid);
        
        // Capability cache methods (XEP-0115)
        void caps_cache_load();
        void caps_cache_save(std::string_view verification_hash, const std::vector<std::string>& features);
        bool caps_cache_get(std::string_view verification_hash, std::vector<std::string>& features);

        void muc_title_cache_put(std::string_view room_jid, std::string_view title);
        [[nodiscard]] std::optional<std::string> muc_title_cache_get(std::string_view room_jid);
        void peer_features_update(std::string_view jid, const std::vector<std::string>& features);
        bool peer_supports_feature(std::string_view jid, std::string_view feature) const;
        bool peer_has_legacy_axolotl_only(std::string_view jid) const;

        // Resolve partner bare JID to the canonical channels map key (case-insensitive).
        [[nodiscard]] std::string resolve_channel_key(std::string_view partner_bare) const;
        void invalidate_channel_key_cache(std::string_view canonical_key);

        // OG preview cache methods (XEP-0511)
        struct og_preview {
            std::string title;
            std::string description;
            std::string url;
            std::string image;
        };
        void og_cache_store(std::string_view url, const og_preview& preview);
        std::expected<og_preview, std::string> og_cache_lookup(std::string_view url);

        struct t_gui_buffer* create_buffer();

        [[nodiscard]] bool roster_bare_jid_online(std::string_view bare_jid) const;
        void update_roster_nicklist_entry(std::string_view bare_jid);
        void sync_roster_nicklist();
        void count_roster_nicklist_presence(int &online, int &offline) const;

        [[nodiscard]] LibstropheConnectionPort connection_port()
        {
            return {static_cast<xmpp_conn_t *>(connection), jid_bare_cache_,
                    option_jid.string()};
        }

        std::string jid() { return connection_port().bound_jid_bare(); }
        void jid(std::string jid) { this->option_jid = jid; }
        std::string jid_device() { return connection_port().bound_jid_full(); }
        std::string_view password() { return this->option_password.string(); }
        void password(std::string password) { this->option_password = password; }
        tls_policy tls() { return static_cast<tls_policy>(this->option_tls.integer()); }
        void tls(tls_policy tls) { this->option_tls = fmt::format("%d", std::to_underlying(tls)); }
        void tls(std::string tls) { this->option_tls = tls; }
        std::string_view nickname() { return this->option_nickname.string(); }
        void nickname(std::string nickname) { this->option_nickname = nickname; }
        bool autoconnect() { return this->option_autoconnect.boolean(); }
        void autoconnect(bool autoconnect) { this->option_autoconnect = autoconnect ? "on" : "off"; }
        void autoconnect(std::string autoconnect) { this->option_autoconnect = autoconnect; }
        std::string_view resource() { return this->option_resource.string(); }
        void resource(std::string resource) { this->option_resource = resource; }
        std::string_view status() { return this->option_status.string(); }
        void status(std::string status) { this->option_status = status; }
        std::string_view pgp_path() { return this->option_pgp_path.string(); }
        void pgp_path(std::string pgp_path) { this->option_pgp_path = pgp_path; }
        std::string_view pgp_keyid() { return this->option_pgp_keyid.string(); }
        void pgp_keyid(std::string pgp_keyid) { this->option_pgp_keyid = pgp_keyid; }
        
        void save_pgp_keys();
        void load_pgp_keys();
    };

    extern std::unordered_map<std::string, account>& accounts;
}
