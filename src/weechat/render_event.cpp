// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "render_event.hh"

#include <string>
#include <weechat/weechat-plugin.h>

#include "../plugin.hh"
#include "line_store.hh"
#include "ui_port.hh"

namespace weechat {

namespace {

[[nodiscard]] RenderEvent glyph_update_event(std::string_view acked_id, std::string_view glyph)
{
    if (acked_id.empty())
        return {};

    RenderEvent event;
    event.push_back(UpdateLineGlyphByTagAction{
        std::string(acked_id),
        std::string(glyph),
    });
    return event;
}

}  // namespace

void apply_ui_action(struct t_gui_buffer *const buffer, const UiAction &action)
{
    if (!buffer)
        return;

    std::visit([&](const auto &act) {
        using T = std::decay_t<decltype(act)>;
        if constexpr (std::is_same_v<T, PrintAction>)
        {
            auto ui = UiPort::for_buffer(buffer);
            switch (act.style)
            {
            case PrintStyle::Plain:    ui->printf(act.message); break;
            case PrintStyle::Error:    ui->printf_error(act.message); break;
            case PrintStyle::Info:     ui->printf_info(act.message); break;
            case PrintStyle::Network:  ui->printf_network(act.message); break;
            }
        }
        else if constexpr (std::is_same_v<T, PrintDateTagsAction>)
        {
            auto ui = UiPort::for_buffer(buffer);
            ui->printf_date_tags(act.date, act.tags, act.message);
        }
        else if constexpr (std::is_same_v<T, UpdateLineGlyphByTagAction>)
        {
            (void)line_store_update_line_glyph_by_tag(buffer, act.acked_id, act.glyph);
        }
        else if constexpr (std::is_same_v<T, UpdateMessageByIdAction>)
        {
            (void)line_store_update_message_by_id(buffer, act.target_id, act.new_message);
        }
        else if constexpr (std::is_same_v<T, TombstoneMessageByIdAction>)
        {
            (void)line_store_tombstone_message_by_id(
                buffer, act.target_id, act.tombstone_message, act.replacement_tags);
        }
        else if constexpr (std::is_same_v<T, TombstoneRetractionByIdAction>)
        {
            (void)line_store_tombstone_retraction_by_id(
                buffer,
                act.target_id,
                act.tombstone_message,
                act.replacement_tags,
                act.sender_key,
                act.occupant_id,
                act.prefer_occupant_id);
        }
        else if constexpr (std::is_same_v<T, ApplyReactionsByIdAction>)
        {
            (void)line_store_apply_reactions_by_id(buffer, act.target_id, act.emojis);
        }
        else if constexpr (std::is_same_v<T, NicklistRemoveAllAction>)
        {
            weechat_nicklist_remove_all(buffer);
        }
        else if constexpr (std::is_same_v<T, NicklistRemoveNickAction>)
        {
            if (struct t_gui_nick *const nick = weechat_nicklist_search_nick(
                    buffer, nullptr, act.nick.c_str()))
                weechat_nicklist_remove_nick(buffer, nick);
        }
    }, action);
}

void apply_render_event(struct t_gui_buffer *const buffer, const RenderEvent &event)
{
    for (const UiAction &action : event)
        apply_ui_action(buffer, action);
}

RenderEvent build_incoming_receipt_render_event(
    const std::string_view acked_id,
    const bool muc_channel)
{
    if (muc_channel || acked_id.empty())
        return {};
    return glyph_update_event(acked_id, k_glyph_delivered);
}

RenderEvent build_incoming_displayed_render_event(
    const std::string_view acked_id,
    const bool muc_channel)
{
    if (muc_channel || acked_id.empty())
        return {};
    return glyph_update_event(acked_id, k_glyph_seen);
}

}  // namespace weechat