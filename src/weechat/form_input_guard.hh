// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <string>

struct t_gui_buffer;
struct t_hook;

namespace weechat {
// WeeChat modifier adapter: masks private form input and suppresses form history.
class form_input_guard {
public:
    explicit form_input_guard(t_gui_buffer *buffer);
    ~form_input_guard();
    form_input_guard(const form_input_guard &) = delete;
    form_input_guard &operator=(const form_input_guard &) = delete;
    void mask(bool enabled) { masked_ = enabled; }
    [[nodiscard]] bool ready() const { return display_hook_ && history_hook_; }
private:
    static char *modify(const void *, void *, const char *, const char *, const char *);
    std::string buffer_id_;
    bool masked_ = false;
    t_hook *display_hook_ = nullptr;
    t_hook *history_hook_ = nullptr;
};
}
