// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "xmpp/presence.hh"

#include <string>
#include <string_view>

#include "util.hh"
#include "xmpp/node.hh"

xml::presence::~presence() = default;
xml::message::~message() = default;
xml::iq::~iq() = default;

xml::xep0045::~xep0045() = default;
xml::xep0045::x::~x() = default;
xml::xep0045::x::item::~item() = default;

namespace xmpp {

namespace {

inline constexpr std::string_view k_muc_user_ns = "http://jabber.org/protocol/muc#user";
inline constexpr std::string_view k_muc_ns = "http://jabber.org/protocol/muc";
inline constexpr std::string_view k_caps_ns = "http://jabber.org/protocol/caps";
inline constexpr std::string_view k_signed_ns = "jabber:x:signed";
inline constexpr std::string_view k_idle_ns = "urn:xmpp:idle:1";
inline constexpr std::string_view k_stanzas_ns = "urn:ietf:params:xml:ns:xmpp-stanzas";

[[nodiscard]] std::optional<std::string> child_text(StanzaView parent, std::string_view name)
{
    const StanzaView child = parent.child(name);
    if (!child.valid())
        return std::nullopt;
    const std::string text = child.text();
    return text.empty() ? std::nullopt : std::optional<std::string>(text);
}

[[nodiscard]] MucUserItem parse_muc_item(StanzaView item)
{
    MucUserItem out;
    const std::string affiliation = item.attr_string("affiliation");
    const std::string role = item.attr_string("role");
    const std::string nick = item.attr_string("nick");
    const std::string jid = item.attr_string("jid");
    if (!affiliation.empty())
        out.affiliation = affiliation;
    if (!role.empty())
        out.role = role;
    if (!nick.empty())
        out.nick = nick;
    if (!jid.empty())
        out.real_jid = jid;
    out.reason = item.child("reason").text();
    return out;
}

[[nodiscard]] std::optional<MucPresenceExtension> parse_muc_user(StanzaView muc_user)
{
    if (!muc_user.valid())
        return std::nullopt;

    MucPresenceExtension ext;
    for (const StanzaView status_el : muc_user)
    {
        if (status_el.name() != "status")
            continue;
        if (const auto code = parse_int64(status_el.attr_string("code")); code)
            ext.statuses.push_back(static_cast<int>(*code));
    }

    for (const StanzaView item_el : muc_user)
    {
        if (item_el.name() == "item")
            ext.items.push_back(parse_muc_item(item_el));
    }

    return ext;
}

[[nodiscard]] std::optional<PresenceError> parse_presence_error(StanzaView error_elem)
{
    if (!error_elem.valid())
        return std::nullopt;

    PresenceError err;
    err.reason = muc_presence_error_reason(error_elem);
    const std::string desc = error_elem.child("text").text();
    if (!desc.empty())
        err.description = desc;
    return err;
}

}  // namespace

std::string presence_display_name(
    const std::string_view bare,
    const std::string_view resource,
    const std::string_view full,
    const std::string_view channel_id)
{
    if (!channel_id.empty()
        && bare == channel_id
        && !resource.empty())
        return std::string(resource);
    if (!full.empty())
        return std::string(full);
    return std::string(bare);
}

ParsedJid parse_jid_parts(std::string_view full_jid)
{
    const jid j(nullptr, std::string(full_jid));
    return ParsedJid {
        .full = j.full,
        .bare = j.bare,
        .resource = j.resource,
    };
}

std::string muc_presence_error_reason(StanzaView error_elem)
{
    if (!error_elem.valid())
        return "Unspecified";

    const auto has_cond = [&](std::string_view name) {
        return error_elem.child(name, k_stanzas_ns).valid();
    };

    if (has_cond("not-authorized"))
        return "Password Required";
    if (has_cond("forbidden"))
        return "Banned";
    if (has_cond("item-not-found"))
        return "No such MUC";
    if (has_cond("not-allowed"))
        return "MUC Creation Failed";
    if (has_cond("not-acceptable"))
        return "Unacceptable Nickname";
    if (has_cond("registration-required"))
        return "Not on Member List";
    if (has_cond("conflict"))
        return "Nickname Conflict";
    if (has_cond("service-unavailable"))
        return "Service Unavailable (MUC Full?)";

    return "Unspecified";
}

ParsedPresence parse_presence(StanzaView view)
{
    ParsedPresence pres;

    if (const auto from = view.from(); from && !from->empty())
        pres.from = parse_jid_parts(*from);
    if (const auto to = view.to(); to && !to->empty())
        pres.to = parse_jid_parts(*to);
    if (const auto type = view.type(); type && !type->empty())
        pres.type = std::string(*type);

    pres.show = child_text(view, "show");
    pres.status = child_text(view, "status");

    const StanzaView idle_el = view.child("idle", k_idle_ns);
    if (idle_el.valid())
    {
        const std::string since = idle_el.attr_string("since");
        if (!since.empty())
        {
            try {
                pres.idle_since = get_time(since);
            } catch (const std::invalid_argument &) {
            }
        }
    }

    const StanzaView signed_el = view.child("x", k_signed_ns);
    if (signed_el.valid())
    {
        const std::string sig = signed_el.text();
        if (!sig.empty())
            pres.signature = sig;
    }

    const StanzaView caps_el = view.child("c", k_caps_ns);
    if (caps_el.valid())
    {
        PresenceCaps caps;
        caps.node = caps_el.attr_string("node");
        caps.verification = caps_el.attr_string("ver");
        if (!caps.node.empty() && !caps.verification.empty())
            pres.caps = std::move(caps);
    }

    pres.has_muc = view.child("x", k_muc_ns).valid();

    const StanzaView muc_user_el = view.child("x", k_muc_user_ns);
    if (muc_user_el.valid())
        pres.muc_user = parse_muc_user(muc_user_el);

    const StanzaView error_el = view.child("error");
    if (error_el.valid())
        pres.error = parse_presence_error(error_el);

    return pres;
}

}  // namespace xmpp