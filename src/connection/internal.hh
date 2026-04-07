// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// Internal helpers shared between connection.cpp and the separately-compiled
// connection handler translation units (iq_handler.cpp, message_handler.cpp,
// presence_handler.cpp, session_lifecycle.cpp).
//
// Nothing outside src/connection/ should include this header.

#pragma once

#include <optional>
#include <string>
#include <list>
#include <thread>
#include <vector>
#include <stdint.h>

#include <weechat/weechat-plugin.h>
#include <strophe.h>

// Forward declaration
namespace weechat { struct account; }

// ── parse_omemo_device_id ─────────────────────────────────────────────────────
// Parse a decimal string into a valid OMEMO device-ID (1 … 0x7FFFFFFF).
// Returns nullopt on any parse error, overflow, or zero value.
[[nodiscard]] std::optional<std::uint32_t> parse_omemo_device_id(const char *value);

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
    std::string cipher_url;
    std::string filename;
    std::string key_b64;
    std::string iv_b64;
    struct t_gui_buffer *buffer = nullptr;

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
void esfs_start_download(const std::string &cipher_url,
                         const std::string &filename,
                         const std::string &key_b64,
                         const std::string &iv_b64,
                         struct t_gui_buffer *buf);

// ── XEP-0004: Data Forms renderer ─────────────────────────────────────────────
// Render a <x xmlns='jabber:x:data'> form to a WeeChat buffer.
// Defined in message_handler.cpp; called from iq_handler.cpp.
void render_data_form(struct t_gui_buffer *buf,
                      xmpp_stanza_t *x_form,
                      const char *jid,
                      const char *node,
                      const char *session_id);
