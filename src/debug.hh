// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <string>
#include <string_view>
#include <weechat/weechat-plugin.h>
#include "fmt/core.h"
#include "weechat/buffer_port.hh"
#include "weechat/runtime_port.hh"
#include "weechat/ui_port.hh"

// Debug buffer singleton — created lazily on first XDEBUG() call.
// All XDEBUG() calls write here when debug mode is on.
namespace weechat::debug
{
    // The xmpp.debug buffer pointer.  nullptr until first use.
    inline struct t_gui_buffer *buffer = nullptr;

    // Ensure the debug buffer exists and return it.
    // Created lazily to avoid racing with WeeChat's layout restoration,
    // which can reassign buffer names/short-names at plugin load time.
    inline struct t_gui_buffer *get_or_create()
    {
        if (buffer)
            return buffer;
        auto &bp = BufferPort::default_port_ref();
        // Reuse an existing buffer if WeeChat already has one (e.g. layout restore).
        buffer = bp.search("xmpp", "debug");
        if (!buffer)
            buffer = bp.create("debug",
                               nullptr, nullptr, nullptr,
                               nullptr, nullptr, nullptr);
        if (!buffer)
            return nullptr;
        bp.set(buffer, "short_name", "xmpp-debug");
        bp.set(buffer, "title", "xmpp debug log");
        bp.set(buffer, "localvar_set_type", "debug");
        bp.set(buffer, "notify", "0");  // no highlights/alerts
        return buffer;
    }

    // Destroy the debug buffer.  Called from plugin::end().
    inline void fini()
    {
        if (buffer)
        {
            BufferPort::default_port_ref().close(buffer);
            buffer = nullptr;
        }
    }

    // Print one line to the debug buffer.
    // file/line are passed automatically via the XDEBUG macro.
    inline void print(const char *file, int line, std::string msg)
    {
        struct t_gui_buffer *buf = get_or_create();
        if (!buf)
            return;
        // Strip leading path components for readability
        std::string_view path{file ? file : ""};
        std::string_view base = path;
        if (auto pos = path.rfind('/'); pos != std::string_view::npos)
            base = path.substr(pos + 1);
        if (auto ui = weechat::UiPort::for_buffer(buf))
        {
            auto &rt = RuntimePort::default_runtime();
            ui->printf(fmt::format("{}[{}:{}]{} {}",
                                   rt.color("darkgray"),
                                   base, line,
                                   rt.color("reset"),
                                   msg));
        }
    }
} // namespace weechat::debug

// xmpp_debug_is_on() — defined in config.cpp; returns true when
// xmpp.look.debug option is enabled.
bool xmpp_debug_is_on();

// xmpp_raw_xml_log_is_on() — defined in config.cpp; returns true when
// xmpp.look.raw_xml_log option is enabled.
bool xmpp_raw_xml_log_is_on();

// XDEBUG(fmt, ...) — print to the debug buffer when debug mode is on.
// Usage: XDEBUG("PEP event from {}: {}", from, node);
#define XDEBUG(fmt_str, ...) \
    do { \
        if (xmpp_debug_is_on()) \
            weechat::debug::print(__FILE__, __LINE__, \
                ::fmt::format(fmt_str __VA_OPT__(,) __VA_ARGS__)); \
    } while (0)
