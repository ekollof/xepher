// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <algorithm>
#include <ranges>
#include "account.hh"
#include "ui/form_editor.hh"
#include "weechat/ui_port.hh"
#include "xmpp/iq_error.hh"
#include "xmpp/iq_registration.hh"

namespace weechat {
bool account::handle_muc_registration(::xmpp::StanzaView response, const muc_owner_query_info &info)
{
    if (info.kind != muc_owner_kind::register_get && info.kind != muc_owner_kind::register_set
        && info.kind != muc_owner_kind::register_cancel) return false;
    auto output = UiPort::for_buffer(buffer);
    if (response.type() == "error") {
        const auto reason = ::xmpp::iq_error_text(response.child("error"));
        ui::form_editor::failed(info.editor_id, reason);
        output->printf_error(fmt::format("Room registration failed for {}: {}", info.room_jid, reason));
        return true;
    }
    if (response.type() != "result") return true;
    if (info.kind != muc_owner_kind::register_get) {
        ui::form_editor::close(info.editor_id);
        output->printf_network(fmt::format("Room registration {} for {}",
            info.kind == muc_owner_kind::register_cancel ? "cancelled" : "accepted", info.room_jid));
        return true;
    }
    const auto query = response.child("query", "jabber:iq:register");
    if (!query.valid()) {
        output->printf_error("Room registration response is missing jabber:iq:register query");
        return true;
    }
    if (query.child("registered").valid()) {
        output->printf_network(fmt::format("Already registered with {} (nickname: {})",
            info.room_jid, query.child("username").text()));
        if (!query.child("x", "jabber:x:data").valid()) return true;
    }
    if (!info.edit_registration) {
        auto form = ::xmpp::parse_data_form(query.child("x", "jabber:x:data"));
        if (!form) output->printf_error(form.error());
        else std::ranges::for_each(::xmpp::data_form_lines(*form), [&](std::string_view line) { output->printf(line); });
        return true;
    }
    if (!channels.contains(info.room_jid)) return true;
    auto form = ::xmpp::muc_registration_form(query, info.register_nick);
    if (!form) { output->printf_error(form.error()); return true; }
    auto opened = ui::form_editor::open(name, info.editor_id,
        fmt::format("Room registration — {}", info.room_jid), std::move(*form), {"submit", "cancel"}, "submit",
        [owner = name, room = info.room_jid, editor = info.editor_id]
        (std::string_view action, const ::xmpp::data_form &answers) -> std::expected<void, std::string> {
            const auto found = accounts.find(owner);
            if (found == accounts.end() || !found->second.connected())
                return std::unexpected("Account disconnected; run /mucregister after reconnecting");
            auto &acc = found->second;
            const auto room_entry = acc.channels.find(room);
            if (room_entry == acc.channels.end() || room_entry->second.type != channel::chat_type::MUC)
                return std::unexpected("Room buffer closed; reopen it and run /mucregister");
            if (action != "submit" && action != "cancel")
                return std::unexpected("Unsupported room registration action");
            auto payload = ::xmpp::muc_registration_query(&answers, action == "cancel");
            if (!payload) return std::unexpected(payload.error());
            const auto id = stanza::uuid(acc.context);
            muc_owner_query_info pending{room, acc.buffer,
                action == "cancel" ? muc_owner_kind::register_cancel : muc_owner_kind::register_set};
            pending.editor_id = editor;
            acc.muc_owner_queries.emplace(id, std::move(pending));
            auto request = stanza::iq().type("set").to(room).id(id);
            request.child(*payload);
            acc.connection.send(request.build(acc.context).get());
            return {};
        });
    if (!opened) output->printf_error(opened.error());
    return true;
}
}
