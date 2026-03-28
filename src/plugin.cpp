// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// Suppress IntelliSense false positives for WeeChat opaque types
// The compiler (GCC) correctly resolves these, but IntelliSense may not
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wincompatible-pointer-types"
#endif

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <csignal>
#include <exception>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "config.hh"
#include "account.hh"
#include "channel.hh"
#include "user.hh"
#include "connection.hh"
#include "command.hh"
#include "input.hh"
#include "buffer.hh"
#include "completion.hh"
#include "debug.hh"

#define WEECHAT_TIMER_INTERVAL_SEC 0.01
#define WEECHAT_TIMER_SECONDS(IVL) (WEECHAT_TIMER_INTERVAL_SEC * IVL)

#pragma GCC visibility push(default)
extern "C" {
WEECHAT_PLUGIN_NAME(WEECHAT_XMPP_PLUGIN_NAME);
WEECHAT_PLUGIN_DESCRIPTION(N_("XMPP client protocol"));
WEECHAT_PLUGIN_AUTHOR("bqv <weechat@fron.io>");
WEECHAT_PLUGIN_VERSION(WEECHAT_XMPP_PLUGIN_VERSION);
WEECHAT_PLUGIN_LICENSE("MPL2");
WEECHAT_PLUGIN_PRIORITY(5500);
}

void (* weechat_signal_handler)(int);

// Callback invoked when weechat.color.chat_nick_colors or
// weechat.look.nick_color_* changes. Updates all nicklist entry colors
// in-place (no remove+re-add) so the palette takes effect immediately.
// Also walks existing buffer lines and updates their prefix color so that
// already-printed messages pick up the new palette without a restart.
extern "C"
int nick_color_config_cb(const void *, void *, const char *, const char *)
{
    // ── 1. Update nicklist colors ────────────────────────────────────────────
    for (auto& [aname, account] : weechat::accounts)
    {
        for (auto& [cid, channel] : account.channels)
        {
            if (!channel.buffer)
                continue;
            for (auto& [uid, user] : account.users)
            {
                // Match users to this channel by comparing the bare JID of the
                // user's map key (full JID, e.g. "room@conf/nick") against the
                // channel id (bare JID, e.g. "room@conf").  Using display_name
                // was wrong for MUC users whose display_name is just a nickname.
                xmpp_string_guard bare_g(account.context,
                    xmpp_jid_bare(account.context, uid.c_str()));
                if (bare_g &&
                    weechat_strcasecmp(bare_g.c_str(), channel.id.data()) == 0)
                {
                    user.nicklist_set_color(&account, &channel);
                }
            }
        }
    }

    // ── 2. Update prefix colors on already-printed chat lines ────────────────
    // Walk every line of every XMPP channel buffer, look for nick_XXXX tags,
    // and rebuild the prefix with the freshly-computed color so the on-screen
    // text immediately reflects the new palette.
    struct t_hdata *hdata_buffer   = weechat_hdata_get("buffer");
    struct t_hdata *hdata_lines    = weechat_hdata_get("lines");
    struct t_hdata *hdata_line     = weechat_hdata_get("line");
    struct t_hdata *hdata_ldata    = weechat_hdata_get("line_data");

    if (!hdata_buffer || !hdata_lines || !hdata_line || !hdata_ldata)
        return WEECHAT_RC_OK;

    for (auto& [aname, account] : weechat::accounts)
    {
        for (auto& [cid, channel] : account.channels)
        {
            if (!channel.buffer)
                continue;

            void *lines = weechat_hdata_pointer(hdata_buffer, channel.buffer, "lines");
            if (!lines)
                continue;

            void *line = weechat_hdata_pointer(hdata_lines, lines, "last_line");
            while (line)
            {
                void *ldata = weechat_hdata_pointer(hdata_line, line, "data");
                if (!ldata)
                {
                    line = weechat_hdata_move(hdata_line, line, -1);
                    continue;
                }

                int tags_count = weechat_hdata_integer(hdata_ldata, ldata, "tags_count");
                const char *nick_name = nullptr;
                for (int i = 0; i < tags_count; ++i)
                {
                    // tags_array is a char** — index via "NNN|tags_array"
                    char index_key[32];
                    snprintf(index_key, sizeof(index_key), "%d|tags_array", i);
                    const char *tag = weechat_hdata_string(hdata_ldata, ldata, index_key);
                    if (tag && strncmp(tag, "nick_", 5) == 0 && tag[5] != '\0')
                    {
                        nick_name = tag + 5;
                        break;
                    }
                }

                if (nick_name)
                {
                    // Get the current prefix so we can detect avatar prefix or
                    // other leading content before the tab separator.
                    const char *cur_prefix = weechat_hdata_string(hdata_ldata, ldata, "prefix");

                    // Rebuild prefix: {new_color}{nick}\t
                    // (same as weechat::user::as_prefix(nick_name))
                    std::string new_color = weechat_info_get("nick_color", nick_name);
                    std::string new_prefix = new_color + nick_name + "\t";

                    // Only update if it actually changed (avoid unnecessary redraws).
                    if (!cur_prefix || new_prefix != cur_prefix)
                    {
                        struct t_hashtable *update = weechat_hashtable_new(
                            4,
                            WEECHAT_HASHTABLE_STRING,
                            WEECHAT_HASHTABLE_STRING,
                            nullptr, nullptr);
                        if (update)
                        {
                            weechat_hashtable_set(update, "prefix", new_prefix.c_str());
                            weechat_hdata_update(hdata_ldata, ldata, update);
                            weechat_hashtable_free(update);
                        }
                    }
                }

                line = weechat_hdata_move(hdata_line, line, -1);
            }
        }
    }

    return WEECHAT_RC_OK;
}

