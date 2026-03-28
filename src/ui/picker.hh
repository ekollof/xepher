// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <memory>
    #include "../plugin.hh"
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
            // If buffer creation fails, self-destruct immediately.
            delete this;
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

        // Register as the active picker (replaces any previous one).
        picker_base::s_active = this;

        // Move selection to first selectable entry.
        selected_ = first_selectable(0);

        redraw();

        weechat_buffer_set(buf_, "display", "1");
    }

    ~picker() override
    {
        if (picker_base::s_active == this)
            picker_base::s_active = nullptr;
    }

    // Add an entry (for async/streaming use cases).
    void add_entry(entry e)
    {
        entries_.push_back(std::move(e));
        if (selected_ < 0)
            selected_ = first_selectable(0);
        redraw();
    }

    // Redraw the entire buffer from scratch.
    void redraw()
    {
        if (!buf_) return;

        weechat_buffer_clear(buf_);

        // Row 0: header / instructions
        weechat_printf_y(buf_, 0,
            "%s%s%s  %s[Enter=select  ↑↓=navigate  q=cancel]%s",
            weechat_color("bold"),
            title_.c_str(),
            weechat_color("reset"),
            weechat_color("darkgray"),
            weechat_color("reset"));

        for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
            const auto &e = entries_[i];
            bool is_sel = (i == selected_) && e.selectable;

            std::string line;
            if (is_sel)
                line += weechat_color("reverse");

            if (!e.selectable) {
                line += weechat_color("darkgray");
                line += "  ";
                line += e.label;
                line += weechat_color("reset");
            } else {
                line += "  ";
                line += e.label;
                if (!e.sublabel.empty()) {
                    line += "  ";
                    line += weechat_color("darkgray");
                    line += e.sublabel;
                    line += weechat_color("reset");
                }
                if (is_sel)
                    line += weechat_color("reset");
            }

            weechat_printf_y(buf_, i + 1, "%s", line.c_str());
        }
    }

    // ---------------------------------------------------------------------------
    // picker_base interface
    // ---------------------------------------------------------------------------

    void navigate(int delta) override
    {
        if (entries_.empty()) return;
        int n = static_cast<int>(entries_.size());
        int next = selected_;
        // Keep stepping until we land on a selectable entry (or wrap around once)
        for (int steps = 0; steps < n; ++steps) {
            next = (next + delta + n) % n;
            if (entries_[next].selectable)
                break;
        }
        selected_ = next;
        redraw();
    }

    void confirm() override
    {
        if (selected_ >= 0 && selected_ < static_cast<int>(entries_.size())
                && entries_[selected_].selectable) {
            auto selected_data = entries_[selected_].data;  // copy before close
            auto cb = std::move(on_select_);  // move out before close_picker() deletes this
            close_picker();   // deletes this via s_close_cb
            if (cb)
                cb(selected_data);  // safe: cb lives on the stack, not in the deleted object
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
    std::vector<entry>    entries_;
    std::string           title_;
    int                   selected_  = -1;
    action_cb             on_select_;
    close_cb              on_close_;

    // Find the index of the first selectable entry at or after `start`.
    int first_selectable(int start) const
    {
        for (int i = start; i < static_cast<int>(entries_.size()); ++i)
            if (entries_[i].selectable)
                return i;
        return -1;
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

        if (on_close_)
            on_close_();

        // Close the buffer — this triggers s_close_cb which will delete this.
        if (b)
            weechat_buffer_close(b);
        // 'this' is now deleted by s_close_cb.
    }

    // WeeChat input callback — we don't use the input bar but WeeChat requires it.
    static int s_input_cb(const void *ptr, void * /*data*/,
                          struct t_gui_buffer * /*buf*/,
                          const char * /*input_data*/)
    {
        (void) ptr;
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
