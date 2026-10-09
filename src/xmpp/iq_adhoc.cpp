// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
#include "xmpp/iq_adhoc.hh"

#include <algorithm>
#include <array>
#include <ranges>
#include "xmpp/ns.hh"
#include "xmpp/xep-0004.inl"

namespace xmpp {
auto parse_adhoc_arguments(std::string_view input)
    -> std::expected<std::vector<std::string>, std::string>
{
    std::vector<std::string> arguments;
    std::string token;
    char quote = 0;
    bool escaped = false;
    bool started = false;
    std::ranges::for_each(input, [&](char c) {
        if (escaped)
        {
            token.push_back(c);
            escaped = false;
        }
        else if (c == '\\' && quote != '\'')
        {
            escaped = true;
            started = true;
        }
        else if (quote)
        {
            if (c == quote) quote = 0;
            else token.push_back(c);
        }
        else if (c == '\'' || c == '"')
        {
            quote = c;
            started = true;
        }
        else if (c == ' ' || c == '\t' || c == '\n')
        {
            if (started)
            {
                arguments.emplace_back(std::move(token));
                token.clear();
                started = false;
            }
        }
        else
        {
            token.push_back(c);
            started = true;
        }
    });
    if (quote || escaped)
        return std::unexpected("unterminated quote or escape in form arguments");
    if (started)
        arguments.emplace_back(std::move(token));
    return arguments;
}

auto parse_adhoc_session(StanzaView command) -> std::expected<AdhocSession, std::string>
{
    AdhocSession session;
    session.actions.emplace_back("cancel");
    const auto actions = command.child("actions", "http://jabber.org/protocol/commands");
    session.default_action = actions.valid() ? actions.attr_string("execute") : "complete";
    if (session.default_action.empty())
        session.default_action = "next";
    if (!actions.valid())
        session.actions.emplace_back("complete");
    else
        std::ranges::for_each(actions, [&](StanzaView action) {
            const auto name = action.name();
            if (name == "next" || name == "prev" || name == "complete")
                session.actions.emplace_back(name);
        });
    if (std::ranges::find(session.actions, session.default_action) == session.actions.end()
        || session.default_action == "cancel")
        return std::unexpected("server returned an invalid default command action");
    const auto form = command.child("x", "jabber:x:data");
    session.has_form = form.valid() && form.attr_string("type") == "form";
    std::ranges::for_each(form, [&](StanzaView field) {
        if (field.name() != "field" || field.attr_string("type") != "hidden")
            return;
        const auto name = field.attr_string("var");
        if (name.empty())
            return;
        auto &values = session.hidden_fields[name];
        std::ranges::for_each(field, [&](StanzaView value) {
            if (value.name() == "value")
                values.emplace_back(value.text());
        });
    });
    return session;
}

auto make_adhoc_command(std::string_view node, std::string_view session_id,
    std::string_view action, std::span<const std::string_view> fields,
    const AdhocSession *session, const stanza::xep0004::form *submission)
    -> std::expected<stanza::spec, std::string>
{
    if (node.empty())
        return std::unexpected("a command node is required");
    if (action.empty() || action == "execute")
        action = session ? std::string_view(session->default_action) : "execute";
    constexpr std::array<std::string_view, 5> valid_actions{
        "execute", "next", "prev", "complete", "cancel"};
    if (std::ranges::find(valid_actions, action) == valid_actions.end())
        return std::unexpected("action must be execute, next, prev, complete, or cancel");
    if (session_id.empty() && action != "execute")
        return std::unexpected("continuation actions require a session ID");
    if (session && std::ranges::find(session->actions, action) == session->actions.end())
        return std::unexpected("action is not allowed at this command stage");
    if (action == "cancel" && !fields.empty())
        return std::unexpected("cancel does not accept form fields");
    if (submission && (!fields.empty() || action == "cancel"))
        return std::unexpected("explicit forms cannot be mixed with fields or cancellation");

    auto values = session ? session->hidden_fields
                         : std::map<std::string, std::vector<std::string>>{};
    const auto malformed = std::ranges::find_if(fields, [](std::string_view field) {
        const auto equal = field.find('=');
        return equal == std::string_view::npos || equal == 0;
    });
    if (malformed != fields.end())
        return std::unexpected("form arguments must be nonempty-name=value pairs");
    if (session && std::ranges::any_of(fields, [&](std::string_view field) {
            return session->hidden_fields.contains(std::string(field.substr(0, field.find('='))));
        }))
        return std::unexpected("hidden form fields cannot be changed");
    std::ranges::for_each(fields, [&](std::string_view field) {
        const auto equal = field.find('=');
        values[std::string(field.substr(0, equal))].emplace_back(field.substr(equal + 1));
    });

    struct command_spec : stanza::spec {
        command_spec(std::string_view node, std::string_view session_id,
                     std::string_view action) : spec("command") {
            xmlns<jabber_org::protocol::commands>();
            attr("node", node);
            attr("action", action);
            if (!session_id.empty())
                attr("sessionid", session_id);
        }
    } command(node, session_id, action);
    // Cancellation contains no payload, including cached hidden fields.
    if (submission) {
        auto form = *submission;
        command.child(form);
    }
    else if (action != "cancel" && (!values.empty() || !fields.empty()
        || (session && session->has_form)))
    {
        stanza::xep0004::form form("submit");
        std::ranges::for_each(values, [&](const auto &entry) {
            const auto &[name, field_values] = entry;
            stanza::xep0004::field field(name);
            if (session && session->hidden_fields.contains(name))
                field.type("hidden");
            std::ranges::for_each(field_values, [&](std::string_view value) { field.value(value); });
            form.add_field(field);
        });
        command.child(form);
    }
    return command;
}
}
