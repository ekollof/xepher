// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <ctime>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <optional>

#define CHANNEL_MEMBERS_SPEAKING_LIMIT 128

namespace weechat
{
    class account;
    class user;

    class channel
    {
    public:
        enum class chat_type { MUC, PM, FEED };

        enum class transport { PLAIN, OMEMO, PGP, OTR, OX };

        // XEP-0385: SIMS file metadata
        struct file_metadata
        {
            std::string filename;
            std::string content_type;
            size_t size;
            std::string sha256_hash;  // Base64-encoded SHA-256
        };

        static const char *transport_name(enum transport transport)
        {
            switch (transport)
            {
                case transport::PLAIN:
                    return "PLAINTEXT";
                case transport::OMEMO:
                    return "OMEMO";
                case transport::PGP:
                    return "PGP";
                case transport::OTR:
                    return "OTR";
                default:
                    return NULL;
            }
        }

        struct typing
        {
            weechat::user *user;
            std::string name;
            time_t ts;
        };

        struct member
        {
            std::string id;

            std::optional<std::string> role;
            std::optional<std::string> affiliation;
        };

        struct topic
        {
            std::optional<std::string> value;
            std::optional<std::string> creator;
            time_t last_set = 0;
        };

        struct unread
        {
            std::string id;
            std::optional<std::string> thread;
            // XEP-0490: server-assigned stanza-id and its `by` JID so the MDS
            // PEP publish correctly references the archiver's stanza-id.
            std::optional<std::string> stanza_id;
            std::optional<std::string> stanza_id_by;
        };

    private:
        topic topic;

        /* mpim */
        std::optional<std::string> creator;
        double last_read = 0.0;
        int unread_count = 0;
        int unread_count_display = 0;

        struct t_hook *self_typing_hook_timer = nullptr;

    public:
        std::vector<weechat::channel::unread> unreads;

    public:
        std::string id;
        std::string name;
        enum chat_type type;
        enum transport transport = weechat::channel::transport::PLAIN;
        struct {
            int enabled;
            struct t_hashtable *devicelist_requests;
            struct t_hashtable *bundle_requests;
        } omemo;
        struct {
            int enabled = 0;
            std::unordered_set<std::string> ids;
        } pgp;
        struct {
            int enabled = 0;
       } otr;
        struct t_weelist *members_speaking[2] = { nullptr };
        std::vector<typing> self_typings;
        std::unordered_map<std::string, member> members;
        
        time_t last_mam_fetch = 0;

        // Smart filter: last time each nick spoke (key = resource nick in MUC)
        std::unordered_map<std::string, time_t> last_speak;
        // Set to true while the initial MUC join presence flood is in progress
        // (between receiving status 110 and the first non-presence stanza / explicit reset)
        bool joining = false;

        // XEP-0085: JIDs that have sent us at least one chat state notification,
        // meaning they support the protocol and we may send states back to them.
        // For MUC we always send (room echoes tell us all clients support it).
        std::unordered_set<std::string> chat_state_supported;

        // XEP-0085 §5.2: Duplicate suppression — track the last chat state we
        // sent to each JID (key = bare JID, value = state element name).
        // Don't re-send the same state twice in a row to the same recipient.
        std::unordered_map<std::string, std::string> last_sent_chat_state;

        // Conversations-style deferred OMEMO send queue for PMs.
        // When we cannot encrypt yet (missing sessions/bundles), we queue
        // plaintexts here and flush them once sessions become available.
        std::deque<std::string> pending_omemo_messages;
        bool flushing_pending_omemo = false;

    public:
        struct t_gui_buffer *buffer;

    public:
        channel(weechat::account& account, enum chat_type type, std::string_view id, std::string_view name);
        ~channel();

        void set_transport(enum weechat::channel::transport transport, int force);

        struct t_gui_buffer *search_buffer(weechat::channel::chat_type type,
                                           const char *name);
        struct t_gui_buffer *create_buffer(weechat::channel::chat_type type,
                                           const char *name);

        void add_nicklist_groups();

        void member_speaking_add_to_list(const char *nick, int highlight);
        void member_speaking_add(const char *nick, int highlight);
        void member_speaking_rename(const char *old_nick, const char *new_nick);
        void member_speaking_rename_if_present(const char *nick);

        int add_typing(weechat::user *user);
        int remove_typing(weechat::user *user);

    private:
        int set_typing_state(weechat::user *user, const char *state);
        void send_chat_state(weechat::user *user, const char *state);

    public:

        static int self_typing_cb(const void *pointer, void *data, int remaining_calls);
        std::optional<typing*> self_typing_search(weechat::user *user);
        int add_self_typing(weechat::user *user);

        void free(channel *channel);
        void free_all();

        void update_topic(const char* title, const char* creator, int last_set);
        void update_name(const char* name);
        void update_purpose(const char* purpose, const char* creator, int last_set);

        std::optional<member*> add_member(const char *id, const char *client);
        std::optional<member*> member_search(const char *id);
        std::optional<member*> remove_member(const char *id, const char *reason);

        // Smart filter helpers
        void record_speak(const char *nick);
        bool smart_filter_nick(const char *nick) const;

        // XEP-0085 chat state support tracking
        void mark_chat_state_supported(const std::string& jid);
        bool is_chat_state_supported(const std::string& jid) const;

        int send_message(std::string to, std::string body,
                         std::optional<std::string> oob = {},
                         std::optional<file_metadata> file_meta = {});
        // Low-level overload: skip_probe must be supplied explicitly to avoid
        // ambiguity with the std::string overload above.
        int send_message(std::string_view to, std::string_view body, bool skip_probe);

        void queue_pending_omemo_message(const std::string& body);
        void flush_pending_omemo_messages();

        void send_link_preview(const std::string& to, const std::string& url);

        void send_reads();
        void send_active(weechat::user *user);
        void send_typing(weechat::user *user);
        void send_paused(weechat::user *user);
        void send_inactive(weechat::user *user);
        void send_gone(weechat::user *user);

        void fetch_mam(const char *id, time_t *start, time_t *end, const char *after);

        weechat::account& account;
    };
}
