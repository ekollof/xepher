// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <memory>
#include <string_view>

struct t_gui_buffer;

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

    [[nodiscard]] static std::unique_ptr<BufferPort> default_port();
};

class WeechatBufferPort final : public BufferPort {
public:
    [[nodiscard]] struct t_gui_buffer *search(std::string_view plugin,
                                            std::string_view name) override;
    void nicklist_remove_all(struct t_gui_buffer *buffer) override;
    void nicklist_remove_nick(struct t_gui_buffer *buffer,
                              std::string_view nick) override;
};

}  // namespace weechat