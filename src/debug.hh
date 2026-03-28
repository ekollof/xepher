// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <string>
#include <weechat/weechat-plugin.h>
#include "fmt/core.h"
#include "plugin.hh"

// Debug buffer singleton — created on plugin load, destroyed on unload.
// All XDEBUG() calls write here when debug mode is on.
namespace weechat::debug
{
    // The xmpp.debug buffer pointer.  nullptr when not yet created.
    inline struct t_gui_buffer *buffer = nullptr;

    // Create the debug buffer.  Called from plugin::init().
    inline void init()
    {
        if (buffer)
            return;
        buffer = weechat_buffer_new("debug",
                                    nullptr, nullptr, nullptr,
                                    nullptr, nullptr, nullptr);
        if (!buffer)
            return;
        weechat_buffer_set(buffer, "short_name", "xmpp.debug");
        weechat_buffer_set(buffer, "title", "xmpp debug log");
        weechat_buffer_set(buffer, "localvar_set_type", "debug");
        weechat_buffer_set(buffer, "notify", "0");  // no highlights/alerts
    }

    // Destroy the debug buffer.  Called from plugin::end().
    inline void fini()
    {
        if (buffer)
        {
            weechat_buffer_close(buffer);
            buffer = nullptr;
        }
    }

    // Print one line to the debug buffer.
    // file/line are passed automatically via the XDEBUG macro.
    inline void print(const char *file, int line, std::string msg)
    {
        if (!buffer)
            return;
        // Strip leading path components for readability
        const char *base = file;
        for (const char *p = file; *p; ++p)
            if (*p == '/')
                base = p + 1;
        weechat_printf(buffer, "%s[%s:%d]%s %s",
                       weechat_color("darkgray"),
                       base, line,
                       weechat_color("reset"),
                       msg.c_str());
    }
} // namespace weechat::debug

// xmpp_debug_is_on() — defined in config.cpp; returns true when
// xmpp.look.debug option is enabled.
bool xmpp_debug_is_on();

// XDEBUG(fmt, ...) — print to the debug buffer when debug mode is on.
// Usage: XDEBUG("PEP event from {}: {}", from, node);
#define XDEBUG(fmt_str, ...) \
    do { \
        if (xmpp_debug_is_on()) \
            weechat::debug::print(__FILE__, __LINE__, \
                ::fmt::format(fmt_str __VA_OPT__(,) __VA_ARGS__)); \
    } while (0)
