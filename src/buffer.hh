// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

// O(1) buffer→object pointers set at buffer creation (see channel/account).
inline constexpr const char *XMPP_BUFFER_CHANNEL_PTR = "xmpp_channel_pointer";
inline constexpr const char *XMPP_BUFFER_ACCOUNT_PTR = "xmpp_account_pointer";

void buffer__get_account_and_channel(struct t_gui_buffer *buffer,
                                     weechat::account **account,
                                     weechat::channel **channel);

char *buffer__encryption_bar_cb(const void *pointer, void *data,
                                struct t_gui_bar_item *item,
                                struct t_gui_window *window,
                                struct t_gui_buffer *buffer,
                                struct t_hashtable *extra_info);

int buffer__switch_cb(const void *pointer, void *data,
                      const char *signal, const char *type_data,
                      void *signal_data);

int buffer__nickcmp_cb(const void *pointer, void *data,
                       struct t_gui_buffer *buffer,
                       const char *nick1,
                       const char *nick2);

int buffer__close_cb(const void *pointer, void *data,
                      struct t_gui_buffer *buffer);

// Update the message text of a buffer line whose tags include `id_<id_tag>`.
// Returns true if a matching line was found and updated.
bool buffer__update_line_by_id(struct t_gui_buffer *buffer,
                                const char *id_tag,
                                const char *new_message);
