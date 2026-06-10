// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <string>

struct t_gui_buffer;

namespace weechat::nicklist
{
    inline constexpr char k_separator_group[] = "450|────────";
    inline constexpr char k_separator_nick[] = "────────";

    [[nodiscard]] std::string muc_group_name(char prefix, bool online);
    [[nodiscard]] std::string account_group_name(bool online);

    void ensure_muc_groups(struct t_gui_buffer *buffer);
    void ensure_account_groups(struct t_gui_buffer *buffer);

    struct t_gui_nick_group *find_or_add_group(struct t_gui_buffer *buffer,
                                               std::string_view group_name);

    void refresh_separator(struct t_gui_buffer *buffer,
                           bool has_online,
                           bool has_offline);
}