// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <strophe.h>
#include "xmpp/ns.hh"
#include "xmpp/stanza_view.hh"
#include "strophe.hh"
#include "config.hh"

namespace weechat {
    class account;

    class connection
    {
    private:
        libstrophe::connection m_conn;

        enum class event {
            connect = XMPP_CONN_CONNECT,
            raw_connect = XMPP_CONN_RAW_CONNECT,
            disconnect = XMPP_CONN_DISCONNECT,
            fail = XMPP_CONN_FAIL,
        };

    public:
        weechat::account &account;

        connection(weechat::account &acc, libstrophe::context &ctx)
            : m_conn(ctx), account(acc) {
        }

        inline operator xmpp_conn_t*() {
            return m_conn;
        }

        void send(xmpp_stanza_t *stanza);
        void send_threadsafe(xmpp_stanza_t *stanza);

        inline auto context() {
            return m_conn.get_context();
        }

        inline bool connect_client(const char* altdomain, unsigned short altport, xmpp_conn_handler callback) {
            return m_conn.connect_client(altdomain, altport, callback, this) == XMPP_EOK;
        }

        inline auto handler_add(const char *name, const char *type, xmpp_handler callback) {
            return m_conn.handler_add(callback, nullptr, name, type, this);
        }

        template <typename X, std::enable_if_t<std::is_base_of<xmlns,X>::value, int> = 0>
        inline auto handler_add(const char *name, const char *type, xmpp_handler callback) {
            return m_conn.handler_add(callback, X(), name, type, this);
        }

        static void init();

        int connect(std::string jid, std::string password, weechat::tls_policy tls);

        void process(xmpp_ctx_t *context, const unsigned long timeout);

        bool version_handler(xmpp_stanza_t *stanza);
        bool time_handler(xmpp_stanza_t *stanza);
        bool presence_handler(xmpp_stanza_t *stanza, bool top_level = true);
        bool message_handler(xmpp_stanza_t *stanza, bool top_level = true, bool is_mam_replay = false,
                             std::string_view override_archive_id = {},
                             std::string_view override_delay_stamp = {},
                             bool is_carbon_copy = false);
        void handle_pubsub_pep_event(xmpp_stanza_t *stanza, std::string_view own_jid_str);
        bool handle_ping_iq_event(xmpp_stanza_t *stanza, std::string_view own_jid_str);
        bool handle_avatar_pubsub_iq_event(xmpp_stanza_t *stanza, std::string_view own_jid_str);
        bool handle_bob_iq_event(xmpp_stanza_t *stanza, std::string_view own_jid_str);
        bool handle_pubsub_feed_iq_event(xmpp_stanza_t *stanza);
        bool handle_omemo_pubsub_iq_event(xmpp_stanza_t *stanza, std::string_view own_jid_str);
        bool handle_upload_slot_iq_event(xmpp_stanza_t *stanza);
        bool handle_upload_slot_iq_error(xmpp_stanza_t *stanza);
        void handle_mam_query_iq_error(xmpp_stanza_t *stanza);
        bool handle_mam_fin_iq_event(xmpp_stanza_t *stanza);
        void handle_pubsub_mam_disco_iq_error(xmpp_stanza_t *stanza);
        bool handle_disco_items_iq_event(xmpp_stanza_t *stanza);
        void handle_adhoc_command_iq_event(xmpp_stanza_t *stanza);
        bool handle_channel_search_iq_event(xmpp_stanza_t *stanza);
        bool handle_disco_info_iq_event(xmpp_stanza_t *stanza);
        bool handle_vcard_iq_event(xmpp_stanza_t *stanza, std::string_view own_jid_str);
        bool handle_vcard4_pubsub_iq_event(xmpp_stanza_t *stanza, std::string_view own_jid_str);
        bool handle_bookmarks_iq_event(xmpp_stanza_t *stanza);
        bool iq_handler(xmpp_stanza_t *stanza, bool top_level = true);
        bool sm_handler(xmpp_stanza_t *stanza);

        bool conn_handler(event status, int error, xmpp_stream_error_t *stream_error);
        void run_post_connect_setup(bool resumed_session);
        void run_account_connect_probes(bool resumed_session);
        void request_server_disco_probes(bool resumed_session);
        void run_optional_server_probes(bool resumed_session,
                                        std::span<const std::string> server_features);

        void send_sm_graceful_ack();

        xmpp_stanza_t *get_caps(::xmpp::StanzaView request,
                                std::optional<std::string> *hash = nullptr,
                                const char *node = nullptr);
    };
}
