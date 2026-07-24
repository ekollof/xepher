// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <fmt/core.h>
#include <algorithm>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <memory>
#include "plugin.hh"
#include "weechat/runtime_port.hh"
#include "weechat/ui_port.hh"
#include <weechat/weechat-plugin.h>

namespace weechat::ui {

// ---------------------------------------------------------------------------
// picker_base — non-template abstract interface used by the static dispatcher
// in picker.cpp so that /xmpp-picker-nav can call navigate()/confirm()/cancel()
// without knowing T.
// ---------------------------------------------------------------------------
class picker_base
{
public:
    virtual ~picker_base() = default;
    virtual void navigate(int delta) = 0;  // +1 = down, -1 = up
    virtual void confirm() = 0;
    virtual void cancel() = 0;

    // Called from picker.cpp to register/unregister as the active picker.
    static picker_base *s_active;
};

// ---------------------------------------------------------------------------
// picker<T> — interactive navigable list in a WeeChat "free" buffer.
//
// Usage:
//   auto p = std::make_unique<picker<std::string>>(
//       "xmpp.mypicker", "Pick a mood",
//       entries,
//       [&](const std::string& selected) { /* do something with selected */ }
//   );
//   p.release();  // picker owns itself; deleted on close
//
// Lifetime: the picker owns itself — it is deleted inside cancel()/confirm()
// after calling the user callbacks and closing the WeeChat buffer.
// Callers should release() the unique_ptr after construction and not touch it.
//
// Search: the input bar is live — typing filters entries fzf-style
// (case-insensitive substring match across label + sublabel).
// ---------------------------------------------------------------------------
template <typename T>
class picker final : public picker_base
{
public:
    struct entry {
        T           data;
        std::string label;      // primary display text (column 1)
        std::string sublabel;   // secondary info (column 2, may be empty)
        bool        selectable = true;  // false = greyed header/separator row
    };

    using action_cb = std::function<void(const T &selected)>;
    using close_cb  = std::function<void()>;

    // -----------------------------------------------------------------------
    // Constructor — opens the WeeChat buffer and switches to it.
    //
    // buf_name     : WeeChat buffer name (e.g. "xmpp.picker.mood")
    // title        : header line displayed at the top of the picker
    // entries      : initial list of items (may be empty; use add_entry later)
    // on_select    : called with the selected item when the user presses Enter
    // on_close     : optional; called on cancel or after on_select
    // origin_buf   : buffer to return focus to after the picker closes
    // sort_entries : if true, sort entries alphabetically by label before display
    // -----------------------------------------------------------------------
    picker(std::string_view buf_name,
           std::string_view title,
           std::vector<entry> entries,
           action_cb on_select,
           close_cb  on_close,
           struct t_gui_buffer *origin_buf,
           bool sort_entries = false)
         : origin_buf_(origin_buf)
        , entries_(std::move(entries))
        , title_(title)
        , on_select_(std::move(on_select))
        , on_close_(std::move(on_close))
    {
        if (sort_entries)
        {
            std::ranges::stable_sort(entries_, [](const entry &a, const entry &b) {
                // Case-insensitive sort on label; non-selectable entries (headers)
                // sort before selectable ones to keep separators at the top.
                if (a.selectable != b.selectable)
                    return !a.selectable;
                std::string la = a.label, lb = b.label;
                std::ranges::transform(la, la.begin(), ::tolower);
                std::ranges::transform(lb, lb.begin(), ::tolower);
                return la < lb;
            });
        }

        buf_ = weechat_buffer_new(
            std::string(buf_name).c_str(),
            &picker::s_input_cb, this, nullptr,
            &picker::s_close_cb, this, nullptr);

        if (!buf_) {
            // Buffer creation failed — leave buf_ null. The caller's
            // unique_ptr will destroy the picker normally via RAII.
            return;
        }

        weechat_buffer_set(buf_, "type", "free");
        weechat_buffer_set(buf_, "title",
            std::string(title).c_str());
        weechat_buffer_set(buf_, "key_bind_meta2-A",   "/xmpp-picker-nav up");
        weechat_buffer_set(buf_, "key_bind_meta2-B",   "/xmpp-picker-nav down");
        weechat_buffer_set(buf_, "key_bind_ctrl-M",    "/xmpp-picker-nav enter");
        weechat_buffer_set(buf_, "key_bind_ctrl-J",    "/xmpp-picker-nav enter");
        weechat_buffer_set(buf_, "key_bind_q",         "/xmpp-picker-nav quit");
        weechat_buffer_set(buf_, "key_bind_ctrl-[",    "/xmpp-picker-nav quit");

        // Hook input_text_changed signal for live search filtering.
        signal_hook_ = weechat_hook_signal("input_text_changed",
                                           &picker::s_signal_cb, this, nullptr);

        // Register as the active picker (replaces any previous one).
        picker_base::s_active = this;

        // Build initial visible set (all entries) and set selection.
        refilter();

        weechat_buffer_set(buf_, "display", "1");
    }

