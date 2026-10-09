// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "ui/form_editor.hh"

#include <algorithm>
#include <charconv>
#include <map>
#include <ranges>
#include <span>
#include <fmt/core.h>
#include <weechat/weechat-plugin.h>
#include "xmpp/iq_adhoc.hh"
#include "weechat/buffer_port.hh"
#include "weechat/ui_port.hh"
#include "weechat/runtime_port.hh"

namespace weechat::ui {
namespace {
std::map<std::string, std::unique_ptr<form_editor>> editors;

auto number(std::string_view input) -> std::expected<std::size_t, std::string>
{
    std::size_t result = 0;
    const auto [end, ec] = std::from_chars(input.data(), input.data() + input.size(), result);
    if (ec != std::errc{} || end != input.data() + input.size() || result == 0)
        return std::unexpected("Enter a positive field or option number");
    return result;
}
}

std::string adhoc_form_id(std::string_view owner, std::string_view target,
                         std::string_view node, std::string_view session)
{
    return fmt::format("{}:{}{}:{}{}:{}{}:{}", owner.size(), owner, target.size(), target,
                       node.size(), node, session.size(), session);
}

auto form_editor::open(std::string_view owner, std::string_view id, std::string_view title,
    ::xmpp::data_form form, std::vector<std::string> actions, std::string_view default_action,
    form_submit_callback submit) -> std::expected<void, std::string>
{
    const auto version = RuntimePort::default_runtime().version_string();
    const auto dot = version.find('.');
    const auto major = number(std::string_view(version).substr(0, dot));
    unsigned minor = 0;
    if (dot != std::string::npos)
        (void)std::from_chars(version.data() + dot + 1, version.data() + version.size(), minor);
    if (!major || *major < 4 || (*major == 4 && minor < 3))
        return std::unexpected("Interactive forms require WeeChat 4.3 or newer; use /adhoc field=value submission");
    close(id);
    auto editor = std::make_unique<form_editor>();
    editor->owner_ = owner;
    editor->id_ = id;
    editor->form_ = std::move(form);
    editor->actions_ = std::move(actions);
    editor->default_action_ = default_action;
    editor->submit_ = std::move(submit);
    auto &bp = BufferPort::default_port_ref();
    editor->buffer_ = bp.create(fmt::format("xmpp.form.{}", id), input_cb, editor.get(), nullptr,
                                close_cb, editor.get(), nullptr);
    if (!editor->buffer_) return std::unexpected("Cannot create form buffer");
    bp.set(editor->buffer_, "type", "free");
    bp.set(editor->buffer_, "title", title);
    bp.set(editor->buffer_, "localvar_set_no_log", "1");
    bp.set(editor->buffer_, "input_get_empty", "1");
    bp.set(editor->buffer_, "input_multiline", "1");
    bp.set(editor->buffer_, "input_get_any_user_data", "1");
    editor->guard_ = std::make_unique<form_input_guard>(editor->buffer_);
    if (!editor->guard_->ready()) {
        bp.set_pointer(editor->buffer_, "close_callback", nullptr);
        bp.close(editor->buffer_);
        return std::unexpected("Cannot protect private form input");
    }
    editor->advance();
    bp.set(editor->buffer_, "display", "1");
    editors.emplace(id, std::move(editor));
    return {};
}

void form_editor::close(std::string_view id)
{
    if (const auto it = editors.find(std::string(id)); it != editors.end())
        BufferPort::default_port_ref().close(it->second->buffer_);
}

void form_editor::close_owner(std::string_view owner)
{
    const auto ids = editors | std::views::filter([&](const auto &entry) {
        return entry.second->owner_ == owner;
    }) | std::views::keys | std::ranges::to<std::vector>();
    std::ranges::for_each(ids, [](std::string_view id) { close(id); });
}

void form_editor::close_all()
{
    while (!editors.empty()) close(editors.begin()->first);
}

int form_editor::close_cb(const void *pointer, void *, t_gui_buffer *)
{
    const auto *editor = static_cast<const form_editor *>(pointer);
    const auto id = editor->id_;
    editors.erase(id);
    return WEECHAT_RC_OK;
}

int form_editor::input_cb(const void *pointer, void *, t_gui_buffer *, const char *input)
{
    auto *editor = const_cast<form_editor *>(static_cast<const form_editor *>(pointer));
    const std::string value(input ? input : "");
    BufferPort::default_port_ref().set(editor->buffer_, "input", "");
    if (value == "/close" || value == "/buffer close") close(editor->id_);
    else editor->input(value);
    return WEECHAT_RC_OK;
}

void form_editor::error(std::string_view message)
{
    UiPort::for_buffer(buffer_)->printf_y(0, fmt::format("Error: {}", message));
}

void form_editor::failed(std::string_view id, std::string_view reason)
{
    if (const auto it = editors.find(std::string(id)); it != editors.end()) {
        it->second->waiting_ = false;
        it->second->render();
        it->second->error(reason);
    }
}

void form_editor::advance()
{
    const auto begin = current_ ? *current_ + 1 : 0;
    current_.reset();
    for (const auto [index, field] : form_.fields | std::views::enumerate | std::views::drop(begin)) {
        if (field.editable()) { current_ = index; break; }
    }
    render();
}

void form_editor::render()
{
    auto &bp = BufferPort::default_port_ref();
    bp.clear(buffer_);
    auto ui = UiPort::for_buffer(buffer_);
    int line = 1;
    const auto write = [&](std::string_view text) { ui->printf_y(line++, text); };
    std::ranges::for_each(::xmpp::data_form_lines(form_), write);
    write(" ");
    guard_->mask(current_ && form_.fields[*current_].type == "text-private");
    if (waiting_) { write("Waiting for the server…"); return; }
    if (current_) {
        const auto &field = form_.fields[*current_];
        write(fmt::format("Editing {}. {}", *current_ + 1, field.label.empty() ? field.var : field.label));
        if (!field.description.empty()) write(field.description);
        std::ranges::for_each(field.options | std::views::enumerate, [&](const auto entry) {
            const auto &[index, option] = entry;
            write(fmt::format("  {}: {}", index + 1, option.label.empty() ? option.value : option.label));
        });
        if (field.type.starts_with("list-")) write("Enter option number(s), separated by spaces.");
        else if (field.type == "boolean") write("Enter true or false (1 or 0 also accepted).");
        else if (field.type.ends_with("-multi")) write("Enter quoted values separated by spaces; pasted text lines are separate values.");
        else write("Enter a value. Prefix a literal colon with another colon.");
        write("Enter keeps the current value. :clear unsets; :empty sets empty text; :omit leaves an optional field unchanged.");
    } else write("Review your answers, then type :submit. Nothing is sent until you submit.");
    write(":edit NUMBER revisits a field; :review shows answers; :cancel cancels the command.");
    write(fmt::format(":submit [ACTION] — default {}; allowed: {}", default_action_,
        actions_ | std::views::join_with(std::string_view(", ")) | std::ranges::to<std::string>()));
}

void form_editor::input(std::string_view input)
{
    if (waiting_) { error("Waiting for the server response"); return; }
    if (input == ":review") { current_.reset(); render(); return; }
    if (input.starts_with(":edit ")) {
        auto index = number(input.substr(6));
        if (!index || *index > form_.fields.size() || !form_.fields[*index - 1].editable()) {
            error("Choose an editable field number"); return;
        }
        current_ = *index - 1;
        render();
        return;
    }
    if (input == ":cancel" || input == ":submit" || input.starts_with(":submit ")) {
        const auto action = input == ":cancel" ? std::string_view("cancel")
            : input == ":submit" ? std::string_view(default_action_) : input.substr(8);
        if (!std::ranges::contains(actions_, action)) { error("Action is not available"); return; }
        if (action != "cancel" && action != "prev") {
            auto valid = ::xmpp::submit_data_form(form_);
            if (!valid) { error(valid.error()); return; }
        }
        auto result = submit_(action, form_);
        if (!result) { error(result.error()); return; }
        waiting_ = true;
        render();
        return;
    }
    if (!current_) { error("Use :edit NUMBER or :submit"); return; }
    auto &field = form_.fields[*current_];
    if (input.empty()) { advance(); return; }
    if (input == ":omit") {
        if (field.required) { error("Required fields cannot be omitted"); return; }
        field.included = false;
        advance();
        return;
    }
    std::vector<std::string> values;
    if (input == ":empty") values.emplace_back();
    else if (input != ":clear") {
        if (input.starts_with("::")) input.remove_prefix(1);
        else if (input.starts_with(':')) { error("Unknown editor command; use :: for literal colon text"); return; }
        if (field.type.starts_with("list-")) {
            const auto tokens = ::xmpp::parse_adhoc_arguments(input);
            if (!tokens) { error(tokens.error()); return; }
            for (const auto &token : *tokens) {
                const auto index = number(token);
                if (!index || *index > field.options.size()) { error("Choose an offered option number"); return; }
                values.emplace_back(field.options[*index - 1].value);
            }
        } else if (field.type.ends_with("-multi")) {
            if (input.contains('\n')) {
                values = input | std::views::split('\n') | std::views::transform([](auto line) {
                    std::string value(line.begin(), line.end());
                    if (value.ends_with('\r')) value.pop_back();
                    return value;
                }) | std::ranges::to<std::vector>();
            } else {
                auto tokens = ::xmpp::parse_adhoc_arguments(input);
                if (!tokens) { error(tokens.error()); return; }
                values = std::move(*tokens);
            }
        } else values.emplace_back(input);
    }
    auto result = ::xmpp::set_data_form_values(field, values);
    if (!result) { error(result.error()); return; }
    advance();
}
}
