// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "account.hh"
#include "ui/form_editor.hh"
#include "weechat/ui_port.hh"
#include "xmpp/iq_error.hh"
#include "xmpp/iq_room_config.hh"

namespace weechat {
bool account::handle_room_config_editor(::xmpp::StanzaView response,
    const muc_owner_query_info &info)
{
    if (info.kind != muc_owner_kind::config_edit_get && info.kind != muc_owner_kind::config_edit_set
        && info.kind != muc_owner_kind::config_edit_cancel) return false;
    auto output = UiPort::for_buffer(buffer);
    const auto type = response.attr_string("type");
    if (type == "error") {
        const auto error = response.child("error");
        auto reason = ::xmpp::iq_error_text(error);
        if (reason.empty()) reason = "Room configuration rejected by the server";
        ui::form_editor::failed(info.editor_id, reason);
        output->printf_error(reason);
        return true;
    }
    if (type != "result") return true;
    if (info.kind != muc_owner_kind::config_edit_get) {
        ui::form_editor::close(info.editor_id);
        output->printf_network(fmt::format("Room configuration {} for {}",
            info.kind == muc_owner_kind::config_edit_cancel ? "cancelled" : "saved", info.room_jid));
        if (auto found = channels.find(info.room_jid); found != channels.end()) {
            found->second.clear_config_form();
            muc_modes_fetched.erase(info.room_jid);
            const auto id = stanza::uuid(context);
            muc_modes_queries.emplace(id, info.room_jid);
            connection.send(stanza::iq().type("get").to(info.room_jid).id(id)
                .xep0030().query().build(context).get());
        }
        return true;
    }
    if (!channels.contains(info.room_jid)) return true;
    auto form = ::xmpp::parse_data_form(response.child("query",
        "http://jabber.org/protocol/muc#owner").child("x", "jabber:x:data"));
    if (!form || form->type != "form") {
        output->printf_error(form ? "Expected a room configuration input form" : form.error());
        return true;
    }
    form->instructions.emplace_back("Use :submit to save. :cancel sends protocol cancellation; "
        "for a newly created locked room this destroys the room. /close sends nothing.");
    auto opened = ui::form_editor::open(name, info.editor_id,
        fmt::format("Room configuration — {}", info.room_jid), std::move(*form),
        {"submit", "cancel"}, "submit",
        [owner = name, room = info.room_jid, editor = info.editor_id]
        (std::string_view action, const ::xmpp::data_form &answers) -> std::expected<void, std::string> {
            const auto found = accounts.find(owner);
            if (found == accounts.end() || !found->second.connected())
                return std::unexpected("Account disconnected; run /roomconfig after reconnecting");
            auto &acc = found->second;
            const auto room_entry = acc.channels.find(room);
            if (room_entry == acc.channels.end() || room_entry->second.type != channel::chat_type::MUC)
                return std::unexpected("Room buffer closed; reopen it and run /roomconfig");
            if (action != "submit" && action != "cancel")
                return std::unexpected("Unsupported room configuration action");
            auto query = ::xmpp::make_room_config_query(&answers, action == "cancel");
            if (!query) return std::unexpected(query.error());
            const auto id = stanza::uuid(acc.context);
            muc_owner_query_info pending{room, acc.buffer,
                action == "cancel" ? muc_owner_kind::config_edit_cancel : muc_owner_kind::config_edit_set};
            pending.editor_id = editor;
            acc.muc_owner_queries.emplace(id, std::move(pending));
            auto request = stanza::iq().type("set").to(room).id(id);
            request.child(*query);
            acc.connection.send(request.build(acc.context).get());
            return {};
        });
    if (!opened) output->printf_error(opened.error());
    return true;
}
}
