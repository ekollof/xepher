// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "runtime_port.hh"

#include <cstdlib>
#include <string>
#include <weechat/weechat-plugin.h>

#include "../plugin.hh"

namespace weechat {

std::string WeechatRuntimePort::version_string()
{
    std::unique_ptr<char, decltype(&free)> version(
        weechat_info_get("version", nullptr), free);
    return version ? std::string(version.get()) : std::string {};
}

const char *WeechatRuntimePort::color(std::string_view name)
{
    return weechat_color(std::string(name).c_str());
}

RuntimePort &RuntimePort::default_runtime()
{
    static WeechatRuntimePort instance;
    return instance;
}

}  // namespace weechat