    ~picker() override
    {
        if (signal_hook_) {
            weechat_unhook(signal_hook_);
            signal_hook_ = nullptr;
        }
        if (picker_base::s_active == this)
            picker_base::s_active = nullptr;
    }

    [[nodiscard]] explicit operator bool() const noexcept { return buf_ != nullptr; }

    // Add an entry (for async/streaming use cases).
    void add_entry(entry e)
    {
        entries_.push_back(std::move(e));
        refilter();
    }

    // Redraw the entire buffer from scratch.
    void redraw()
    {
        if (!buf_) return;

        weechat_buffer_clear(buf_);

        // Row 0: header / instructions / current filter
        std::string header;
        header += weechat::RuntimePort::default_runtime().color("bold");
        header += title_;
        header += weechat::RuntimePort::default_runtime().color("reset");
        if (!filter_.empty()) {
            header += "  ";
            header += weechat::RuntimePort::default_runtime().color("yellow");
            header += "[search: ";
            header += filter_;
            header += "]";
            header += weechat::RuntimePort::default_runtime().color("reset");
        }
        header += "  ";
        header += weechat::RuntimePort::default_runtime().color("darkgray");
        header += "[Enter=select  ↑↓=navigate  q=cancel  type to search]";
        header += weechat::RuntimePort::default_runtime().color("reset");
        weechat::UiPort::for_buffer(buf_)->printf_y(0, header);

        if (visible_.empty()) {
            weechat::UiPort::for_buffer(buf_)->printf_y(1,
                fmt::format("  {}(no matches){}",
                            weechat::RuntimePort::default_runtime().color("darkgray"), weechat::RuntimePort::default_runtime().color("reset")));
            return;
        }

        ensure_selection_visible();
        const int page = page_size();
        const int n = static_cast<int>(visible_.size());
        int y = 1;
        for (int row = scroll_top_; row < n && (row - scroll_top_) < page; ++row) {
            const auto &e = entries_[visible_[row]];
            bool is_sel = (row == selected_vis_) && e.selectable;

            std::string line;
            if (is_sel)
                line += weechat::RuntimePort::default_runtime().color("reverse");

            if (!e.selectable) {
                line += weechat::RuntimePort::default_runtime().color("darkgray");
                line += "  ";
                line += e.label;
                line += weechat::RuntimePort::default_runtime().color("reset");
            } else {
                line += "  ";
                // Highlight matching portion in the label if filtering.
                if (!filter_.empty()) {
                    line += highlight_match(e.label, filter_, is_sel);
                } else {
                    line += e.label;
                }
                if (!e.sublabel.empty()) {
                    line += "  ";
                    line += weechat::RuntimePort::default_runtime().color("darkgray");
                    if (!filter_.empty()) {
                        line += highlight_match(e.sublabel, filter_, false);
                    } else {
                        line += e.sublabel;
                    }
                    line += weechat::RuntimePort::default_runtime().color("reset");
                }
                if (is_sel)
                    line += weechat::RuntimePort::default_runtime().color("reset");
            }

            weechat::UiPort::for_buffer(buf_)->printf_y(y++, line);
        }
        // Blank leftover rows after a shorter page (shrink / filter).
        while (y <= page)
            weechat::UiPort::for_buffer(buf_)->printf_y(y++, "");
    }

