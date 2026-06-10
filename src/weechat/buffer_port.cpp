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

std::unique_ptr<BufferPort> BufferPort::default_port()
{
    return std::make_unique<WeechatBufferPort>();
}

}  // namespace weechat