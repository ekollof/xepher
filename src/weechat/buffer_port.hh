// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <memory>
#include <string_view>

struct t_gui_buffer;
struct t_gui_nick;
struct t_gui_nick_group;

namespace weechat {

// WeeChat buffer + nicklist operations behind a testable port.
class BufferPort {
public:
    virtual ~BufferPort() = default;

    [[nodiscard]] virtual struct t_gui_buffer *search(std::string_view plugin,
                                                      std::string_view name) = 0;
    virtual void nicklist_remove_all(struct t_gui_buffer *buffer) = 0;
    virtual void nicklist_remove_nick(struct t_gui_buffer *buffer,
                                      std::string_view nick) = 0;

    [[nodiscard]] virtual struct t_gui_nick_group *nicklist_search_group(
        struct t_gui_buffer *buffer,
        struct t_gui_nick_group *parent,
        std::string_view name) = 0;
    [[nodiscard]] virtual struct t_gui_nick_group *nicklist_add_group(
        struct t_gui_buffer *buffer,
        struct t_gui_nick_group *parent,
        std::string_view name,
        std::string_view color = "weechat.color.nicklist_group",
        int visible = 1) = 0;
    [[nodiscard]] virtual struct t_gui_nick *nicklist_search_nick(
        struct t_gui_buffer *buffer,
        struct t_gui_nick_group *group,
        std::string_view name) = 0;
    virtual void nicklist_add_nick(struct t_gui_buffer *buffer,
                                   struct t_gui_nick_group *group,
                                   std::string_view name,
                                   std::string_view color,
                                   std::string_view prefix,
                                   std::string_view prefix_color,
                                   int visible) = 0;
    virtual void nicklist_nick_set(struct t_gui_buffer *buffer,
                                   struct t_gui_nick *nick,
                                   std::string_view property,
                                   std::string_view value) = 0;

    [[nodiscard]] static std::unique_ptr<BufferPort> default_port();
    [[nodiscard]] static BufferPort &default_port_ref();
};

class WeechatBufferPort final : public BufferPort {
public:
    [[nodiscard]] struct t_gui_buffer *search(std::string_view plugin,
                                              std::string_view name) override;
    void nicklist_remove_all(struct t_gui_buffer *buffer) override;
    void nicklist_remove_nick(struct t_gui_buffer *buffer,
                              std::string_view nick) override;

    [[nodiscard]] struct t_gui_nick_group *nicklist_search_group(
        struct t_gui_buffer *buffer,
        struct t_gui_nick_group *parent,
        std::string_view name) override;
    [[nodiscard]] struct t_gui_nick_group *nicklist_add_group(
        struct t_gui_buffer *buffer,
        struct t_gui_nick_group *parent,
        std::string_view name,
        std::string_view color,
        int visible) override;
    [[nodiscard]] struct t_gui_nick *nicklist_search_nick(
        struct t_gui_buffer *buffer,
        struct t_gui_nick_group *group,
        std::string_view name) override;
    void nicklist_add_nick(struct t_gui_buffer *buffer,
                           struct t_gui_nick_group *group,
                           std::string_view name,
                           std::string_view color,
                           std::string_view prefix,
                           std::string_view prefix_color,
                           int visible) override;
    void nicklist_nick_set(struct t_gui_buffer *buffer,
                           struct t_gui_nick *nick,
                           std::string_view property,
                           std::string_view value) override;
};

}  // namespace weechat