// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "weechat/buffer_port.hh"

#include <string>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"

namespace weechat {

struct t_gui_buffer *WeechatBufferPort::search(
    const std::string_view plugin,
    const std::string_view name)
{
    struct t_gui_buffer *const buffer =
        weechat_buffer_search(std::string(plugin).c_str(), std::string(name).c_str());
    if (!buffer)
        return nullptr;
    if (weechat_buffer_get_pointer(buffer, "plugin") != weechat_plugin)
        return nullptr;
    return buffer;
}

void WeechatBufferPort::nicklist_remove_all(struct t_gui_buffer *const buffer)
{
    if (buffer)
        weechat_nicklist_remove_all(buffer);
}

void WeechatBufferPort::nicklist_remove_nick(
    struct t_gui_buffer *const buffer,
    const std::string_view nick)
{
    if (!buffer || nick.empty())
        return;
    if (struct t_gui_nick *const ptr_nick = weechat_nicklist_search_nick(
            buffer, nullptr, std::string(nick).c_str()))
        weechat_nicklist_remove_nick(buffer, ptr_nick);
}

struct t_gui_nick_group *WeechatBufferPort::nicklist_search_group(
    struct t_gui_buffer *const buffer,
    struct t_gui_nick_group *const parent,
    const std::string_view name)
{
    if (!buffer || name.empty())
        return nullptr;
    return weechat_nicklist_search_group(
        buffer, parent, std::string(name).c_str());
}

struct t_gui_nick_group *WeechatBufferPort::nicklist_add_group(
    struct t_gui_buffer *const buffer,
    struct t_gui_nick_group *const parent,
    const std::string_view name,
    const std::string_view color,
    const int visible)
{
    if (!buffer || name.empty())
        return nullptr;
    return weechat_nicklist_add_group(
        buffer,
        parent,
        std::string(name).c_str(),
        std::string(color).c_str(),
        visible);
}

struct t_gui_nick *WeechatBufferPort::nicklist_search_nick(
    struct t_gui_buffer *const buffer,
    struct t_gui_nick_group *const group,
    const std::string_view name)
{
    if (!buffer || name.empty())
        return nullptr;
    return weechat_nicklist_search_nick(buffer, group, std::string(name).c_str());
}

void WeechatBufferPort::nicklist_add_nick(
    struct t_gui_buffer *const buffer,
    struct t_gui_nick_group *const group,
    const std::string_view name,
    const std::string_view color,
    const std::string_view prefix,
    const std::string_view prefix_color,
    const int visible)
{
    if (!buffer || !group || name.empty())
        return;
    weechat_nicklist_add_nick(
        buffer,
        group,
        std::string(name).c_str(),
        std::string(color).c_str(),
        std::string(prefix).c_str(),
        std::string(prefix_color).c_str(),
        visible);
}

void WeechatBufferPort::nicklist_nick_set(
    struct t_gui_buffer *const buffer,
    struct t_gui_nick *const nick,
    const std::string_view property,
    const std::string_view value)
{
    if (!buffer || !nick || property.empty())
        return;
    weechat_nicklist_nick_set(
        buffer,
        nick,
        std::string(property).c_str(),
        std::string(value).c_str());
}

std::string WeechatBufferPort::get_string(
    struct t_gui_buffer *const buffer,
    const std::string_view property)
{
    if (!buffer || property.empty())
        return {};
    if (const char *value = weechat_buffer_get_string(buffer, std::string(property).c_str()))
        return value;
    return {};
}

std::unique_ptr<BufferPort> BufferPort::default_port()
{
    return std::make_unique<WeechatBufferPort>();
}

BufferPort &BufferPort::default_port_ref()
{
    static const std::unique_ptr<BufferPort> port = default_port();
    return *port;
}

}  // namespace weechat