extern "C"
void wrapped_signal_handler(int arg)
{ // wrap weechat's handler
    weechat_signal_handler(arg);
    __builtin_trap();
}

extern "C"
int weechat_plugin_init(struct t_weechat_plugin *plugin, int argc, char *argv[])
{
    try {
        weechat::plugin::instance = std::make_unique<weechat::plugin>(plugin);
        weechat::plugin::instance->init(argc, argv);
    }
    catch (std::exception const& ex) {
        return WEECHAT_RC_ERROR;
    }

    return WEECHAT_RC_OK;
}

extern "C"
int weechat_plugin_end(struct t_weechat_plugin *plugin)
{
    try {
        if (plugin != *weechat::plugin::instance)
            throw std::runtime_error("wrong plugin?");
        weechat::plugin::instance->end();
        weechat::plugin::instance.reset();
    }
    catch (std::exception const& ex) {
        return WEECHAT_RC_ERROR;
    }

    return WEECHAT_RC_OK;
}

std::unique_ptr<weechat::plugin> weechat::plugin::instance;

weechat::plugin::plugin(struct t_weechat_plugin *plugin)
    : m_plugin_ptr(plugin)
    , m_process_timer(nullptr)
    , m_encryption_bar_item(nullptr)
    , m_buffer_switch_hook(nullptr)
    , m_input_text_changed_hook(nullptr)
    , m_nick_color_config_hook(nullptr)
    , m_nick_color_look_hook(nullptr)
{
}

void weechat::plugin::init(int argc, char *argv[])
{
    // Reset unloading flag
    weechat::g_plugin_unloading = false;
    
    m_args = std::vector<std::string_view>(argv, argv+argc);

    // Signal handler wrapping disabled - causes crashes when not under debugger
    // if (std::find(m_args.begin(), m_args.end(), "debug") != m_args.end())
    //     weechat_signal_handler = std::signal(SIGSEGV, wrapped_signal_handler);

    if (!weechat::config::init()) // TODO: bool -> exceptions
        throw std::runtime_error("Config init failed");

    weechat::config::read();

    weechat::debug::init();

    weechat::connection::init();

    command__init(); // TODO: port

    completion__init(); // TODO: port

    m_process_timer = weechat_hook_timer(WEECHAT_TIMER_SECONDS(1000), 0, 0,
                                         &weechat::account::timer_cb,
                                         nullptr, nullptr);

    m_encryption_bar_item = weechat_bar_item_new(encryption_bar_item_name.data(),
                                                  &buffer__encryption_bar_cb,
                                                  nullptr, nullptr);

    m_buffer_switch_hook = weechat_hook_signal("buffer_switch",
                                                &buffer__switch_cb,
                                                nullptr, nullptr);

    m_input_text_changed_hook = weechat_hook_signal("input_text_changed",
                                                      &input__text_changed_cb, // TODO: port
                                                      nullptr, nullptr);

    m_nick_color_config_hook = weechat_hook_config("weechat.color.chat_nick_colors",
                                                    &nick_color_config_cb,
                                                    nullptr, nullptr);

    m_nick_color_look_hook = weechat_hook_config("weechat.look.nick_color_*",
                                                  &nick_color_config_cb,
                                                  nullptr, nullptr);

    // Smart filter: auto-register a WeeChat filter to hide join/leave/nick-change
    // lines tagged with xmpp_smart_filter.  Users can toggle it with
    //   /filter enable|disable xmpp_smart_filter_default
    // or remove it with /filter del xmpp_smart_filter_default
    weechat_command(NULL,
                    "/filter add xmpp_smart_filter_default * xmpp_smart_filter *");
}

void weechat::plugin::end() {
    // CRITICAL: Set flag FIRST to stop any in-flight callbacks
    // If we unhook first, a callback could start before we set the flag
    weechat::g_plugin_unloading = true;
    
    // Unhook timer to stop new callbacks from being scheduled
    if (m_process_timer) {
        weechat_unhook(m_process_timer);
        m_process_timer = nullptr;
    }
    
    // Unhook signals to prevent callbacks during shutdown
    if (m_buffer_switch_hook) {
        weechat_unhook(m_buffer_switch_hook);
        m_buffer_switch_hook = nullptr;
    }
    
    if (m_input_text_changed_hook) {
        weechat_unhook(m_input_text_changed_hook);
        m_input_text_changed_hook = nullptr;
    }

    if (m_nick_color_config_hook) {
        weechat_unhook(m_nick_color_config_hook);
        m_nick_color_config_hook = nullptr;
    }

    if (m_nick_color_look_hook) {
        weechat_unhook(m_nick_color_look_hook);
        m_nick_color_look_hook = nullptr;
    }

    if (m_encryption_bar_item)
        weechat_bar_item_remove(m_encryption_bar_item);

    weechat::account::disconnect_all();

    // Write config before clearing accounts
    weechat::config::write();

    weechat::debug::fini();
    
    // Do not clear accounts during shutdown.
    // account/omemo teardown can still race libsignal/libstrophe object lifetime
    // during process exit and has historically caused ref-count assertions.
    // The global account map is intentionally never freed and the OS reclaims
    // memory at process end.

    weechat::config::instance.reset();

    libstrophe::shutdown();
}

weechat::plugin::~plugin()
{
    // Note: instance is being destroyed here, so we can't check it
    // This destructor should be empty anyway
}

#ifdef __clang__
#pragma clang diagnostic pop
#endif

