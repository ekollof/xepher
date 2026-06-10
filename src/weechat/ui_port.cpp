// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "weechat/ui_port.hh"

#include <string>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"

namespace weechat {

void WeechatUiPort::printf(std::string_view msg)
{
    weechat_printf(buffer_, "%s", std::string(msg).c_str());
}

void WeechatUiPort::printf_error(std::string_view msg)
{
    weechat_printf(buffer_, "%s%s", weechat_prefix("error"), std::string(msg).c_str());
}

void WeechatUiPort::printf_info(std::string_view msg)
{
    weechat_printf(buffer_, "%s%s", weechat_prefix("info"), std::string(msg).c_str());
}

void WeechatUiPort::printf_network(std::string_view msg)
{
    weechat_printf(buffer_, "%s%s", weechat_prefix("network"), std::string(msg).c_str());
}

void WeechatUiPort::printf_date_tags(std::time_t date, std::string_view tags, std::string_view msg)
{
    weechat_printf_date_tags(
        buffer_, date, std::string(tags).c_str(), "%s", std::string(msg).c_str());
}

void WeechatUiPort::printf_y(int y, std::string_view msg)
{
    weechat_printf_y(buffer_, y, "%s", std::string(msg).c_str());
}

std::unique_ptr<UiPort> UiPort::for_buffer(struct t_gui_buffer *buffer)
{
    return std::make_unique<WeechatUiPort>(buffer);
}

}  // namespace weechat