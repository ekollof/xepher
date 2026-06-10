// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "weechat/render_event.hh"

#include <optional>
#include <string>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "weechat/buffer_port.hh"
#include "weechat/line_store.hh"
#include "weechat/ui_port.hh"

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

[[nodiscard]] RenderEvent single_action_render_event(UiAction action)
{
    RenderEvent event;
    event.push_back(std::move(action));
    return event;
}

}  // namespace

std::optional<LineStoreLookupResult> apply_ui_action(struct t_gui_buffer *const buffer,
                                                     const UiAction &action)
{
    if (!buffer)
        return std::nullopt;

    return std::visit([&](const auto &act) -> std::optional<LineStoreLookupResult> {
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
            return std::nullopt;
        }
        if constexpr (std::is_same_v<T, PrintDateTagsAction>)
        {
            auto ui = UiPort::for_buffer(buffer);
            ui->printf_date_tags(act.date, act.tags, act.message);
            return std::nullopt;
        }
        if constexpr (std::is_same_v<T, UpdateLineGlyphByTagAction>)
        {
            (void)line_store_update_line_glyph_by_tag(buffer, act.acked_id, act.glyph);
            return std::nullopt;
        }
        if constexpr (std::is_same_v<T, UpdateMessageByIdAction>)
        {
            (void)line_store_update_message_by_id(buffer, act.target_id, act.new_message);
            return std::nullopt;
        }
        if constexpr (std::is_same_v<T, TombstoneMessageByIdAction>)
        {
            return line_store_tombstone_message_by_id(
                buffer, act.target_id, act.tombstone_message, act.replacement_tags);
        }
        if constexpr (std::is_same_v<T, TombstoneRetractionByIdAction>)
        {
            return line_store_tombstone_retraction_by_id(
                buffer,
                act.target_id,
                act.tombstone_message,
                act.replacement_tags,
                act.sender_key,
                act.occupant_id,
                act.prefer_occupant_id);
        }
        if constexpr (std::is_same_v<T, ApplyReactionsByIdAction>)
        {
            (void)line_store_apply_reactions_by_id(buffer, act.target_id, act.emojis);
            return std::nullopt;
        }
        if constexpr (std::is_same_v<T, NicklistRemoveAllAction>)
        {
            BufferPort::default_port_ref().nicklist_remove_all(buffer);
            return std::nullopt;
        }
        if constexpr (std::is_same_v<T, NicklistRemoveNickAction>)
        {
            BufferPort::default_port_ref().nicklist_remove_nick(buffer, act.nick);
            return std::nullopt;
        }
        return std::nullopt;
    }, action);
}

std::optional<LineStoreLookupResult> apply_render_event(struct t_gui_buffer *const buffer,
                                                        const RenderEvent &event)
{
    std::optional<LineStoreLookupResult> result;
    for (const UiAction &action : event)
    {
        if (auto lookup = apply_ui_action(buffer, action))
            result = lookup;
    }
    return result;
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

RenderEvent build_reactions_render_event(const std::string_view target_id,
                                         const std::string_view emojis)
{
    if (target_id.empty())
        return {};
    return single_action_render_event(ApplyReactionsByIdAction{
        std::string(target_id),
        std::string(emojis),
    });
}

RenderEvent build_correction_render_event(const std::string_view target_id,
                                          const std::string_view new_message)
{
    if (target_id.empty())
        return {};
    return single_action_render_event(UpdateMessageByIdAction{
        std::string(target_id),
        std::string(new_message),
    });
}

RenderEvent build_moderation_tombstone_render_event(
    const std::string_view target_id,
    const std::string_view tombstone_message,
    const std::string_view replacement_tags)
{
    if (target_id.empty())
        return {};
    return single_action_render_event(TombstoneMessageByIdAction{
        std::string(target_id),
        std::string(tombstone_message),
        std::string(replacement_tags),
    });
}

RenderEvent build_retraction_tombstone_render_event(
    const std::string_view target_id,
    const std::string_view tombstone_message,
    const std::string_view replacement_tags,
    const std::string_view sender_key,
    const std::string_view occupant_id,
    const bool prefer_occupant_id)
{
    if (target_id.empty())
        return {};
    return single_action_render_event(TombstoneRetractionByIdAction{
        std::string(target_id),
        std::string(tombstone_message),
        std::string(replacement_tags),
        std::string(sender_key),
        std::string(occupant_id),
        prefer_occupant_id,
    });
}

}  // namespace weechat