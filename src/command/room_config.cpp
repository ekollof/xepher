// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <algorithm>
#include <ranges>
#include <weechat/weechat-plugin.h>
#include "plugin.hh"
#include "account.hh"
#include "buffer.hh"
#include "command.hh"
#include "ui/form_editor.hh"
#include "weechat/ui_port.hh"
#include "xmpp/iq_room_config.hh"

int command__roomconfig([[maybe_unused]] const void *pointer, [[maybe_unused]] void *data,
    t_gui_buffer *buffer, int argc, [[maybe_unused]] char **argv, [[maybe_unused]] char **argv_eol)
{
    weechat::account *acc = nullptr;
    weechat::channel *room = nullptr;
    buffer__get_account_and_channel(buffer, &acc, &room);
    auto output = weechat::UiPort::for_buffer(buffer);
    if (!acc || !room || room->type != weechat::channel::chat_type::MUC) {
        output->printf_error("/roomconfig must be run in a MUC buffer");
        return WEECHAT_RC_OK;
    }
    if (!acc->connected() || argc != 1) {
        output->printf_error(acc->connected() ? "Usage: /roomconfig" : "Account is disconnected");
        return WEECHAT_RC_OK;
    }
    const auto editor = weechat::ui::adhoc_form_id(acc->name, room->id, "roomconfig", "");
    if (std::ranges::any_of(acc->muc_owner_queries, [&editor](const auto &entry) {
        return entry.second.editor_id == editor;
    })) {
        output->printf_error("A room configuration request is already pending");
        return WEECHAT_RC_OK;
    }
    weechat::ui::form_editor::close(editor);
    const auto id = stanza::uuid(acc->context);
    weechat::account::muc_owner_query_info pending{
        room->id, acc->buffer, weechat::account::muc_owner_kind::config_edit_get};
    pending.editor_id = editor;
    acc->muc_owner_queries.emplace(id, std::move(pending));
    auto query = ::xmpp::make_room_config_query();
    auto request = stanza::iq().type("get").to(room->id).id(id);
    request.child(*query);
    acc->connection.send(request.build(acc->context).get());
    output->printf_network("Fetching room configuration…");
    return WEECHAT_RC_OK;
}
