// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

struct t_gui_buffer;

#include "test_export.hh"
#include "weechat/line_store.hh"

namespace weechat {

enum class PrintStyle {
    Plain,
    Error,
    Info,
    Network,
};

struct PrintAction {
    PrintStyle style = PrintStyle::Plain;
    std::string message;
};

struct PrintDateTagsAction {
    std::time_t date = 0;
    std::string tags;
    std::string message;
};

struct UpdateLineGlyphByTagAction {
    std::string acked_id;
    std::string glyph;
};

struct UpdateMessageByIdAction {
    std::string target_id;
    std::string new_message;
};

struct TombstoneMessageByIdAction {
    std::string target_id;
    std::string tombstone_message;
    std::string replacement_tags;
};

struct TombstoneRetractionByIdAction {
    std::string target_id;
    std::string tombstone_message;
    std::string replacement_tags;
    std::string sender_key;
    std::string occupant_id;
    bool prefer_occupant_id = false;
};

struct ApplyReactionsByIdAction {
    std::string target_id;
    std::string emojis;
};

struct NicklistRemoveAllAction {};

struct NicklistRemoveNickAction {
    std::string nick;
};

using UiAction = std::variant<
    PrintAction,
    PrintDateTagsAction,
    UpdateLineGlyphByTagAction,
    UpdateMessageByIdAction,
    TombstoneMessageByIdAction,
    TombstoneRetractionByIdAction,
    ApplyReactionsByIdAction,
    NicklistRemoveAllAction,
    NicklistRemoveNickAction>;

using RenderEvent = std::vector<UiAction>;

[[nodiscard]] XMPP_TEST_EXPORT std::optional<LineStoreLookupResult>
apply_ui_action(struct t_gui_buffer *buffer, const UiAction &action);

[[nodiscard]] XMPP_TEST_EXPORT std::optional<LineStoreLookupResult>
apply_render_event(struct t_gui_buffer *buffer, const RenderEvent &event);

[[nodiscard]] XMPP_TEST_EXPORT RenderEvent build_incoming_receipt_render_event(
    std::string_view acked_id,
    bool muc_channel);

[[nodiscard]] XMPP_TEST_EXPORT RenderEvent build_incoming_displayed_render_event(
    std::string_view acked_id,
    bool muc_channel);

[[nodiscard]] XMPP_TEST_EXPORT RenderEvent build_reactions_render_event(
    std::string_view target_id,
    std::string_view emojis);

[[nodiscard]] XMPP_TEST_EXPORT RenderEvent build_correction_render_event(
    std::string_view target_id,
    std::string_view new_message);

[[nodiscard]] XMPP_TEST_EXPORT RenderEvent build_moderation_tombstone_render_event(
    std::string_view target_id,
    std::string_view tombstone_message,
    std::string_view replacement_tags);

[[nodiscard]] XMPP_TEST_EXPORT RenderEvent build_retraction_tombstone_render_event(
    std::string_view target_id,
    std::string_view tombstone_message,
    std::string_view replacement_tags,
    std::string_view sender_key,
    std::string_view occupant_id,
    bool prefer_occupant_id);

}  // namespace weechat