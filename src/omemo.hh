// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <memory>
#include <functional>
#include <set>
#include <unordered_map>
#include <utility>

// RAII owner for malloc()/calloc()-allocated byte buffers.
using heap_buf = std::unique_ptr<uint8_t[], decltype(&free)>;
inline heap_buf make_heap_buf(uint8_t *p) { return {p, free}; }
#include <cstdint>
#include <string>
#include <strophe.h>
#include <lmdb++.h>
#include <signal_protocol.h>

#include "signal.hh"

extern const char *OMEMO_ADVICE;

namespace weechat {
    class account;

    namespace xmpp {
        struct t_pre_key {
            const char *id;
            const char *public_key;
        };

        struct omemo
        {
            // IMPORTANT: C++ destroys members in reverse declaration order.
            // Desired destruction order: store_context first, context second, db_env last.
            // So declare in reverse: db_env first (destroyed last),
            // context second, store_context third (destroyed first).
            lmdb::env db_env = nullptr;
            struct dbi {
                lmdb::dbi omemo = 0;
            } dbi;
            std::string db_path;

            libsignal::context context;
            libsignal::store_context store_context;

            libsignal::identity_key_pair identity;

            std::uint32_t device_id;

            // Devices for which we need to send a KeyTransportElement once
            // their bundle has been fetched and the session built.
            // Populated in decode() when keys_for_this_device==0;
            // drained in handle_bundle() after bks_store_bundle().
            std::set<std::pair<std::string, std::uint32_t>> pending_key_transport;

            // Devices for which a bundle fetch IQ is currently in-flight.
            // Prevents duplicate fetches when repeated PEP devicelist events
            // arrive before the first IQ result returns.
            // Populated before sending the bundle IQ; cleared in handle_bundle().
            std::set<std::pair<std::string, std::uint32_t>> pending_bundle_fetch;

            // Maps outgoing IQ id → target JID for bundle/devicelist PubSub
            // fetches directed at a contact. Used in the IQ result handler to
            // recover the correct JID even when `from` is the server domain.
            std::unordered_map<std::string, std::string> pending_iq_jid;

            // Maps configure-IQ id → node name for pending precondition-not-met
            // recovery.  When a bundle or devicelist publish fails with
            // <precondition-not-met/>, we send a node configure IQ and record
            // the id here.  On the configure result we re-publish the node.
            std::unordered_map<std::string, std::string> pending_configure_retry;

            class bundle_request
            {
            public:
                std::string id;
                std::string jid;
                std::string device;
                std::string message_text;
            };

            class devicelist_request
            {
            public:
                std::string id;
                bundle_request bundle_req;
            };

            ~omemo();

            inline operator bool() { return this->context && this->store_context &&
                    this->identity && this->device_id != 0; }

            xmpp_stanza_t *get_bundle(xmpp_ctx_t *context, char *from, char *to);

            void init(struct t_gui_buffer *buffer, const char *account_name);

            void handle_devicelist(const char *jid, xmpp_stanza_t *items);

            void handle_bundle(weechat::account *account,
                               struct t_gui_buffer *buffer,
                               const char *jid, std::uint32_t device_id,
                               xmpp_stanza_t *items);

            // Returns true if a Signal session already exists for (jid, device_id).
            bool has_session(const char *jid, std::uint32_t device_id);

            char *decode(weechat::account *account, struct t_gui_buffer *buffer,
                         const char *jid, xmpp_stanza_t *encrypted);

            xmpp_stanza_t *encode(weechat::account *account, struct t_gui_buffer *buffer,
                                  const char *jid, const char *unencrypted);

            // Key management helpers
            // Show fingerprint (hex of public identity key) for own key or a peer JID.
            // jid == nullptr → show own key; otherwise show stored peer keys.
            void show_fingerprint(struct t_gui_buffer *buffer, const char *jid);

            // Delete all stored identity keys for jid (all device IDs) so the next
            // message from that JID triggers TOFU re-store.
            void distrust_jid(struct t_gui_buffer *buffer, const char *jid);

            // Show all known device IDs for jid (from the stored devicelist).
            void show_devices(struct t_gui_buffer *buffer, const char *jid);

            // Show full OMEMO status: own fingerprint, device ID, pre-key count,
            // SPK ID and age.  channel_name may be NULL.
            void show_status(struct t_gui_buffer *buffer, const char *account_name,
                             const char *channel_name, int channel_omemo_enabled);

            // Proactively fetch the OMEMO devicelist for `jid` from the server.
            // Safe to call even if a fetch is already in-flight (deduplication is
            // handled in the IQ result handler via pending_iq_jid).
            void request_devicelist(weechat::account &account, std::string_view jid);
        };
    }
}
