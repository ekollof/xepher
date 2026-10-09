// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "account.hh"
#include "ui/form_editor.hh"

namespace weechat {
auto account::edit_adhoc_form(std::string_view target, std::string_view node,
    std::string_view session_id, ::xmpp::data_form form, const ::xmpp::AdhocSession &session)
    -> std::expected<void, std::string>
{
    const auto id = ui::adhoc_form_id(name, target, node, session_id);
    return ui::form_editor::open(name, id, fmt::format("{} — {}", target, node),
        std::move(form), session.actions, session.default_action,
        [owner = name, target = std::string(target), node = std::string(node), session_id = std::string(session_id)]
        (std::string_view action, const ::xmpp::data_form &answers) -> std::expected<void, std::string> {
            const auto found = accounts.find(owner);
            if (found == accounts.end() || !found->second.connected())
                return std::unexpected("Account disconnected; restart the command after reconnecting");
            auto &acc = found->second;
            const auto stage = acc.adhoc_sessions.find({target, node, session_id});
            if (stage == acc.adhoc_sessions.end())
                return std::unexpected("This command session has ended");
            auto control = stage->second;
            std::optional<stanza::xep0004::form> submission;
            if (action == "prev") {
                control.has_form = false;
                control.hidden_fields.clear();
            }
            else if (action != "cancel") {
                auto form = ::xmpp::submit_data_form(answers);
                if (!form) return std::unexpected(form.error());
                submission.emplace(std::move(*form));
            }
            auto command = ::xmpp::make_adhoc_command(node, session_id, action, {},
                &control, submission ? &*submission : nullptr);
            if (!command) return std::unexpected(command.error());
            const auto request_id = stanza::uuid(acc.context);
            account::adhoc_query_info pending;
            pending.target_jid = target;
            pending.node = node;
            pending.session_id = session_id;
            pending.buffer = acc.buffer;
            acc.adhoc_queries.emplace(request_id, std::move(pending));
            auto request = stanza::iq().type("set").to(target).id(request_id);
            request.child(*command);
            acc.connection.send(request.build(acc.context).get());
            return {};
        });
}
}
