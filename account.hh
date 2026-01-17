// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <ctime>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
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
        std::unordered_map<std::string, time_t> user_ping_queries;  // ping_id -> start_time
        std::unordered_map<std::string, std::string> caps_disco_queries;  // disco_id -> verification_hash
        std::unordered_map<std::string, std::string> upload_disco_queries;  // disco_id -> service_jid
        
        // Capability cache (XEP-0115)
        std::unordered_map<std::string, std::vector<std::string>> caps_cache;  // verification_hash -> features
        
        // MAM cache database
        lmdb::env mam_db_env = nullptr;
        struct mam_dbi {
            lmdb::dbi messages = 0;
            lmdb::dbi timestamps = 0;
            lmdb::dbi capabilities = 0;  // XEP-0115 capability cache
            lmdb::dbi retractions = 0;   // XEP-0424 retracted message IDs
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
        static int activity_cb(const void *pointer, void *data,
                              const char *signal, const char *type_data,
                              void *signal_data);
        static void disconnect_all();

        bool connected() { return is_connected; }

        bool search_device(device* out, std::uint32_t id);
        void add_device(device *device);
        void device_free_all();
        xmpp_stanza_t *get_devicelist();

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
        void send_bookmarks();
        
        // Capability cache methods (XEP-0115)
        void caps_cache_load();
        void caps_cache_save(const std::string& verification_hash, const std::vector<std::string>& features);
        bool caps_cache_get(const std::string& verification_hash, std::vector<std::string>& features);

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
