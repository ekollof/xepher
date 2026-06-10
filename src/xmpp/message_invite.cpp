// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "message_invite.hh"

#include <ranges>
#include <string_view>

#include <fmt/core.h>

#include "xmpp/node.hh"

namespace xmpp {

namespace {

[[nodiscard]] auto bare_jid_from(std::string_view full) -> std::string
{
    return ::jid(nullptr, std::string(full).c_str()).bare;
}

} // namespace

bool stanza_is_error_message(StanzaView msg)
{
    const auto type = msg.type();
    return type && *type == "error";
}

std::optional<DirectMucInvite> parse_direct_muc_invite(StanzaView msg)
{
    const StanzaView invite = msg.child("x", k_muc_invite_ns);
    if (!invite.valid())
        return std::nullopt;

    const std::string room_jid = invite.attr_string("jid");
    if (room_jid.empty())
        return std::nullopt;

    DirectMucInvite parsed;
    parsed.room_jid = room_jid;

    if (const std::string password = invite.attr_string("password"); !password.empty())
        parsed.password = password;

    if (const std::string reason = invite.attr_string("reason"); !reason.empty())
        parsed.reason = reason;

    if (const auto from = msg.from(); from && !from->empty())
        parsed.inviter_bare = bare_jid_from(*from);
    else
        parsed.inviter_bare = "unknown";

    return parsed;
}

std::optional<MediatedMucInvite> parse_mediated_muc_invite(StanzaView msg)
{
    const StanzaView muc_user = msg.child("x", k_muc_user_ns);
    if (!muc_user.valid())
        return std::nullopt;

    const StanzaView invite = muc_user.child("invite");
    if (!invite.valid())
        return std::nullopt;

    const auto from = msg.from();
    if (!from || from->empty())
        return std::nullopt;

    MediatedMucInvite parsed;
    parsed.room_jid = bare_jid_from(*from);
    if (parsed.room_jid.empty())
        return std::nullopt;

    if (const std::string inviter = invite.attr_string("from"); !inviter.empty())
        parsed.inviter_bare = bare_jid_from(inviter);

    if (const StanzaView reason_el = invite.child("reason"); reason_el.valid())
    {
        if (const std::string reason = reason_el.text(); !reason.empty())
            parsed.reason = reason;
    }

    if (const StanzaView password_el = muc_user.child("password"); password_el.valid())
    {
        if (const std::string password = password_el.text(); !password.empty())
            parsed.password = password;
    }

    return parsed;
}

std::optional<MediatedMucDecline> parse_mediated_muc_decline(StanzaView msg)
{
    const StanzaView muc_user = msg.child("x", k_muc_user_ns);
    if (!muc_user.valid())
        return std::nullopt;

    const StanzaView decline = muc_user.child("decline");
    if (!decline.valid())
        return std::nullopt;

    const auto from = msg.from();
    if (!from || from->empty())
        return std::nullopt;

    MediatedMucDecline parsed;
    parsed.room_jid = bare_jid_from(*from);
    if (parsed.room_jid.empty())
        return std::nullopt;

    if (const std::string decliner = decline.attr_string("from"); !decliner.empty())
        parsed.decliner_bare = bare_jid_from(decliner);

    if (const StanzaView reason_el = decline.child("reason"); reason_el.valid())
    {
        if (const std::string reason = reason_el.text(); !reason.empty())
            parsed.reason = reason;
    }

    return parsed;
}

MucInviteNotification render_direct_muc_invite_notification(const DirectMucInvite& invite)
{
    MucInviteNotification out;
    const auto reason_suffix = invite.reason
        ? fmt::format(" ({})", *invite.reason) : std::string{};
    const auto join_args = invite.password
        ? fmt::format(" {}", *invite.password) : std::string{};
    out.network_lines.push_back(
        fmt::format("{} invited you to {}{}",
                    invite.inviter_bare, invite.room_jid, reason_suffix));
    out.network_lines.push_back(
        fmt::format("To join: /join {}{}", invite.room_jid, join_args));
    return out;
}

MucInviteNotification render_mediated_muc_invite_notification(const MediatedMucInvite& invite)
{
    MucInviteNotification out;
    const std::string inviter = invite.inviter_bare
        ? *invite.inviter_bare
        : invite.room_jid;
    const auto reason_suffix = invite.reason
        ? fmt::format(" ({})", *invite.reason) : std::string{};
    const auto join_args = invite.password
        ? fmt::format(" {}", *invite.password) : std::string{};
    out.network_lines.push_back(
        fmt::format("{} invited you to {}{}",
                    inviter, invite.room_jid, reason_suffix));
    out.network_lines.push_back(
        fmt::format("To join: /join {}{}", invite.room_jid, join_args));
    out.network_lines.push_back("To decline: /decline [reason]");
    return out;
}

std::string render_mediated_muc_decline_notification(const MediatedMucDecline& decline)
{
    const std::string_view who = decline.decliner_bare
        ? std::string_view{*decline.decliner_bare}
        : std::string_view{decline.room_jid};
    const auto reason_suffix = decline.reason
        ? fmt::format(" ({})", *decline.reason) : std::string{};
    return fmt::format("{} declined your invitation to {}{}",
                       who, decline.room_jid, reason_suffix);
}

std::vector<MucAdminListItem> parse_muc_admin_list_items(StanzaView admin_query)
{
    if (!admin_query.valid())
        return {};

    std::vector<MucAdminListItem> items;
    for (const auto item : admin_query)
    {
        if (item.name() != "item")
            continue;
        MucAdminListItem parsed;
        parsed.jid = item.attr_string("jid");
        parsed.nick = item.attr_string("nick");
        parsed.affiliation = item.attr_string("affiliation");
        items.push_back(std::move(parsed));
    }
    return items;
}

std::vector<MucRegisterFormField> parse_muc_register_form_fields(StanzaView xdata_form)
{
    if (!xdata_form.valid())
        return {};

    std::vector<MucRegisterFormField> fields;
    for (const auto field : xdata_form)
    {
        if (field.name() != "field")
            continue;
        const std::string var = field.attr_string("var");
        if (var.empty() || var == "FORM_TYPE")
            continue;
        MucRegisterFormField parsed;
        parsed.var = var;
        parsed.label = field.attr_string("label");
        parsed.type = field.attr_string("type");
        for (const auto value_el : field)
        {
            if (value_el.name() == "value")
                parsed.value = value_el.text();
        }
        fields.push_back(std::move(parsed));
    }
    return fields;
}

}  // namespace xmpp