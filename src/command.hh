// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

// Common signature for all WeeChat command callbacks.
#define COMMAND_ARGS \
    const void *pointer, void *data, \
    struct t_gui_buffer *buffer, \
    int argc, char **argv, char **argv_eol

int command__enter(COMMAND_ARGS);
void command__init();

// Declarations for all command__ functions defined in src/command/*.inl.
// These are needed by command__init() in command.cpp.
int command__account(COMMAND_ARGS);
int command__activity(COMMAND_ARGS);
int command__adhoc(COMMAND_ARGS);
int command__ban(COMMAND_ARGS);
int command__block(COMMAND_ARGS);
int command__blocklist(COMMAND_ARGS);
int command__bookmark(COMMAND_ARGS);
int command__buzz(COMMAND_ARGS);
int command__disco(COMMAND_ARGS);
int command__edit(COMMAND_ARGS);
int command__edit_to(COMMAND_ARGS);
int command__ephemeral(COMMAND_ARGS);
int command__feed(COMMAND_ARGS);
int command__invite(COMMAND_ARGS);
int command__kick(COMMAND_ARGS);
int command__list(COMMAND_ARGS);
int command__mam(COMMAND_ARGS);
int command__me(COMMAND_ARGS);
int command__moderate(COMMAND_ARGS);
int command__mood(COMMAND_ARGS);
int command__msg(COMMAND_ARGS);
int command__muc_nick(COMMAND_ARGS);
int command__notify(COMMAND_ARGS);
int command__omemo(COMMAND_ARGS);
int command__open(COMMAND_ARGS);
int command__pgp(COMMAND_ARGS);
int command__ping(COMMAND_ARGS);
int command__plain(COMMAND_ARGS);
int command__react(COMMAND_ARGS);
int command__reply(COMMAND_ARGS);
int command__reply_to(COMMAND_ARGS);
int command__retract(COMMAND_ARGS);
int command__roster(COMMAND_ARGS);
int command__selfping(COMMAND_ARGS);
int command__setavatar(COMMAND_ARGS);
int command__setvcard(COMMAND_ARGS);
int command__spoiler(COMMAND_ARGS);
int command__topic(COMMAND_ARGS);
int command__trap(COMMAND_ARGS);
int command__unblock(COMMAND_ARGS);
int command__upload(COMMAND_ARGS);
int command__whois(COMMAND_ARGS);
int command__xml(COMMAND_ARGS);
int command__xmpp(COMMAND_ARGS);

// Internal picker navigation command — implemented in src/ui/picker.cpp
namespace weechat::ui {
int picker_nav_cb(const void *pointer, void *data,
                  struct t_gui_buffer *buffer,
                  int argc, char **argv, char **argv_eol);
} // namespace weechat::ui
