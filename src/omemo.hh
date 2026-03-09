// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <memory>
#include <functional>
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

            void handle_bundle(const char *jid, std::uint32_t device_id,
                               xmpp_stanza_t *items);

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
        };
    }
}