    // ---------------------------------------------------------------------------
    // picker_base interface
    // ---------------------------------------------------------------------------

    void navigate(int delta) override
    {
        if (visible_.empty()) return;
        int n = static_cast<int>(visible_.size());
        int next = selected_vis_;
        for (int steps = 0; steps < n; ++steps) {
            next = (next + delta + n) % n;
            if (entries_[visible_[next]].selectable)
                break;
        }
        selected_vis_ = next;
        ensure_selection_visible();
        redraw();
    }

    void confirm() override
    {
        if (selected_vis_ >= 0 && selected_vis_ < static_cast<int>(visible_.size())
                && entries_[visible_[selected_vis_]].selectable) {
            auto selected_data = entries_[visible_[selected_vis_]].data;
            auto cb = std::move(on_select_);
            close_picker();
            if (cb)
                cb(selected_data);
        } else {
            close_picker();
        }
    }

    void cancel() override
    {
        close_picker();
    }

private:
    struct t_gui_buffer  *buf_       = nullptr;
    struct t_gui_buffer  *origin_buf_ = nullptr;
    struct t_hook        *signal_hook_ = nullptr;
    std::vector<entry>    entries_;
    std::vector<int>      visible_;   // indices into entries_ that match filter_
    std::string           title_;
    std::string           filter_;
    int                   selected_vis_ = -1;  // index into visible_
    int                   scroll_top_ = 0;     // first visible_ index shown (viewport)
    action_cb             on_select_;
    close_cb              on_close_;

    // Chat area rows available for list entries (header uses row 0).
    [[nodiscard]] int page_size() const
    {
        int h = 20;
        if (auto *win = static_cast<struct t_gui_window *>(weechat_current_window()))
        {
            const int chat_h = weechat_window_get_integer(win, "win_chat_height");
            if (chat_h > 0)
                h = chat_h;
        }
        // One row reserved for the title/instructions line.
        return std::max(1, h - 1);
    }

    // Keep selected_vis_ inside the on-screen window [scroll_top_, scroll_top_+page).
    void ensure_selection_visible()
    {
        if (selected_vis_ < 0 || visible_.empty())
        {
            scroll_top_ = 0;
            return;
        }
        const int page = page_size();
        if (selected_vis_ < scroll_top_)
            scroll_top_ = selected_vis_;
        else if (selected_vis_ >= scroll_top_ + page)
            scroll_top_ = selected_vis_ - page + 1;

        const int max_top = std::max(0, static_cast<int>(visible_.size()) - page);
        scroll_top_ = std::clamp(scroll_top_, 0, max_top);
    }

    // Rebuild visible_ from entries_ using the current filter_.
    // Tries to keep the currently selected entry selected; otherwise resets
    // to the first selectable visible entry.
    void refilter()
    {
        // Remember what entry was selected so we can try to keep it.
        int prev_entry_idx = (selected_vis_ >= 0 && selected_vis_ < static_cast<int>(visible_.size()))
            ? visible_[selected_vis_] : -1;

        visible_.clear();

        std::string filter_lc = filter_;
        std::ranges::transform(filter_lc, filter_lc.begin(), ::tolower);

        for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
            const auto &e = entries_[i];
            if (filter_lc.empty() || !e.selectable) {
                // Always show non-selectable headers; show all when no filter.
                visible_.push_back(i);
            } else {
                // Case-insensitive substring match against label + sublabel.
                std::string label_lc = e.label;
                std::string sub_lc   = e.sublabel;
                std::ranges::transform(label_lc, label_lc.begin(), ::tolower);
                std::ranges::transform(sub_lc,   sub_lc.begin(),   ::tolower);
                if (label_lc.contains(filter_lc) || sub_lc.contains(filter_lc))
                    visible_.push_back(i);
            }
        }

        // Try to restore the same entry; otherwise pick first selectable.
        selected_vis_ = -1;
        if (prev_entry_idx >= 0) {
            for (int r = 0; r < static_cast<int>(visible_.size()); ++r) {
                if (visible_[r] == prev_entry_idx) {
                    selected_vis_ = r;
                    break;
                }
            }
        }
        if (selected_vis_ < 0)
            selected_vis_ = first_selectable_vis(0);

