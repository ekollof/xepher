// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include "xmpp/data_form.hh"
#include "weechat/form_input_guard.hh"
#include "test_export.hh"

struct t_gui_buffer;

namespace weechat::ui {
using form_submit_callback = std::function<std::expected<void, std::string>(
    std::string_view action, const ::xmpp::data_form &form)>;

// Owned by the editor registry until its buffer closes. Callbacks never own accounts.
class XMPP_TEST_EXPORT form_editor {
public:
    static auto open(std::string_view owner, std::string_view id, std::string_view title,
        ::xmpp::data_form form, std::vector<std::string> actions, std::string_view default_action,
        form_submit_callback submit, std::function<void()> closed = {}) -> std::expected<void, std::string>;
    static void close(std::string_view id);
    static void close_owner(std::string_view owner);
    static void close_all();
    static void failed(std::string_view id, std::string_view reason);
private:
    static int input_cb(const void *, void *, t_gui_buffer *, const char *);
    static int close_cb(const void *, void *, t_gui_buffer *);
    void render();
    void advance();
    void input(std::string_view value);
    void error(std::string_view message);
    std::string owner_;
    std::string id_;
    ::xmpp::data_form form_;
    std::vector<std::string> actions_;
    std::string default_action_;
    form_submit_callback submit_;
    std::function<void()> closed_;
    t_gui_buffer *buffer_ = nullptr;
    std::unique_ptr<form_input_guard> guard_;
    std::optional<std::size_t> current_;
    bool waiting_ = false;
};

[[nodiscard]] std::string adhoc_form_id(std::string_view owner, std::string_view target,
                                       std::string_view node, std::string_view session);
}
