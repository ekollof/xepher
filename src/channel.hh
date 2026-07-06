// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>
#include <ctime>
#include <span>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <optional>

#define CHANNEL_MEMBERS_SPEAKING_LIMIT 128

namespace stanza {
    struct message;
}

namespace weechat
{
    class UiPort;
    class account;
    class user;

    struct add_member_opts
    {
        bool announce_join = true;
        bool online = true;
    };

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
            // XEP-0300 hash agility: (algo, base64) pairs for <file> <hash/> elements.
            std::vector<std::pair<std::string, std::string>> hashes;
            size_t width  = 0;        // Image width  in pixels (0 = unknown/non-image)
            size_t height = 0;        // Image height in pixels (0 = unknown/non-image)
            std::string file_date;    // XEP-0446 <date> (UTC ISO-8601)

            // XEP-0448: Encrypted File Sharing — set when the upload was AES-256-GCM encrypted.
            // When present, the <sources> element will contain an <encrypted xmlns='urn:xmpp:esfs:0'>
            // child instead of a plain <url-data>.
            struct esfs_info
            {
                std::string key_b64;          // Base64(32-byte AES-256-GCM key)
                std::string iv_b64;           // Base64(12-byte GCM IV)
                std::string cipher_hash_b64;  // Base64(SHA-256 of ciphertext incl. tag)
            };
            std::optional<esfs_info> esfs;    // non-empty iff encrypted upload
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
                    return nullptr;
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
            std::optional<std::string> real_jid;  // occupant's real JID if room is non-anonymous
            bool present = true;  // false when affiliated but not currently in the room
        };

        struct topic
        {
            std::optional<std::string> value;
            std::optional<std::string> creator;
            time_t last_set = 0;
        };

        // XEP-0045 §6.4 + §6.5: room metadata discovered via disco#info.
        // Populated by the disco#info handler; rendered to the buffer's "modes"
        // property (IRC-style) by update_modes(). Full metadata is printed
        // on demand by /modes.
        struct muc_info
        {
            // Mode flags (XEP-0045 §16.3 feature registry, muc_* vars).
            bool moderated       = false; // muc_moderated
            bool members_only    = false; // muc_membersonly
            bool persistent      = false; // muc_persistent
            bool password        = false; // muc_passwordprotected
            bool hidden          = false; // muc_hidden
            enum class anonymity { unknown, nonanonymous, semianonymous, anonymous };
            anonymity anon       = anonymity::unknown;
            // muc#roominfo_* x-data form fields (only populated when the server
            // returns the FORM_TYPE in disco#info).
            std::optional<std::string> description;
            std::optional<std::string> language;
            std::optional<std::string> subject;
            std::optional<std::string> logs_url;
            std::optional<int>         occupants;
            std::optional<int>         max_users;
            bool subject_modifiable = true; // muc#roominfo_subjectmod
            // XEP-0045 status 102/103: show affiliated members who left the room.
            bool show_unavailable_members = true;
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

        // XEP-0045 §10.2: a single <field> in the muc#roomconfig form.
        // Public so the IQ result handler (in a different TU) can construct
        // values. Preserves the server's original ordering and any
        // server-added read-only fields (e.g. muc#roomconfig_roomadmins) so
        // submit round-trips cleanly.
        struct room_config_field {
            std::string var;
            std::string type;            // "boolean", "text-single", etc. — empty if absent
            std::vector<std::string> values;  // first entry is the "default" for fixed lists
            std::string label;           // for display only (returned by form, ignored on submit)
        };
        struct room_config_form {
            std::vector<room_config_field> fields;
            // sessionid attribute on the <x/> element (XEP-0045 §10.2). Empty
            // string when not present (some servers omit it).
            std::string sessionid;
            // Last refresh time — used to invalidate on a stale form (e.g. if
            // a status 104 fires between get and submit).
            time_t fetched_at = 0;
        };

        // XEP-0045 §10.2: when /setmodes --confirm runs without a cached
        // form, we stash the requested diff here and send a GET. The
        // config_get result handler picks it up, applies it to the
        // fetched form, and submits a full read-modify-write. This is the
        // robust path that preserves all server-side fields per XEP-0004
        // §3.4 (partial submits are rejected by some servers).
        struct pending_setmodes_diff {
            // Index mapping: 0=m, 1=i, 2=k, 3=p, 4=P, 5=N, 6=S
            bool want_set[7]   = {false, false, false, false, false, false, false};
            bool want_clear[7] = {false, false, false, false, false, false, false};
            std::string password;       // only meaningful when want_set[2]
            time_t queued_at = 0;
        };

    private:
        topic topic;

        // XEP-0045 room metadata. Populated by the disco#info handler in
        // src/connection/iq_handler.inl. Rendered by update_modes() and the
        // /modes command (command/muc_admin.inl).
        muc_info muc_info_;

        // XEP-0045 §10.2: last muc#roomconfig form received from the room.
        // Populated by the muc#owner IQ result handler; consumed by /setmodes
        // to compute diffs and re-submit.
        std::optional<room_config_form> last_config_form;

        // XEP-0045 §10.2: pending /setmodes --confirm diff waiting for a form
        // GET to complete. Set by command__setmodes when no form is cached;
        // consumed by the config_get result handler which applies the diff
        // and submits a full read-modify-write.
        std::optional<pending_setmodes_diff> pending_setmodes;

        /* mpim */
        std::optional<std::string> creator;
        double last_read = 0.0;
        int unread_count = 0;
        int unread_count_display = 0;

        struct t_hook *self_typing_hook_timer = nullptr;

    public:
        std::vector<weechat::channel::unread> unreads;

        // XEP-0490: track the last incoming MUC message so send_reads() can
        // publish MDS PEP when leaving the buffer.  MUC messages suppress
        // <displayed> markers (XEP-0333 §4.1) so they never populate
        // `unreads`; this separate field bridges that gap.
        std::optional<weechat::channel::unread> muc_last_seen;

    public:
        std::string id;
        std::string name;
        enum chat_type type;
        enum transport transport = weechat::channel::transport::PLAIN;
        struct {
            int enabled;
            struct t_hashtable *devicelist_requests;
            struct t_hashtable *bundle_requests;
            // MUC OMEMO: pending "bare_jid/device_id" bundle fetches for this room.
            std::unordered_set<std::string> pending_muc_bundle_keys;
        } omemo;
        // XEP-0384 §5.8.1: bare JIDs from member/admin/owner affiliation lists.
        std::unordered_set<std::string> omemo_recipient_jids;
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
        void apply_muc_display_name(std::string_view display_name);
        void update_purpose(const char* purpose, const char* creator, int last_set);

        // XEP-0045 §10.2: store the last muc#roomconfig form received from
        // the room. Called by the muc#owner IQ result handler. Drops the
        // previous form (caller is responsible for re-fetching if needed).
        void store_config_form(room_config_form form);

        // XEP-0045 §10.2: access the cached muc#roomconfig form (or nullopt
        // if /setmodes has not yet fetched one this session).
        std::optional<room_config_form> get_config_form() const { return last_config_form; }

        // XEP-0045 §10.2: drop the cached form. Used after a successful
        // /setmodes submit (form is now stale — next op re-fetches) and by
        // disconnect cleanup.
        void clear_config_form() { last_config_form.reset(); }

        // XEP-0045 §10.2: stash a /setmodes --confirm diff for the form GET
        // result handler to apply. Pass the diff by value; the channel
        // moves it into the optional.
        void set_pending_setmodes(pending_setmodes_diff d)
        {
            d.queued_at = ::time(nullptr);
            pending_setmodes = std::move(d);
        }

        // XEP-0045 §10.2: take ownership of the pending diff (if any). The
        // caller (the config_get result handler) gets the diff and the
        // channel's optional is reset to nullopt.
        std::optional<pending_setmodes_diff> take_pending_setmodes()
        {
            return std::move(pending_setmodes);
        }

        // XEP-0045: read accessors for room metadata.
        const muc_info& get_muc_info() const { return muc_info_; }
        // Apply parsed disco#info features/form fields to muc_info_.
        // Called by the disco#info handler in iq_handler.inl when the from-JID
        // matches this MUC channel.
        void apply_muc_info(const muc_info &incoming);

        // XEP-0004 §3.4/§3.5: prepare a muc#roomconfig form for submit.
        // Boolean fields without a parsed value default to "0".
        static void prepare_room_config_submit(room_config_form &form);
        // Omit fields with no values so partial submits preserve server state.
        [[nodiscard]] static bool include_room_config_field_in_submit(
            const room_config_field &f);

        // Render muc_info_ to the buffer's "modes" property (IRC-style).
        // No-op for non-MUC channels.
        void update_modes();

        std::optional<member*> add_member(std::string_view id,
                                           std::string_view client = {},
                                           std::optional<std::string_view> real_jid = std::nullopt,
                                           weechat::user *known_user = nullptr,
                                           add_member_opts opts = {});
        std::optional<member*> member_search(std::string_view id);
        std::optional<member*> remove_member(std::string_view id, std::string_view reason = {});
        void set_member_offline(std::string_view id, weechat::user *known_user = nullptr);
        void set_show_unavailable_members(bool show);
        void count_nicklist_presence(int &online, int &offline) const;
        std::string find_member_by_nick(std::string_view nick) const;

        // For MUC OMEMO: true only when every *online* occupant has a real_jid.
        [[nodiscard]] bool all_occupants_have_real_jid() const;

        // Online occupant nicks lacking real_jid (empty when all present members have one).
        [[nodiscard]] std::vector<std::string> online_occupants_missing_real_jid() const;

        // Log (XDEBUG) and print the standard real-JID guard error for send /omemo.
        void notify_omemo_missing_real_jids(weechat::UiPort &ui) const;

        // XEP-0384 §5.8: OMEMO group chat requires a non-anonymous MUC.
        [[nodiscard]] bool muc_supports_omemo() const;

        // True when the room is OMEMO-eligible, we have recipients, online occupants
        // have visible real JIDs, and no bundle fetches are outstanding.
        [[nodiscard]] bool muc_omemo_ready() const;

        void register_omemo_recipient(std::string_view bare_jid);
        void unregister_omemo_recipient(std::string_view bare_jid);
        [[nodiscard]] std::vector<std::string> omemo_recipient_list() const;

        // XEP-0384 §5.8.1: only member/admin/owner affiliations are encrypt targets.
        [[nodiscard]] static bool is_omemo_recipient_affiliation(std::string_view affiliation);

        void mark_omemo_bundle_pending(std::string_view bare_jid, std::uint32_t device_id);
        void clear_omemo_bundle_pending(std::string_view bare_jid, std::uint32_t device_id);

        void set_muc_anonymity(muc_info::anonymity anon);
        void maybe_disable_muc_omemo();

        // Short status string used by the xmpp_encryption bar item.
        // For MUC OMEMO with pending bundles this can surface "pending" state.
        std::string omemo_status() const;

        // Smart filter helpers
        void record_speak(const char *nick);
        bool smart_filter_nick(const char *nick) const;

        // XEP-0085 chat state support tracking
        void mark_chat_state_supported(std::string_view jid);
        bool is_chat_state_supported(std::string_view jid) const;

        int send_message(std::string to, std::string body,
                         std::optional<std::string> oob = {},
                         std::optional<file_metadata> file_meta = {},
                         std::optional<std::string> local_preview_path = {});

        // XEP-0231: send a small image (≤8 KiB) via BoB + XHTML-IM (plaintext only).
        int send_bob_image(std::string_view to,
                           std::span<const std::uint8_t> data,
                           std::string_view mime,
                           std::string_view alt = "image");
        // Low-level overload: skip_probe must be supplied explicitly to avoid
        // ambiguity with the std::string overload above.
        int send_message(std::string_view to, std::string_view body, bool skip_probe);

        // Build a file share message stanza (SFS + SIMS + OOB + fallbacks) for a target.
        // Used by channel::send_message(file) and by upload fd_cb fallback (when local
        // channel entry is gone after /close or race). Returns unbuilt message (no body
        // or OMEMO yet) so the caller can set body and optionally wrap before building.
        // The caller provides the pre-generated saved_id (for origin-id + later cache).
        static stanza::message make_file_share_stanza(xmpp_ctx_t *xmpp_ctx,
            std::string_view to, const char *msg_type /*"chat" or "groupchat"*/,
            std::string_view saved_id,
            std::string_view body, std::string_view oob_url,
            const file_metadata& meta);

        void queue_pending_omemo_message(std::string_view body);
        void flush_pending_omemo_messages();

        void send_link_preview(std::string_view to, std::string_view url);

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