        ensure_selection_visible();
        redraw();
    }

    // Find first selectable row in visible_ at or after `start`.
    int first_selectable_vis(int start) const
    {
        for (int r = start; r < static_cast<int>(visible_.size()); ++r)
            if (entries_[visible_[r]].selectable)
                return r;
        return -1;
    }

    // Return `text` with the first case-insensitive occurrence of `needle`
    // wrapped in yellow colour codes. Falls back to plain text if no match.
    static std::string highlight_match(std::string_view text,
                                       std::string_view needle,
                                       bool is_selected)
    {
        if (needle.empty()) return std::string(text);

        std::string text_lc = std::string(text);
        std::string needle_lc = std::string(needle);
        std::ranges::transform(text_lc,   text_lc.begin(),   ::tolower);
        std::ranges::transform(needle_lc, needle_lc.begin(), ::tolower);

        auto pos = text_lc.find(needle_lc);
        if (pos == std::string::npos) return std::string(text);

        // Temporarily suspend reverse video so the highlight colour is visible.
        std::string result;
        result += std::string(text.substr(0, pos));
        if (is_selected) result += weechat::RuntimePort::default_runtime().color("reset");
        result += weechat::RuntimePort::default_runtime().color("yellow");
        result += weechat::RuntimePort::default_runtime().color("bold");
        result += std::string(text.substr(pos, needle.size()));
        result += weechat::RuntimePort::default_runtime().color("reset");
        if (is_selected) result += weechat::RuntimePort::default_runtime().color("reverse");
        result += std::string(text.substr(pos + needle.size()));
        return result;
    }

    void close_picker()
    {
        // Restore focus to the originating buffer before closing this one,
        // to avoid a flash to whatever WeeChat would otherwise show.
        if (origin_buf_)
            weechat_buffer_set(origin_buf_, "display", "1");

        struct t_gui_buffer *b = buf_;
        buf_ = nullptr;
        picker_base::s_active = nullptr;

        if (signal_hook_) {
            weechat_unhook(signal_hook_);
            signal_hook_ = nullptr;
        }

        if (on_close_)
            on_close_();

        // Close the buffer — this triggers s_close_cb which will delete this.
        if (b)
            weechat_buffer_close(b);
        // 'this' is now deleted by s_close_cb.
    }

    // WeeChat input callback — called when the user presses Enter in the
    // input bar. We handle confirmation via the key binding instead, so
    // this is a no-op (but must exist for buffer_new).
    static int s_input_cb(const void *ptr, void * /*data*/,
                          struct t_gui_buffer * /*buf*/,
                          const char * /*input_data*/)
    {
        (void) ptr;
        return WEECHAT_RC_OK;
    }

    // WeeChat signal callback — fires on every keystroke in the input bar
    // (input_text_changed). Read the current input text and refilter.
    static int s_signal_cb(const void *ptr, void * /*data*/,
                           const char * /*signal*/, const char * /*type_data*/,
                           void * /*signal_data*/)
    {
        auto *self = static_cast<picker *>(const_cast<void *>(ptr));
        if (!self || !self->buf_) return WEECHAT_RC_OK;

        // Only act when the picker buffer is the current buffer.
        if (weechat_current_buffer() != self->buf_) return WEECHAT_RC_OK;

        const char *input = weechat_buffer_get_string(self->buf_, "input");
        std::string new_filter = input ? input : "";

        if (new_filter != self->filter_) {
            self->filter_ = std::move(new_filter);
            self->refilter();
        }
        return WEECHAT_RC_OK;
    }

    // WeeChat close callback — deletes this picker instance.
    static int s_close_cb(const void *ptr, void * /*data*/,
                          struct t_gui_buffer * /*buf*/)
    {
        auto *self = static_cast<picker *>(const_cast<void *>(ptr));
        if (self) {
            if (picker_base::s_active == self)
                picker_base::s_active = nullptr;
            delete self;
        }
        return WEECHAT_RC_OK;
    }
};

} // namespace weechat::ui
