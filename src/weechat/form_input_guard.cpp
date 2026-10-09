// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "weechat/form_input_guard.hh"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>
#include <fmt/core.h>
#include <weechat/weechat-plugin.h>
#include "plugin.hh"

namespace weechat {
form_input_guard::form_input_guard(t_gui_buffer *buffer)
    : buffer_id_(fmt::format("{:p}", static_cast<void *>(buffer)))
{
    display_hook_ = weechat_hook_modifier("input_text_display", modify, this, nullptr);
    history_hook_ = weechat_hook_modifier("history_add", modify, this, nullptr);
}

form_input_guard::~form_input_guard()
{
    if (display_hook_) weechat_unhook(display_hook_);
    if (history_hook_) weechat_unhook(history_hook_);
}

char *form_input_guard::modify(const void *pointer, void *, const char *modifier,
                              const char *data, const char *input)
{
    const auto &guard = *static_cast<const form_input_guard *>(pointer);
    std::string value(input ? input : "");
    if (data && guard.buffer_id_ == data) {
        if (std::string_view(modifier) == "history_add") value.clear();
        else if (guard.masked_) value.assign(value.size(), '*');
    }
    // The modifier C ABI transfers a malloc-compatible string to WeeChat.
    std::unique_ptr<char, decltype(&std::free)> result(::strdup(value.c_str()), std::free);
    return result.release();
}
}
