// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <ctime>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <optional>
#include <utility>
#include <lmdb++.h>

#include "fmt/core.h"
#include "strophe.h"
#include "pgp.hh"
#include "omemo.hh"
#include "config.hh"
#include "channel.hh"
#include "connection.hh"
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
            std::string sha256_hash;      // SHA-256 hash for SIMS
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
            std::string filename;
            std::string content_type;
            size_t file_size = 0;
            std::string sha256_hash;
            size_t image_width  = 0;    // Image width  in pixels (0 = unknown/non-image)
            size_t image_height = 0;    // Image height in pixels (0 = unknown/non-image)
            long http_code = 0;
            std::string curl_error;
            int pipe_write_fd = -1;     // write end (closed by thread after writing)
            struct t_hook *hook = nullptr; // weechat_hook_fd (filled after thread starts)
            std::thread worker;         // owns the background thread
            // Non-empty when this upload belongs to a pending feed post (embed tag).
            // Set to the upload slot IQ id that originated this upload.
            std::string feed_post_upload_id;
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

    public:
        // Client State Indication (XEP-0352)
        bool csi_active = true;
        time_t last_activity = 0;
        struct t_hook *idle_timer_hook = nullptr;
        struct t_hook *csi_activity_hooks[3] = {nullptr, nullptr, nullptr};  // Store activity signal hooks

        // Stream Management (XEP-0198)
        bool sm_enabled = false;
        bool sm_available = true;       // Can we try to enable SM?
        bool sm_handlers_registered = false;  // Track if handlers are already added
        std::string sm_id;              // session ID for resumption
        uint32_t sm_h_inbound = 0;      // stanzas we've received and handled
        uint32_t sm_h_outbound = 0;     // stanzas we've sent
        uint32_t sm_last_ack = 0;       // last h value acknowledged by server
        struct t_hook *sm_ack_timer_hook = nullptr;
        // Unacknowledged outbound stanzas: (outbound_seq, stanza_copy)
        // seq is 1-based (matches sm_h_outbound at time of send)
        std::deque<std::pair<uint32_t, std::shared_ptr<xmpp_stanza_t>>> sm_outqueue;

        std::string name;
        weechat::xmpp::pgp pgp;
        weechat::xmpp::omemo omemo;
        libstrophe::context context;
        weechat::connection connection;
        struct t_gui_buffer *buffer = nullptr;
        std::unordered_map<std::string, weechat::channel> channels;
        std::unordered_map<std::string, weechat::user> users;

        std::unordered_map<std::string, struct t_config_option *> options;
        
        std::unordered_set<std::string> user_disco_queries;
        std::unordered_set<std::string> user_disco_items_queries;  // disco#items queries initiated by /disco items
        std::unordered_map<std::string, time_t> user_ping_queries;  // ping_id -> start_time
        std::unordered_map<std::string, std::string> caps_disco_queries;  // disco_id -> verification_hash
        std::unordered_map<std::string, std::string> upload_disco_queries;  // disco_id -> service_jid

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
        std::unordered_map<std::string, std::vector<std::string>> peer_features;
        
        // MAM cache database
        lmdb::env mam_db_env = nullptr;
        struct mam_dbi {
            lmdb::dbi messages = 0;
            lmdb::dbi timestamps = 0;
            lmdb::dbi capabilities = 0;  // XEP-0115 capability cache
            lmdb::dbi retractions = 0;   // XEP-0424 retracted message IDs
            lmdb::dbi cursors = 0;       // RSM cursor persistence for MAM queries
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

        bool search_device(device* out, std::uint32_t id);
        void add_device(device *device);
        void device_free_all();
        xmpp_stanza_t *get_devicelist();
        xmpp_stanza_t *get_legacy_devicelist();

        void add_mam_query(const std::string id, const std::string with,
                           std::optional<time_t> start, std::optional<time_t> end);
        bool mam_query_search(mam_query* out, const std::string id);
        void mam_query_remove(const std::string id);
        void mam_query_free_all();

        void disconnect(int reconnect);
        void reset();
        int connect();
        int connect(bool manual);  // manual=true means user-initiated /connect
        
        void mam_cache_init();
        void mam_cache_cleanup();
        void mam_cache_message(const std::string& channel_jid, const std::string& message_id,
                              const std::string& from, time_t timestamp, const std::string& body);
        void mam_cache_retract_message(const std::string& channel_jid, const std::string& message_id);
        bool mam_cache_is_retracted(const std::string& channel_jid, const std::string& message_id);
        void mam_cache_load_messages(const std::string& channel_jid, struct t_gui_buffer *buffer);
        void mam_cache_clear_messages(const std::string& channel_jid);
        time_t mam_cache_get_last_timestamp(const std::string& channel_jid);
         void mam_cache_set_last_timestamp(const std::string& channel_jid, time_t timestamp);
        std::string mam_cursor_get(const std::string& key);
        void mam_cursor_set(const std::string& key, const std::string& cursor_id);
        void mam_cursor_clear(const std::string& key);  // delete saved cursor to return to latest page
        // Feed item deduplication (stored in cursors LMDB table)
        bool feed_item_seen(const std::string& feed_key, const std::string& item_id);
        void feed_item_mark_seen(const std::string& feed_key, const std::string& item_id);
        // Feed buffer persistence across restarts (stored in cursors LMDB table)
        void feed_open_register(const std::string& feed_key);
        void feed_open_unregister(const std::string& feed_key);
        std::vector<std::string> feed_open_list();
        void feed_atom_id_set(const std::string& feed_key, const std::string& item_id,
                              const std::string& atom_id)
        {
            if (feed_key.empty() || item_id.empty() || atom_id.empty())
                return;
            feed_atom_ids[feed_key + "\n" + item_id] = atom_id;
        }
        std::string feed_atom_id_get(const std::string& feed_key, const std::string& item_id) const
        {
            auto it = feed_atom_ids.find(feed_key + "\n" + item_id);
            return it != feed_atom_ids.end() ? it->second : std::string();
        }
        void feed_replies_link_set(const std::string& feed_key, const std::string& item_id,
                                   const std::string& replies_uri)
        {
            if (feed_key.empty() || item_id.empty() || replies_uri.empty())
                return;
            feed_replies_links[feed_key + "\n" + item_id] = replies_uri;
        }
        std::string feed_replies_link_get(const std::string& feed_key, const std::string& item_id) const
        {
            auto it = feed_replies_links.find(feed_key + "\n" + item_id);
            return it != feed_replies_links.end() ? it->second : std::string();
        }
        // Assign a short #N alias for an item and return N.
        // If the item already has an alias in this feed, return the existing one.
        int feed_alias_assign(const std::string& feed_key, const std::string& item_id)
        {
            if (feed_key.empty() || item_id.empty()) return -1;
            auto rev_key = feed_key + "\n" + item_id;
            auto rev_it = feed_alias_rev.find(rev_key);
            if (rev_it != feed_alias_rev.end())
                return std::stoi(rev_it->second);
            int n = ++feed_alias_ctr[feed_key];
            std::string ns = std::to_string(n);
            feed_alias_fwd[feed_key + "\n" + ns] = item_id;
            feed_alias_rev[rev_key] = ns;
            return n;
        }
        // Resolve a short alias N (as string or "#N") for a feed_key → item_id.
        // Returns empty string if not found.
        std::string feed_alias_resolve(const std::string& feed_key, std::string_view alias) const
        {
            // Strip leading '#' if present.
            if (!alias.empty() && alias[0] == '#')
                alias.remove_prefix(1);
            auto it = feed_alias_fwd.find(feed_key + "\n" + std::string(alias));
            return it != feed_alias_fwd.end() ? it->second : std::string();
        }
        // Reverse lookup: item_id → alias number for a feed_key. Returns -1 if not found.
        int feed_alias_lookup(const std::string& feed_key, const std::string& item_id) const
        {
            auto it = feed_alias_rev.find(feed_key + "\n" + item_id);
            if (it == feed_alias_rev.end()) return -1;
            try { return std::stoi(it->second); } catch (...) { return -1; }
        }
        void send_bookmarks();
        void retract_bookmark(std::string_view jid);
        
        // Capability cache methods (XEP-0115)
        void caps_cache_load();
        void caps_cache_save(const std::string& verification_hash, const std::vector<std::string>& features);
        bool caps_cache_get(const std::string& verification_hash, std::vector<std::string>& features);
        void peer_features_update(const std::string& jid, const std::vector<std::string>& features);
        bool peer_supports_feature(const std::string& jid, const std::string& feature) const;
        bool peer_has_legacy_axolotl_only(const std::string& jid) const;

        struct t_gui_buffer* create_buffer();

        std::string_view jid() {
            if (connection && xmpp_conn_is_connected(connection))
                return xmpp_jid_bare(context, xmpp_conn_get_bound_jid(connection));
            else
                return this->option_jid.string();
        }
        void jid(std::string jid) { this->option_jid = jid; }
        std::string_view jid_device() {
            if (connection && xmpp_conn_is_connected(connection))
                return xmpp_conn_get_bound_jid(connection);
            else
                return xmpp_jid_new(context,
                                    xmpp_jid_node(context, this->option_jid.string().data()),
                                    xmpp_jid_domain(context, this->option_jid.string().data()),
                                    "weechat");
        }
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
