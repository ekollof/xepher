// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <ctime>
#include <string>
#include <variant>
#include <vector>

#include "../src/weechat/buffer_port.hh"
#include "../src/weechat/render_event.hh"
#include "../src/weechat/runtime_port.hh"
#include "../src/weechat/ui_port.hh"

namespace test_weechat {

class CapturingUiPort final : public weechat::UiPort {
public:
    struct dated_line {
        std::time_t date{};
        std::string tags;
        std::string msg;
    };

    std::vector<std::string> lines;
    std::vector<std::string> errors;
    std::vector<std::string> info;
    std::vector<std::string> network;
    std::vector<dated_line> dated;

    void printf(std::string_view msg) override { lines.emplace_back(msg); }
    void printf_error(std::string_view msg) override { errors.emplace_back(msg); }
    void printf_info(std::string_view msg) override { info.emplace_back(msg); }
    void printf_network(std::string_view msg) override { network.emplace_back(msg); }
    void printf_date_tags(std::time_t date, std::string_view tags, std::string_view msg) override
    {
        dated.push_back({date, std::string(tags), std::string(msg)});
    }
    void printf_y(int y, std::string_view msg) override
    {
        lines.emplace_back("[y=" + std::to_string(y) + "] " + std::string(msg));
    }

    void clear()
    {
        lines.clear();
        errors.clear();
        info.clear();
        network.clear();
        dated.clear();
    }
};

class CapturingBufferPort final : public weechat::BufferPort {
public:
    struct search_call {
        std::string plugin;
        std::string name;
        struct t_gui_buffer *result = nullptr;
    };

    std::vector<search_call> searches;
    int nicklist_remove_all_count = 0;
    std::vector<std::string> nicklist_removed;

    [[nodiscard]] struct t_gui_buffer *search(std::string_view plugin,
                                              std::string_view name) override
    {
        searches.push_back({std::string(plugin), std::string(name), nullptr});
        return searches.back().result;
    }

    void nicklist_remove_all(struct t_gui_buffer * /*buffer*/) override
    {
        ++nicklist_remove_all_count;
    }

    void nicklist_remove_nick(struct t_gui_buffer * /*buffer*/,
                              std::string_view nick) override
    {
        nicklist_removed.emplace_back(nick);
    }

    void clear()
    {
        searches.clear();
        nicklist_remove_all_count = 0;
        nicklist_removed.clear();
    }
};

class StubRuntimePort final : public weechat::RuntimePort {
public:
    explicit StubRuntimePort(std::string version = "test-version")
        : version_(std::move(version))
    {}

    [[nodiscard]] std::string version_string() override { return version_; }
    [[nodiscard]] const char *color(std::string_view) override { return ""; }
    [[nodiscard]] const char *prefix(std::string_view) override { return ""; }
    [[nodiscard]] std::string xmpp_color(std::string_view) override { return ""; }

private:
    std::string version_;
};

// Records line-store side effects from RenderEvent application (no real buffer).
struct LineStoreCapture {
    struct glyph_update {
        std::string acked_id;
        std::string glyph;
    };
    struct message_update {
        std::string target_id;
        std::string new_message;
    };
    struct tombstone_message {
        std::string target_id;
        std::string tombstone_message;
        std::string replacement_tags;
    };
    struct tombstone_retraction {
        std::string target_id;
        std::string tombstone_message;
        std::string replacement_tags;
        std::string sender_key;
        std::string occupant_id;
        bool prefer_occupant_id = false;
    };
    struct reactions_update {
        std::string target_id;
        std::string emojis;
    };

    std::vector<glyph_update> glyph_updates;
    std::vector<message_update> message_updates;
    std::vector<tombstone_message> tombstones;
    std::vector<tombstone_retraction> retractions;
    std::vector<reactions_update> reactions;

    void clear()
    {
        glyph_updates.clear();
        message_updates.clear();
        tombstones.clear();
        retractions.clear();
        reactions.clear();
    }
};

inline void apply_ui_action_to(weechat::UiPort &ui,
                               weechat::BufferPort &buffer,
                               LineStoreCapture &line_store,
                               const weechat::UiAction &action,
                               struct t_gui_buffer *nicklist_buffer = nullptr)
{
    std::visit([&](const auto &act) {
        using T = std::decay_t<decltype(act)>;
        if constexpr (std::is_same_v<T, weechat::PrintAction>)
        {
            switch (act.style)
            {
            case weechat::PrintStyle::Plain:    ui.printf(act.message); break;
            case weechat::PrintStyle::Error:    ui.printf_error(act.message); break;
            case weechat::PrintStyle::Info:     ui.printf_info(act.message); break;
            case weechat::PrintStyle::Network:  ui.printf_network(act.message); break;
            }
        }
        else if constexpr (std::is_same_v<T, weechat::PrintDateTagsAction>)
        {
            ui.printf_date_tags(act.date, act.tags, act.message);
        }
        else if constexpr (std::is_same_v<T, weechat::UpdateLineGlyphByTagAction>)
        {
            line_store.glyph_updates.push_back({act.acked_id, act.glyph});
        }
        else if constexpr (std::is_same_v<T, weechat::UpdateMessageByIdAction>)
        {
            line_store.message_updates.push_back({act.target_id, act.new_message});
        }
        else if constexpr (std::is_same_v<T, weechat::TombstoneMessageByIdAction>)
        {
            line_store.tombstones.push_back(
                {act.target_id, act.tombstone_message, act.replacement_tags});
        }
        else if constexpr (std::is_same_v<T, weechat::TombstoneRetractionByIdAction>)
        {
            line_store.retractions.push_back({
                act.target_id,
                act.tombstone_message,
                act.replacement_tags,
                act.sender_key,
                act.occupant_id,
                act.prefer_occupant_id,
            });
        }
        else if constexpr (std::is_same_v<T, weechat::ApplyReactionsByIdAction>)
        {
            line_store.reactions.push_back({act.target_id, act.emojis});
        }
        else if constexpr (std::is_same_v<T, weechat::NicklistRemoveAllAction>)
        {
            buffer.nicklist_remove_all(nicklist_buffer);
        }
        else if constexpr (std::is_same_v<T, weechat::NicklistRemoveNickAction>)
        {
            buffer.nicklist_remove_nick(nicklist_buffer, act.nick);
        }
    }, action);
}

inline void apply_render_event_to(weechat::UiPort &ui,
                                  weechat::BufferPort &buffer,
                                  LineStoreCapture &line_store,
                                  const weechat::RenderEvent &event,
                                  struct t_gui_buffer *nicklist_buffer = nullptr)
{
    for (const weechat::UiAction &action : event)
        apply_ui_action_to(ui, buffer, line_store, action, nicklist_buffer);
}

// Bundles ports used by handler slice tests (parse → RenderEvent → assert side effects).
struct HandlerTestHarness {
    CapturingUiPort ui;
    weechat::NullUiPort null_ui;
    CapturingBufferPort buffer;
    LineStoreCapture line_store;
    StubRuntimePort runtime{"4.9.0-test"};

    void apply(const weechat::RenderEvent &event,
               struct t_gui_buffer *nicklist_buffer = nullptr)
    {
        apply_render_event_to(ui, buffer, line_store, event, nicklist_buffer);
    }

    void clear()
    {
        ui.clear();
        buffer.clear();
        line_store.clear();
    }
};

}  // namespace test_weechat