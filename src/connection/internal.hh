// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// Internal helpers shared between connection.cpp and the separately-compiled
// connection handler translation units (iq_handler.cpp, message_handler.cpp,
// presence_handler.cpp, session_lifecycle.cpp).
//
// Nothing outside src/connection/ should include this header.

#pragma once

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <list>
#include <thread>
#include <vector>
#include <unistd.h>

#include <weechat/weechat-plugin.h>
#include <strophe.h>

#include "connection/strophe_stream_features.hh"

// Forward declarations
namespace weechat {
class account;
class connection;
}

inline constexpr std::time_t k_csi_idle_seconds = 300;

// Wake the WeeChat fd hook after a background worker finishes (best-effort).
inline void signal_worker_pipe(int fd)
{
    if (fd < 0)
        return;
    [[maybe_unused]] const ssize_t nbytes = ::write(fd, "x", 1);
}

// ── parse_omemo_device_id ─────────────────────────────────────────────────────
// Parse a decimal string into a valid OMEMO device-ID (1 … 0x7FFFFFFF).
// Returns unexpected on any parse error, overflow, or zero value.
[[nodiscard]] std::expected<std::uint32_t, std::string> parse_omemo_device_id(std::string_view value);

// XEP-0198 §4: increment handled-stanza counter (wrap UINT32_MAX → 0).
inline void sm_increment_handled_count(std::uint32_t &h) noexcept
{
    if (h == UINT32_MAX)
        h = 0;
    else
        ++h;
}

// Drop acknowledged entries from the outbound SM retransmit queue.
inline void sm_trim_outqueue_through(weechat::account &account,
                                     std::uint32_t ack_h) noexcept
{
    while (!account.sm_outqueue.empty()
           && account.sm_outqueue.front().seq <= ack_h)
        account.sm_outqueue.pop_front();
}

// Drop all SM session state (used when the server rejects or does not advertise SM).
inline void clear_sm_session_state(weechat::account &account) noexcept
{
    account.sm_enabled = false;
    account.sm_id.clear();
    account.sm_h_inbound = 0;
    account.sm_h_outbound = 0;
    account.sm_last_ack = 0;
    account.sm_server_max_seconds = 0;
    account.sm_outqueue.clear();
    account.sm_pending_replay.clear();
}

// True when xepher should send <enable/> or <resume/> on this connect.
[[nodiscard]] inline constexpr bool should_negotiate_sm(bool sm_available,
                                                          bool server_advertises_sm) noexcept
{
    return sm_available && server_advertises_sm;
}

// True when a stream error during SM negotiation means SM must be disabled.
[[nodiscard]] inline constexpr bool sm_stream_error_disables_sm(
    xmpp_error_type_t error_type,
    bool sm_negotiation_active) noexcept
{
    return sm_negotiation_active
        && error_type == XMPP_SE_UNSUPPORTED_STANZA_TYPE;
}

// True when unsupported-stanza-type likely means CSI must be disabled.
[[nodiscard]] inline constexpr bool stream_error_disables_csi(
    xmpp_error_type_t error_type,
    bool sm_negotiation_active,
    bool csi_available) noexcept
{
    return csi_available
        && !sm_negotiation_active
        && error_type == XMPP_SE_UNSUPPORTED_STANZA_TYPE;
}

void sm_start_ack_timer(weechat::account &account);

// Default xmpp-proxy max_stanza_size_bytes is 262144; stay below that.
inline constexpr std::size_t k_proxy_safe_stanza_bytes = 250'000;

[[nodiscard]] inline bool disco_features_contain(
    std::span<const std::string> features,
    std::string_view var) noexcept
{
    return std::ranges::any_of(features, [&](const std::string &f) {
        return f == var;
    });
}

[[nodiscard]] std::optional<std::size_t> stanza_xml_byte_size(xmpp_conn_t *conn,
                                                               xmpp_stanza_t *stanza);

// Send only when the serialized stanza fits within max_bytes (proxy safety).
[[nodiscard]] bool send_within_stanza_byte_limit(weechat::connection &connection,
                                                  xmpp_stanza_t *stanza,
                                                  std::size_t max_bytes,
                                                  std::string_view label);

// ── raw XML trace helpers ─────────────────────────────────────────────────────
// Compute the log-file path for the given account.
[[nodiscard]] std::string raw_xml_trace_path(weechat::account &account);

// Append a stanza to the raw-XML trace log (no-op when logging is disabled).
void append_raw_xml_trace(weechat::account &account,
                           const char *direction,
                           xmpp_stanza_t *stanza);

// ── Ephemeral-message tombstone timer ─────────────────────────────────────────
struct ephemeral_tombstone_ctx {
    struct t_gui_buffer *buffer;
    std::string msg_id;
};

// RAII list that owns all pending ephemeral-tombstone contexts.
extern std::list<ephemeral_tombstone_ctx> g_ephemeral_pending;

// weechat_hook_timer callback — replaces the message text with a tombstone.
int ephemeral_tombstone_cb(const void *pointer, void *data, int remaining_calls);

// ── XEP-0448 Encrypted File Sharing download context ─────────────────────────
struct esfs_download_ctx {
    // Inputs (set before thread starts)
    std::string cipher_url;   // HTTPS ciphertext URL — used for in-flight dedup
    std::string filename;
    std::string key_b64;
    std::string iv_b64;
    struct t_gui_buffer *buffer = nullptr;

