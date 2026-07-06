// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include <strophe.h>

namespace libstrophe_layout {

// libstrophe 0.12–0.14 common.h (verified at build time on x86_64).
struct conn_interface {
    int (*read)(conn_interface *, void *, size_t);
    int (*write)(conn_interface *, const void *, size_t);
    int (*flush)(conn_interface *);
    int (*pending)(conn_interface *);
    int (*get_error)(conn_interface *);
    int (*error_is_recoverable)(conn_interface *, int);
    xmpp_conn_t *conn;
};

struct password_cache {
    char pass[1024];
    unsigned char fname_hash[XMPP_SHA1_DIGEST_SIZE];
    size_t passlen;
    size_t fnamelen;
};

struct conn_through_sm_state {
    conn_interface intf;
    unsigned int ref;
    xmpp_ctx_t *ctx;
    int type;
    int is_raw;
    int state;
    std::uint64_t timeout_stamp;
    int error;
    xmpp_stream_error_t *stream_error;
    void *xsock;
    int sock;
    int ka_timeout;
    int ka_interval;
    int ka_count;
    void *tls;
    int tls_support;
    int tls_disabled;
    int tls_mandatory;
    int tls_legacy_ssl;
    int tls_trust;
    char *tls_cafile;
    char *tls_capath;
    char *tls_client_cert;
    char *tls_client_key;
    int tls_failed;
    int sasl_support;
    int auth_legacy_enabled;
    int secured;
    xmpp_certfail_handler certfail_handler;
    xmpp_password_callback password_callback;
    void *password_callback_userdata;
    password_cache password_cache_field;
    unsigned int password_retries;
    int bind_required;
    int session_required;
    int sm_disable;
    xmpp_sm_state_t *sm_state;
};

struct sm_state_layout {
    xmpp_ctx_t *ctx;
    int sm_support;
};

}  // namespace libstrophe_layout

enum class stream_ext { sm, csi };

enum class last_top_level_ext { none, sm, csi };

// libstrophe records post-auth <sm/> in sm_state->sm_support during
// _handle_features_sasl. xmpp_conn_get_sm_state() only works while disconnected
// (returns nullptr when CONNECT fires), so read conn->sm_state in place.
[[nodiscard]] inline bool libstrophe_server_advertises_sm(xmpp_conn_t *conn) noexcept
{
    if (!conn)
        return false;
    const auto &layout = *reinterpret_cast<const libstrophe_layout::conn_through_sm_state *>(conn);
    const auto *sm = layout.sm_state;
    if (!sm)
        return false;
    const auto &sm_layout = *reinterpret_cast<const libstrophe_layout::sm_state_layout *>(sm);
    return sm_layout.sm_support != 0;
}

// True when a post-auth <stream:features> chunk advertises XEP-0352 CSI.
[[nodiscard]] inline bool stream_features_xml_advertises_csi(std::string_view xml) noexcept
{
    return xml.contains("urn:xmpp:csi:0")
        && (xml.contains("<csi") || xml.contains(":csi"));
}

struct stream_ext_cache_caps {
    std::optional<bool> sm;
    std::optional<bool> csi;
};

namespace weechat {
class connection;
}

void stream_features_sniff_install(weechat::connection &connection) noexcept;
void stream_features_sniff_restore(weechat::connection &connection) noexcept;
[[nodiscard]] std::optional<bool>
stream_features_sniff_finish(weechat::connection &connection) noexcept;

// Classify unsupported-stanza-type as a recoverable SM/CSI downgrade.
[[nodiscard]] inline std::optional<stream_ext> recoverable_unsupported_stanza_downgrade(
    xmpp_error_type_t error_type,
    bool sm_negotiation_active,
    bool sm_available,
    bool csi_available,
    last_top_level_ext last_sent) noexcept
{
    if (error_type != XMPP_SE_UNSUPPORTED_STANZA_TYPE)
        return std::nullopt;

    if (sm_negotiation_active && sm_available)
        return stream_ext::sm;

    if (!sm_negotiation_active && csi_available)
    {
        if (last_sent == last_top_level_ext::csi)
            return stream_ext::csi;
        if (last_sent == last_top_level_ext::sm)
            return stream_ext::sm;
        return stream_ext::csi;
    }

    return std::nullopt;
}

[[nodiscard]] inline std::string_view stream_ext_downgrade_label(stream_ext ext) noexcept
{
    switch (ext)
    {
    case stream_ext::sm:
        return "Stream Management";
    case stream_ext::csi:
        return "Client State Indication";
    }
    return {};
}

// Merge sniffed features, LMDB cache, and SM heuristic for proactive CSI gating.
[[nodiscard]] inline bool resolve_csi_available(
    bool server_advertises_sm,
    std::optional<bool> sniffed_csi,
    const stream_ext_cache_caps &cached) noexcept
{
    if (cached.csi)
        return *cached.csi;
    if (sniffed_csi)
        return *sniffed_csi;
    // Optimistic probe only when SM is also advertised (typical server bundle).
    return server_advertises_sm;
}