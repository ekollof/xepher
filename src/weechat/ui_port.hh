// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <ctime>
#include <memory>
#include <string_view>

struct t_gui_buffer;

namespace weechat {

// Abstract WeeChat output surface for commands, OMEMO feedback, etc.
class UiPort {
public:
    virtual ~UiPort() = default;

    virtual void printf(std::string_view msg) = 0;
    virtual void printf_error(std::string_view msg) = 0;
    virtual void printf_network(std::string_view msg) = 0;
    virtual void printf_date_tags(std::time_t date, std::string_view tags, std::string_view msg) = 0;

    [[nodiscard]] static std::unique_ptr<UiPort> for_buffer(struct t_gui_buffer *buffer);
};

class WeechatUiPort final : public UiPort {
public:
    explicit WeechatUiPort(struct t_gui_buffer *buffer) : buffer_(buffer) {}

    void printf(std::string_view msg) override;
    void printf_error(std::string_view msg) override;
    void printf_network(std::string_view msg) override;
    void printf_date_tags(std::time_t date, std::string_view tags, std::string_view msg) override;

private:
    struct t_gui_buffer *buffer_;
};

}  // namespace weechat