    // Dedup keys — passed through to LMDB on success
    weechat::account *account_ptr = nullptr; // non-owning; valid for plugin lifetime
    std::string channel_jid;
    std::string stable_id;    // stanza_id ?? origin_id ?? message id

    // Communication pipe (read-end watched by weechat_hook_fd)
    int pipe_read_fd  = -1;
    int pipe_write_fd = -1;
    struct t_hook *hook = nullptr;

    // Result (written by thread, read by callback)
    bool success = false;
    std::string saved_path;
    std::string error_msg;

    std::thread worker;
};

extern std::list<esfs_download_ctx> g_esfs_downloads;

// Called from message_handler when an encrypted SFS source is detected.
// cipher_url  — the HTTPS URL of the ciphertext (used for in-flight dedup)
// channel_jid — bare JID of the conversation (LMDB dedup key prefix)
// stable_id   — best stable message identifier: stanza_id ?? origin_id ?? msg id
// account_ptr — owning account (for LMDB store on success)
void esfs_start_download(std::string_view cipher_url,
                         std::string_view filename,
                         std::string_view key_b64,
                         std::string_view iv_b64,
                         struct t_gui_buffer *buf,
                         weechat::account *account_ptr,
                         std::string_view channel_jid,
                         std::string_view stable_id);

// Synchronous ESFS download/decrypt for MAM replay (blocks until complete).
[[nodiscard]] std::expected<std::string, std::string>
esfs_download_sync(std::string_view cipher_url,
                   std::string_view filename,
                   std::string_view key_b64,
                   std::string_view iv_b64,
                   weechat::account *account_ptr,
                   std::string_view channel_jid,
                   std::string_view stable_id);

// ── OG / HTML-title async URL preview fetch ───────────────────────────────────
// Fetches a URL in a background thread, parses OpenGraph meta tags (and falls
// back to <title>) from the HTML head, then prints the result as a
// notify_none,no_log,xmpp_og_preview line in the target buffer and stores it
// in the LMDB og_previews cache.
//
// At most OG_MAX_CONCURRENT threads run at once; excess calls are held in
// g_og_pending and drained one-by-one as active fetches complete.
// URLs that are already cached, in-flight, or pending are silently skipped.

static constexpr int OG_MAX_CONCURRENT = 4;

struct og_fetch_ctx {
    // Inputs
    std::string url;
    struct t_gui_buffer *buffer = nullptr;
    weechat::account   *account_ptr = nullptr; // non-owning
    std::string display_prefix;   // left column (nick) for weechat_printf
    time_t      date = 0;         // timestamp for weechat_printf_date_tags
    bool        silent = false;   // cache-only: fetch+store but do not display preview

    // Pipe (read-end watched by weechat_hook_fd)
    int pipe_read_fd  = -1;
    int pipe_write_fd = -1;
    struct t_hook *hook = nullptr;

    // Result (written by thread, read by callback) — mirrors account::og_preview
    bool success = false;
    long http_code = 0;    // HTTP response code (diagnostic)
    int  curl_res  = 0;    // CURLcode cast to int (diagnostic)
    struct preview_t {
        std::string title;
        std::string description;
        std::string url;
        std::string image;
    } preview;

    std::thread worker;
};

// Active (in-flight) fetches — each has a running thread + pipe watched by weechat_hook_fd.
extern std::list<og_fetch_ctx> g_og_fetches;

// Pending fetch descriptors waiting for a slot below OG_MAX_CONCURRENT.
struct og_pending_entry {
    std::string          url;
    struct t_gui_buffer *buffer      = nullptr;
    weechat::account    *account_ptr = nullptr;
    std::string          display_prefix;
    time_t               date        = 0;
    bool                 silent      = false;
};
extern std::list<og_pending_entry> g_og_pending;

// Strip trailing punctuation characters that are commonly appended to URLs
// when they appear inside quoted strings, markdown links, or IRC messages.
// Modifies the string in-place.  Safe to call on already-clean URLs.
void strip_url_trailing_punct(std::string &url);

// Kick off an async OG preview fetch for a URL.
// If OG_MAX_CONCURRENT active fetches are already running the request is
// queued in g_og_pending and drained one-by-one as active fetches complete.
// Silently no-ops if the URL is already cached, in-flight, or pending.
// When silent=true the result is stored to LMDB but not displayed; instead
// a one-line progress note is printed to the account buffer.
void og_start_fetch(std::string_view url,
                     struct t_gui_buffer *buf,
                     weechat::account *account_ptr,
                     std::string_view display_prefix,
                     time_t date,
                     bool silent = false);

// Format an OpenGraph preview as a WeeChat multi-line string with box-drawing chars.
// description, url, image are the OG fields; fallback_url is used when url is empty.
[[nodiscard]] std::string format_og_preview_card(std::string_view title,
                                                   std::string_view description,
                                                   std::string_view url,
                                                   std::string_view image,
                                                   std::string_view fallback_url);

// ── XEP-0004: Data Forms renderer ─────────────────────────────────────────────
// Render a <x xmlns='jabber:x:data'> form to a WeeChat buffer.
// Defined in message_handler.cpp; called from iq_handler.cpp.
void render_data_form(struct t_gui_buffer *buf,
                      xmpp_stanza_t *x_form,
                      const char *jid,
                      const char *node,
                      const char *session_id);
