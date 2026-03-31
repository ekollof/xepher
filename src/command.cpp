// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <strophe.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <iostream>
#include <sstream>
#include <utility>
#include <algorithm>
#include <optional>
#include <memory>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "xmpp/stanza.hh"
#include "xmpp/xep-0054.inl"
#include "xmpp/xep-0292.inl"
#include "account.hh"
#include "user.hh"
#include "avatar.hh"
#include "channel.hh"
#include "buffer.hh"
#include "message.hh"
#include "command.hh"
#include "sexp/driver.hh"
#include "ui/picker.hh"

#define MAM_DEFAULT_DAYS 2
#define STR(X) #X

// ---------------------------------------------------------------------------
// Command implementations — split into logical groups.
// Each .inl file is a self-contained section that shares the includes above.
// ---------------------------------------------------------------------------
#include "command/account.inl"
#include "command/channel.inl"
#include "command/messaging.inl"
#include "command/ephemeral.inl"
#include "command/notify.inl"
#include "command/archive.inl"
#include "command/encryption.inl"
#include "command/history.inl"
#include "command/presence.inl"
#include "command/roster.inl"
#include "command/rooms.inl"
#include "command/muc_admin.inl"

// ---------------------------------------------------------------------------
// command__init — register all WeeChat command hooks.
// ---------------------------------------------------------------------------
void command__init()
{
    auto *hook = weechat_hook_command(
        "account",
        N_("handle xmpp accounts"),
        N_("list"
           " || add <name> <jid> <password>"
           " || register <name> <jid> <password>"
           " || unregister <account>"
           " || password <account> <new-password>"
           " || connect <account>"
           " || disconnect <account>"
           " || reconnect <account>"
           " || delete <account>"),
        N_("         list: list accounts\n"
           "          add: add a xmpp account\n"
           "     register: register a new account via in-band registration (XEP-0077)\n"
           "   unregister: cancel account registration on the server (XEP-0077)\n"
           "     password: change account password on the server (XEP-0077)\n"
           "      connect: connect to a xmpp account\n"
           "   disconnect: disconnect from a xmpp account\n"
           "    reconnect: reconnect an xmpp account\n"
           "       delete: delete a xmpp account\n"),
        "list"
        " || add %(xmpp_account)"
        " || register %(xmpp_account)"
        " || unregister %(xmpp_account)"
        " || password %(xmpp_account)"
        " || connect %(xmpp_account)"
        " || disconnect %(xmpp_account)"
        " || reconnect %(xmpp_account)"
        " || delete %(xmpp_account)",
        &command__account, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /account");

    hook = weechat_hook_command(
        "enter",
        N_("enter an xmpp multi-user-chat (muc)"),
        N_("<jid>"),
        N_("jid: muc to enter"),
        NULL, &command__enter, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /enter");

    // IRC-style alias for /enter
    hook = weechat_hook_command(
        "join",
        N_("join an xmpp multi-user-chat (muc) - IRC alias for /enter"),
        N_("<jid>"),
        N_("jid: muc to join"),
        NULL, &command__enter, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /join");

    hook = weechat_hook_command(
        "open",
        N_("open a direct xmpp chat"),
        N_("<jid>"),
        N_("jid: jid to target, or nick from the current muc"),
        NULL, &command__open, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /open");

    // IRC-style alias for /open
    hook = weechat_hook_command(
        "query",
        N_("open a direct xmpp chat - IRC alias for /open"),
        N_("<jid> [<message>]"),
        N_("   jid: jid to target\nmessage: optional initial message"),
        NULL, &command__open, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /query");

    hook = weechat_hook_command(
        "msg",
        N_("send a xmpp message to the current buffer"),
        N_("<message>"),
        N_("message: message to send"),
        NULL, &command__msg, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /msg");

    hook = weechat_hook_command(
        "me",
        N_("send a xmpp action to the current buffer"),
        N_("<message>"),
        N_("message: message to send"),
        NULL, &command__me, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /me");

    hook = weechat_hook_command(
        "ephemeral",
        N_("send an ephemeral message that disappears after N seconds (XEP-0466)"),
        N_("<seconds> <message>"),
        N_("seconds: time in seconds before the message must be deleted\n"
           "message: message text to send"),
        NULL, &command__ephemeral, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /ephemeral");

    hook = weechat_hook_command(
        "notify",
        N_("get or set per-chat notification preference (XEP-0492)"),
        N_("[<jid>] [always|on-mention|never]"),
        N_("    jid: JID of the chat (defaults to current buffer)\n"
           "  level: 'always' (default), 'on-mention', or 'never'\n"
           "\n"
           "Without arguments, shows the current setting for this buffer.\n"
           "Saves the preference into the XEP-0402 bookmark <extensions>."),
        NULL, &command__notify, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /notify");

    hook = weechat_hook_command(
        "invite",
        N_("invite a user to the current MUC room (XEP-0249)"),
        N_("<jid> [<reason>]"),
        N_("    jid: user to invite\n reason: optional invitation message"),
        NULL, &command__invite, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /invite");

    hook = weechat_hook_command(
        "selfping",
        N_("send a self-ping to verify MUC membership (XEP-0410)"),
        N_(""),
        N_("Send a ping to your own MUC nickname to verify you are still in the room"),
        NULL, &command__selfping, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /selfping");

    hook = weechat_hook_command(
        "mam",
        N_("retrieve MAM messages or manage archiving preferences (XEP-0313/0441)"),
        N_("[days | prefs [get | default <always|never|roster> | always <jid> | never <jid>]]"),
        N_("         days: number of days to fetch for current channel (default: " STR(MAM_DEFAULT_DAYS) ")\n"
           "        prefs: display current MAM archiving preferences\n"
           "  prefs get  : same as prefs\n"
           "  prefs default <always|never|roster>: set the default archiving policy\n"
           "  prefs always <jid>: add <jid> to the always-archive list\n"
           "  prefs never <jid> : add <jid> to the never-archive list"),
        "prefs"
        " || prefs get"
        " || prefs default always"
        " || prefs default never"
        " || prefs default roster"
        " || prefs always %(jabber_jids)"
        " || prefs never %(jabber_jids)",
        &command__mam, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /mam");

    hook = weechat_hook_command(
        "omemo",
        N_("manage omemo encryption for current buffer or account"),
          N_("[check|republish|status|reset-keys|fingerprint [<jid>]|trust <jid>|devices [<jid>]|fetch [<jid>] [<device-id>]|kex [<jid>] [<device-id>]]"),
        N_("       check: query server for published OMEMO devicelist and bundle\n"
           "   republish: republish OMEMO devicelist and bundle to server\n"
           "      status: show OMEMO status and device ID\n"
           "  reset-keys: reset OMEMO key database to force renegotiation\n"
           " fingerprint: show hex fingerprint of own identity key, or of a peer JID\n"
           "       trust: remove stored identity key for <jid> to re-accept on next contact\n"
              "     devices: show known device IDs for <jid>\n"
              "       fetch: force devicelist refresh and optional bundle fetch\n"
              "         kex: force key transport now (or queue after bundle fetch)\n"
           "\n"
           "Without arguments on a channel buffer: enable OMEMO encryption\n"
           "\n"
           "Examples:\n"
           "  /omemo                   : enable OMEMO for current channel\n"
           "  /omemo check             : verify bundle is published on server\n"
           "  /omemo republish         : republish bundle (fixes missing device keys)\n"
           "  /omemo status            : show OMEMO status and device ID\n"
           "  /omemo reset-keys        : reset OMEMO key database\n"
           "  /omemo fingerprint       : show own identity key fingerprint\n"
           "  /omemo fingerprint alice@example.com : show peer fingerprints\n"
           "  /omemo trust alice@example.com       : re-accept changed keys from alice\n"
              "  /omemo devices alice@example.com     : show alice's known devices\n"
              "  /omemo fetch alice@example.com       : force refresh OMEMO metadata\n"
              "  /omemo kex alice@example.com         : force key transport to all known devices\n"
              "  /omemo kex alice@example.com 1234    : force key transport to one device"),
        "check"
        " || republish"
        " || status"
        " || reset-keys"
        " || fingerprint %(jabber_jids)"
        " || trust %(jabber_jids)"
          " || devices %(jabber_jids)"
          " || fetch %(jabber_jids)"
          " || kex %(jabber_jids)", &command__omemo, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /omemo");

    hook = weechat_hook_command(
        "pgp",
        N_("manage PGP encryption for current channel"),
        N_("[<keyid> | status | reset]"),
        N_("keyid: recipient keyid (add key)\n"
           "status: show configured PGP keys\n"
           "reset: remove all configured PGP keys"),
        "status || reset", &command__pgp, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /pgp");

    hook = weechat_hook_command(
        "plain",
        N_("set the current buffer to use no encryption"),
        N_(""),
        N_(""),
        NULL, &command__plain, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /plain");

    hook = weechat_hook_command(
        "block",
        N_("block one or more JIDs"),
        N_("<jid> [<jid>...]"),
        N_("jid: JID to block"),
        NULL, &command__block, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /block");

    hook = weechat_hook_command(
        "unblock",
        N_("unblock one or more JIDs (or all if no JID specified)"),
        N_("[<jid> [<jid>...]]"),
        N_("jid: JID to unblock (omit to unblock all)"),
        NULL, &command__unblock, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /unblock");

    hook = weechat_hook_command(
        "blocklist",
        N_("list all blocked JIDs"),
        N_(""),
        N_(""),
        NULL, &command__blocklist, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /blocklist");

    hook = weechat_hook_command(
        "xml",
        N_("send a raw xml stanza"),
        N_("<stanza>"),
        N_("stanza: xml to send"),
        NULL, &command__xml, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /xml");

    hook = weechat_hook_command(
        "xmpp",
        N_("get xmpp plugin version (see /help for general help)"),
        N_(""),
        N_(""),
        NULL, &command__xmpp, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /xmpp");

    hook = weechat_hook_command(
        "trap",
        N_("debug trap (int3)"),
        N_(""),
        N_(""),
        NULL, &command__trap, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /trap");

    hook = weechat_hook_command(
        "ping",
        N_("send an xmpp ping"),
        N_("[jid]"),
        N_("jid: optional target jid (defaults to current channel or server)"),
        NULL, &command__ping, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /ping");

    hook = weechat_hook_command(
        "mood",
        N_("set or clear your mood (XEP-0107)"),
        N_("[<mood> [text]]"),
        N_("  mood: mood value (happy, sad, angry, excited, tired, etc.)\n"
           "  text: optional descriptive text\n\n"
           "Sets your mood which is published via PEP and visible to contacts.\n"
           "Examples:\n"
           "  /mood happy\n"
           "  /mood excited Working on a cool project!\n"
           "  /mood tired\n"
           "  /mood (clears mood)"),
        NULL, &command__mood, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /mood");

    hook = weechat_hook_command(
        "activity",
        N_("set or clear your activity (XEP-0108)"),
        N_("[<category>[/<specific>] [text]]"),
        N_("category: activity category (working, relaxing, eating, drinking, etc.)\n"
           "specific: optional specific activity (e.g., working/coding)\n"
           "    text: optional descriptive text\n\n"
           "Sets your current activity which is published via PEP.\n"
           "Examples:\n"
           "  /activity working\n"
           "  /activity working/coding\n"
           "  /activity working/coding Implementing XEP-0108\n"
           "  /activity relaxing/reading\n"
           "  /activity eating\n"
           "  /activity (clears activity)\n\n"
           "Categories: doing_chores, drinking, eating, exercising, grooming,\n"
           "            having_appointment, inactive, relaxing, talking, traveling, working"),
        NULL, &command__activity, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /activity");

    hook = weechat_hook_command(
        "edit",
        N_("edit one of your recent messages (XEP-0308)"),
        N_("[text]"),
        N_("text: new message body — corrects the last message you sent immediately.\n\n"
           "Without arguments, opens an interactive picker of the last 20 messages\n"
           "you sent. Select a message with arrows and press Enter.\n"
           "The input bar is pre-filled with /edit-to <id> <original text>.\n"
           "Edit the text and press Enter to send the correction.\n\n"
           "Examples:\n"
           "  /edit                    open picker to choose which message to edit\n"
           "  /edit corrected text     immediately replace your last message"),
        NULL, &command__edit, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /edit");

    hook = weechat_hook_command(
        "edit-to",
        N_("send a message correction by ID (XEP-0308)"),
        N_("<id> <message>"),
        N_("id     : message id to correct (the original, not any prior correction)\n"
           "message: new message text\n\n"
           "This command is used internally by the /edit picker.\n"
           "You can also use it directly if you know the message ID."),
        NULL, &command__edit_to, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /edit-to");

    hook = weechat_hook_command(
        "retract",
        N_("retract (delete) the last message sent (XEP-0424)"),
        N_(""),
        N_("Retracts the last message you sent in the current buffer.\n"
           "This sends a retraction request to all recipients.\n"
           "Note: Recipients may have already seen the message."),
        NULL, &command__retract, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /retract");

    hook = weechat_hook_command(
        "react",
        N_("react to the last message with an emoji (XEP-0444)"),
        N_("<emoji>"),
        N_("emoji: emoji reaction (e.g., 👍 😀 ❤️ 🎉)\n\n"
           "Reacts to the last message (not yours) in the buffer.\n"
           "Examples:\n"
           "  /react 👍\n"
           "  /react ❤️\n"
           "  /react 😂"),
        NULL, &command__react, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /react");

    hook = weechat_hook_command(
        "reply",
        N_("pick a message to reply to (XEP-0461)"),
        N_("[message]"),
        N_("message: your reply text (optional)\n\n"
           "Without arguments: opens an interactive picker of the last 20 messages.\n"
           "Select a message with arrows, press Enter, then type your reply.\n"
           "With arguments: replies immediately to the last non-own message.\n"
           "Examples:\n"
           "  /reply                : open picker\n"
           "  /reply Thanks!\n"
           "  /reply I agree with that"),
        NULL, &command__reply, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /reply");

    hook = weechat_hook_command(
        "reply-to",
        N_("reply to a specific message by ID (XEP-0461)"),
        N_("<id> <message>"),
        N_("id     : message id to reply to\n"
           "message: your reply text\n\n"
           "This command is used internally by the /reply picker.\n"
           "You can also use it directly if you know the message ID."),
        NULL, &command__reply_to, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /reply-to");

    hook = weechat_hook_command(
        "moderate",
        N_("pick a message to moderate in MUC (XEP-0425)"),
        N_("[reason]"),
        N_("reason: optional reason for moderation\n\n"
           "Opens an interactive picker of the last 20 messages in the MUC.\n"
           "Select the message to retract, press Enter to send the moderation request.\n"
           "This command is for MUC moderators to remove messages from other users.\n"
           "Use /retract to delete your own messages.\n"
           "Examples:\n"
           "  /moderate\n"
           "  /moderate Spam message\n"
           "  /moderate Violates community guidelines"),
        NULL, &command__moderate, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /moderate");

    hook = weechat_hook_command(
        "disco",
        N_("discover services and features (XEP-0030)"),
        N_("[items] [jid]"),
        N_("items: list child items (nodes/services) instead of features\n"
           "  jid: optional target JID (defaults to server domain)\n"
           "\n"
           "Examples:\n"
           "  /disco                        - query server for identities and features\n"
           "  /disco conference.example.org - query a specific service\n"
           "  /disco items                  - list items/nodes on server\n"
           "  /disco items pubsub.example.org - list PubSub nodes on a service"),
        "items", &command__disco, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /disco");

    hook = weechat_hook_command(
        "roster",
        N_("manage XMPP roster (contact list)"),
        N_("|| add <jid> [name] || del <jid>"),
        N_("      : display roster\n"
           "  add : add contact to roster\n"
           "  del : remove contact from roster (also: delete, remove)\n"
           "  jid : Jabber ID of the contact\n"
           " name : optional display name for the contact"),
        "add|del|delete|remove", &command__roster, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /roster");
    
    hook = weechat_hook_command(
        "bookmark",
        N_("manage XMPP bookmarks (XEP-0048)"),
        N_("|| add [jid] [name] || del <jid> || autojoin <jid> <on|off>"),
        N_("         : display bookmarks\n"
           "     add : bookmark current MUC or specified JID\n"
           "     del : remove bookmark (also: delete, remove)\n"
           "autojoin : enable/disable autojoin for a bookmark\n"
           "     jid : Jabber ID of the MUC room\n"
           "    name : optional display name for the bookmark\n"
           "\n"
           "Examples:\n"
           "  /bookmark                              : list all bookmarks\n"
           "  /bookmark add                          : bookmark current MUC\n"
           "  /bookmark add room@conference.example.com My Room\n"
           "  /bookmark del room@conference.example.com\n"
           "  /bookmark autojoin room@conference.example.com on"),
        "add|del|delete|remove|autojoin", &command__bookmark, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /bookmark");
    
    hook = weechat_hook_command(
        "list",
        N_("search for public MUC rooms (XEP-0433)"),
        N_("[[<service>] [keywords]]"),
        N_("service  : search service JID (default: api@search.jabber.network)\n"
           "keywords : search keywords (optional)\n"
           "\n"
           "Search for public MUC rooms using XEP-0433 Extended Channel Search.\n"
           "Without keywords, shows popular rooms sorted by number of users.\n"
           "\n"
           "Examples:\n"
           "  /list                            : list popular rooms\n"
           "  /list xmpp                       : search for rooms about XMPP\n"
           "  /list linux gaming               : search for linux gaming rooms\n"
           "  /list api@search.jabber.network xmpp : use specific search service"),
        NULL, &command__list, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /list");

    hook = weechat_hook_command(
        "upload",
        N_("upload a file via HTTP File Upload (XEP-0363)"),
        N_("[<filename>]"),
        N_("filename: path to file to upload (optional)\n\n"
           "If no filename is provided, an interactive file picker will be used.\n"
           "The picker tries GUI dialogs (zenity/kdialog) when X11/Wayland is detected,\n"
           "falls back to fzf in terminal, or prompts for manual entry.\n\n"
           "The file will be uploaded to the server and a URL will be sent in the chat."),
        "%(filename)", &command__upload, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /upload");

    hook = weechat_hook_command(
        "whois",
        N_("get vCard information about an XMPP user (XEP-0054)"),
        N_("[<jid>]"),
        N_("jid: user JID to query (uses current PM channel if not specified)"),
        NULL, &command__whois, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /whois");

    hook = weechat_hook_command(
        "setvcard",
        N_("publish a field of your own vCard (XEP-0054)"),
        N_("<field> <value>"),
        N_("field: vCard field to set (fn, nickname, email, url, desc, org, title, tel, bday, note)\n"
           "value: the value to set for the field\n\n"
           "Publishes a single field of your own vCard via IQ set (XEP-0054).\n"
           "Example: /setvcard fn Alice Smith\n"
           "         /setvcard email alice@example.com\n"
           "Note: only the specified field is sent; other fields are unaffected on\n"
           "      the server only if the server merges — most servers replace the\n"
           "      entire vCard, so run /whois on yourself first to see current values."),
        "fn|nickname|email|url|desc|org|title|tel|bday|note", &command__setvcard, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /setvcard");

    hook = weechat_hook_command(
        "setavatar",
        N_("publish a local image file as your own avatar (XEP-0084)"),
        N_("<filepath>"),
        N_("filepath: path to a PNG, JPEG, GIF, or WEBP image file\n\n"
           "Reads the file, base64-encodes it, and publishes it to your\n"
           "urn:xmpp:avatar:data and urn:xmpp:avatar:metadata PEP nodes.\n"
           "Also updates the <photo> element in subsequent presences (XEP-0153).\n\n"
           "Example:\n"
           "  /setavatar /home/alice/photo.png\n"
           "  /setavatar ~/pictures/avatar.jpg"),
        NULL, &command__setavatar, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /setavatar");

    hook = weechat_hook_command(
        "buzz",
        N_("request a contact's attention (XEP-0224)"),
        N_(""),
        N_("Sends an attention request to the contact in the current PM channel.\n"
           "This is the XMPP equivalent of a 'buzz' or 'nudge'.\n"
           "Note: can only be used in PM channels, not MUC rooms."),
        NULL, &command__buzz, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /buzz");

    hook = weechat_hook_command(
        "spoiler",
        N_("send a spoiler message (XEP-0382)"),
        N_("[<hint:>] <message>"),
        N_("hint   : optional spoiler warning hint (end with ':')\n"
           "message: the spoiler message text\n\n"
           "Sends a message marked as a spoiler. Supporting clients will hide\n"
           "the message body behind a warning.\n\n"
           "Examples:\n"
           "  /spoiler The butler did it.\n"
           "  /spoiler Movie ending: The hero sacrifices himself.\n"
           "  /spoiler TW: violence: The scene is quite graphic."),
        NULL, &command__spoiler, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /spoiler");

    hook = weechat_hook_command(
        "adhoc",
        N_("list and execute XMPP ad-hoc commands (XEP-0050)"),
        N_("<jid>"
           " || <jid> <node>"
           " || <jid> <node> <sessionid> [<field>=<value> ...]"),
        N_("      jid: target JID (server, component, or user)\n"
           "     node: command node URI to execute\n"
           "sessionid: session ID returned by a previous command step\n"
           "field=val: form field values to submit\n\n"
           "Without a node, lists available commands on the target JID.\n"
           "With a node, executes that command (first step).\n"
           "With a sessionid and field=value pairs, submits a form step.\n\n"
           "Examples:\n"
           "  /adhoc conference.example.com\n"
           "  /adhoc example.com http://jabber.org/protocol/admin#get-active-users\n"
           "  /adhoc example.com http://jabber.org/protocol/admin#change-user-password"
           " abc123 username=bob password=newpass"),
        NULL, &command__adhoc, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /adhoc");

    hook = weechat_hook_command(
        "kick",
        N_("kick a user from a MUC room (XEP-0045)"),
        N_("<nick> [<reason>]"),
        N_("  nick: nickname of the user to kick\n"
           "reason: optional reason for the kick\n\n"
           "Requires moderator role in the room.\n\n"
           "Examples:\n"
           "  /kick annoyinguser\n"
           "  /kick spammer Repeatedly posting spam"),
        NULL, &command__kick, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /kick");

    hook = weechat_hook_command(
        "ban",
        N_("ban a user from a MUC room (XEP-0045)"),
        N_("<jid> [<reason>]"),
        N_("   jid: bare JID of the user to ban\n"
           "reason: optional reason for the ban\n\n"
           "Sets the user's affiliation to 'outcast'. Requires admin or owner role.\n\n"
           "Examples:\n"
           "  /ban bad@example.com\n"
           "  /ban troll@example.com Persistent harassment"),
        NULL, &command__ban, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /ban");

    hook = weechat_hook_command(
        "topic",
        N_("set or clear the MUC room topic (XEP-0045)"),
        N_("[<text>]"),
        N_("text: new topic text (omit to clear the topic)\n\n"
           "Examples:\n"
           "  /topic Welcome to #general!\n"
           "  /topic"),
        NULL, &command__topic, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /topic");

    hook = weechat_hook_command(
        "nick",
        N_("change your nickname in the current MUC room (XEP-0045)"),
        N_("[<newnick>]"),
        N_("newnick: new nickname to use (shows current nick if omitted)\n\n"
           "Examples:\n"
           "  /nick mynewnick"),
        NULL, &command__muc_nick, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /nick");

    hook = weechat_hook_command(
        "feed",
        N_("fetch and interact with PubSub feeds (XEP-0060 / XEP-0472 microblogging)"),
         N_("[<service-jid> [--all | <node>] [--limit N] [--before <id>] [--latest]]\n"
           "  discover [--all]\n"
           "  post <service-jid> <node> [--open] [--title <title>] <text>\n"
           "  post [--open] [--title <title>] <text>  (from a feed buffer)\n"
           "  post [--edit]                           (compose in $EDITOR)\n"
           "  post -- <text>        (force short form when body starts with a JID-like word)\n"
           "  reply <service-jid> <node> <item-id|#N> [--open] <text>\n"
           "  reply #N [--open] <text>  (from a feed buffer)\n"
           "  reply #N --edit           (compose reply in $EDITOR)\n"
           "  comments <service-jid> <node> <item-id|#N>\n"
           "  comments #N           (from a feed buffer)\n"
           "  repeat <service-jid> <node> <item-id> [comment]\n"
           "  repeat #N [comment]   (from a feed buffer)\n"
           "  retract <service-jid> <node> <item-id>\n"
           "  retract #N  (from a feed buffer)\n"
           "  subscribe <service-jid> <node>\n"
           "  unsubscribe <service-jid> <node>\n"
           "  subscriptions <service-jid>"),
         N_("service-jid: JID of the PubSub service (e.g. news.movim.eu)\n"
            "      --all: discover all nodes on the service via disco#items\n"
            "       node: node name on the service (e.g. Phoronix)\n"
            "  --limit N: max items to fetch per node (default: 20)\n"
            "--before <id>: fetch the page of items older than item <id> (XEP-0059 RSM)\n"
            "   --latest: clear the saved RSM cursor and fetch the newest page\n"
            "     --open: publish node with access_model=open (public)\n"
            "    --title: set the Atom <title> for a post (omitted if not given; replies have no title)\n"
            "     --edit: open $EDITOR via feed_compose.py instead of typing inline\n"
            "         #N: short alias shown next to each displayed item (e.g. #3)\n"
            "         --: separator to force short form when body starts with a JID-like word\n\n"
           "Without arguments: auto-discovers PubSub services on your server and fetches\n"
           "your subscribed nodes from each one.\n\n"
           "/feed discover: list PubSub services found on your server at connect time.\n"
           "  /feed discover --all  fetches every node from every discovered service.\n\n"
           "After a fetch the feed buffer shows a '/feed ... --before <id>' hint for\n"
           "paging to older entries. Use --latest to go back to the newest page.\n\n"
           "Examples:\n"
           "  /feed                                          (auto-discover and fetch subscriptions)\n"
           "  /feed discover                                 (list known pubsub services)\n"
           "  /feed discover --all                           (fetch all nodes from all services)\n"
           "  /feed news.movim.eu                            (fetch subscribed nodes)\n"
           "  /feed news.movim.eu --all                      (fetch all discovered nodes)\n"
           "  /feed news.movim.eu --all --limit 50           (up to 50 items per node)\n"
           "  /feed news.movim.eu Phoronix                   (fetch one specific node)\n"
           "  /feed news.movim.eu Phoronix --limit 5         (5 items only)\n"
           "  /feed news.movim.eu Phoronix --before abc123   (page back, older items)\n"
           "  /feed news.movim.eu Phoronix --latest          (return to latest page)\n"
            "  /feed post news.movim.eu myblog Hello world    (publish a post, long form)\n"
             "  /feed post Hello world                         (publish from a feed buffer)\n"
             "  /feed post -- hello@world.example great post   (force short form)\n"
             "  /feed post news.movim.eu myblog --open Hello   (publish to a public node)\n"
             "  /feed post --title My Headline -- Body text    (post with Atom <title>)\n"
             "  /feed post --edit                              (open $EDITOR, requires feed-compose.py)\n"
             "  /feed reply news.movim.eu myblog abc123 Nice!  (reply to item abc123)\n"
             "  /feed reply #3 Nice post!                      (reply using alias, from feed buffer)\n"
             "  /feed reply #3 --edit                          (reply in $EDITOR, requires feed-compose.py)\n"
           "  /feed comments news.movim.eu myblog abc123     (fetch comments for item abc123)\n"
           "  /feed comments #3                              (fetch comments using alias)\n"
           "  /feed repeat news.movim.eu myblog abc123       (boost item abc123)\n"
           "  /feed repeat #3 Great post                     (boost using alias, with comment)\n"
           "  /feed retract news.movim.eu myblog abc123      (delete item abc123)\n"
           "  /feed subscribe news.movim.eu Phoronix         (subscribe to node)\n"
           "  /feed unsubscribe news.movim.eu Phoronix       (unsubscribe from node)\n"
           "  /feed subscriptions news.movim.eu              (list subscriptions)"),
        "discover||post||reply||comments||repeat||retract||subscribe||unsubscribe||subscriptions",
        &command__feed, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /feed");

    // Internal hidden command used by picker key bindings.
    // Not listed in /help — the leading space makes WeeChat treat it as internal.
    hook = weechat_hook_command(
        "xmpp-picker-nav",
        N_("navigate the XMPP interactive picker (internal)"),
        N_("up|down|enter|quit"),
        N_("Internal command dispatched by per-buffer key bindings in the picker UI.\n"
           "Not intended for direct user invocation."),
        "up|down|enter|quit",
        &weechat::ui::picker_nav_cb, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /xmpp-picker-nav");
}
