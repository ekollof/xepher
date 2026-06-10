// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <fmt/core.h>

#include "../util.hh"
#include "node.hh"
#pragma GCC visibility push(default)
#include "ns.hh"
#pragma GCC visibility pop

namespace xml {

    /* Multi-User Chat */
    class xep0045 : virtual public node {
    public:
        virtual ~xep0045();
        enum class affiliation {
            admin,
            member,
            none,
            outcast,
            owner,
        };

        enum class role {
            moderator,
            none,
            participant,
            visitor,
        };

        static affiliation parse_affiliation(std::string_view s) {
            if (s == "admin")
                return affiliation::admin;
            else if (s == "member")
                return affiliation::member;
            else if (s == "none")
                return affiliation::none;
            else if (s == "outcast")
                return affiliation::outcast;
            else if (s == "owner")
                return affiliation::owner;
            throw std::invalid_argument(
                fmt::format("Bad affiliation: {}", s));
        }

        static std::string_view format_affiliation(affiliation e) {
            switch (e) {
            case affiliation::admin:
                return "admin";
            case affiliation::member:
                return "member";
            case affiliation::none:
                return "none";
            case affiliation::outcast:
                return "outcast";
            case affiliation::owner:
                return "owner";
            default:
                return "";
            }
        }

        static role parse_role(std::string_view s) {
            if (s == "moderator")
                return role::moderator;
            else if (s == "none")
                return role::none;
            else if (s == "participant")
                return role::participant;
            else if (s == "visitor")
                return role::visitor;
            throw std::invalid_argument(
                fmt::format("Bad role: {}", s));
        }

        static std::string_view format_role(role e) {
            switch (e) {
            case role::moderator:
                return "moderator";
            case role::none:
                return "none";
            case role::participant:
                return "participant";
            case role::visitor:
                return "visitor";
            default:
                return "";
            }
        }

        class x {
        public:
            ~x();
        private:
            struct decline {
                decline(node& node) {
                    for (auto& child : node.get_children("reason"))
                        reason += child.get().text;
                    if (auto attr = node.get_attr("from"))
                        from = jid(node.context, *attr);
                    if (auto attr = node.get_attr("to"))
                        to = jid(node.context, *attr);
                };

                std::string reason;
                std::optional<jid> from;
                std::optional<jid> to;
            };

            struct destroy {
                destroy(node& node) {
                    for (auto& child : node.get_children("reason"))
                        reason += child.get().text;
                    // XEP-0045 §10.9: the successor room attribute is "jid", not "target"
                    if (auto attr = node.get_attr("jid"))
                        jid_ = jid(node.context, *attr);
                };

                std::string reason;
                std::optional<jid> jid_;
            };

            struct invite {
                invite(node& node) {
                    for (auto& child : node.get_children("reason"))
                        reason += child.get().text;
                    if (auto attr = node.get_attr("from"))
                        from = jid(node.context, *attr);
                    if (auto attr = node.get_attr("to"))
                        to = jid(node.context, *attr);
                };

                std::string reason;
                std::optional<jid> from;
                std::optional<jid> to;
            };

            class item {
            public:
                ~item();
            private:
                struct actor {
                    actor(node& node) {
                        for (auto& child : node.get_children("reason"))
                            reason += child.get().text;
                        if (auto attr = node.get_attr("jid"))
                            target = jid(node.context, *attr);
                        if (auto attr = node.get_attr("nick"))
                            nick = *attr;
                    }

                    std::string reason;
                    std::optional<jid> target;
                    std::string nick;
                };

                struct continue_ {
                    continue_(node& node) {
                        if (auto attr = node.get_attr("thread"))
                            thread = *attr;
                    }

                    std::string thread;
                };

            public:
                item(node& node) {
                    for (auto& child : node.get_children("reason"))
                        reason += child.get().text;
                    if (auto attr = node.get_attr("affiliation"))
                        affiliation = parse_affiliation(*attr);
                    if (auto attr = node.get_attr("jid"))
                        target = jid(node.context, *attr);
                    if (auto attr = node.get_attr("nick"))
                        nick = *attr;
                    if (auto attr = node.get_attr("role"))
                        role = parse_role(*attr);
                };

                std::unique_ptr<std::vector<std::unique_ptr<actor>>> actors;
                std::unique_ptr<std::vector<std::unique_ptr<continue_>>> continues;
                std::string reason;
                std::optional<enum affiliation> affiliation;
                std::optional<jid> target;
                std::optional<std::string> nick;
                std::optional<enum role> role;
            };

        public:
            x(node& node) {
                for (auto& child : node.get_children("item"))
                    items.push_back(std::make_unique<item>(child));
                for (auto& child : node.get_children("status"))
                    if (auto code = child.get().get_attr("code"))
                        if (auto n = parse_int64(*code); n)
                            statuses.push_back(static_cast<int>(*n));
            }

            std::unique_ptr<std::vector<decline>> declines;
            std::unique_ptr<std::vector<destroy>> destroys;
            std::unique_ptr<std::vector<invite>> invites;
            std::vector<std::unique_ptr<item>> items;
            std::unique_ptr<std::vector<std::string>> passwords;
            std::vector<int> statuses;
        };

        class error { // THIS IS RFC 6120 :(
        private:
            enum condition {
                not_authorized,
                registration_required,
                forbidden,
                not_allowed,
                not_acceptable,
                conflict,
                service_unavailable,
                item_not_found
            };

            enum class action {
                auth,
                cancel,
                continue_,
                modify,
                wait
            };

            static action parse_action(std::string_view s) {
                if (s == "auth")
                    return action::auth;
                else if (s == "cancel")
                    return action::cancel;
                else if (s == "continue")
                    return action::continue_;
                else if (s == "modify")
                    return action::modify;
                else if (s == "wait")
                    return action::wait;
                throw std::invalid_argument(
                    fmt::format("Bad action: {}", s));
            }

            static std::string_view format_action(action e) {
                switch (e) {
                case action::auth:
                    return "auth";
                case action::cancel:
                    return "cancel";
                case action::continue_:
                    return "continue";
                case action::modify:
                    return "modify";
                case action::wait:
                    return "wait";
                default:
                    return "";
                }
            }

        public:
            error(node& node) {
                if (auto attr = node.get_attr("by"))
                    by = jid(node.context, *attr);
                if (auto attr = node.get_attr("type"))
                    type = parse_action(*attr);

                using xmpp_stanzas = urn::ietf::params::xml::ns::xmpp_stanzas;

                if (node.get_children<xmpp_stanzas>("not-authorized").size() > 0)
                    condition = not_authorized;
                if (node.get_children<xmpp_stanzas>("forbidden").size() > 0)
                    condition = forbidden;
                if (node.get_children<xmpp_stanzas>("item-not-found").size() > 0)
                    condition = item_not_found;
                if (node.get_children<xmpp_stanzas>("not-allowed").size() > 0)
                    condition = not_allowed;
                if (node.get_children<xmpp_stanzas>("not-acceptable").size() > 0)
                    condition = not_acceptable;
                if (node.get_children<xmpp_stanzas>("registration-required").size() > 0)
                    condition = registration_required;
                if (node.get_children<xmpp_stanzas>("conflict").size() > 0)
                    condition = conflict;
                if (node.get_children<xmpp_stanzas>("service-unavailable").size() > 0)
                    condition = service_unavailable;

                for (auto& child : node.get_children("text"))
                    description = child.get().text;
            }

            std::optional<jid> by;
            std::optional<enum action> type;
            std::optional<enum condition> condition;
            std::optional<std::string> description;

            const char* reason() {
                if (condition)
                    switch (*condition)
                    {
                      case not_authorized:
                        return "Password Required";
                      case forbidden:
                        return "Banned";
                      case item_not_found:
                        return "No such MUC";
                      case not_allowed:
                        return "MUC Creation Failed";
                      case not_acceptable:
                        return "Unacceptable Nickname";
                      case registration_required:
                        return "Not on Member List";
                      case conflict:
                        return "Nickname Conflict";
                      case service_unavailable:
                        return "Service Unavailable (MUC Full?)";
                    }
                return "Unspecified";
            }
        };

    private:
        std::optional<bool> _muc;
        bool _muc_user_parsed = false;
        std::optional<x> _muc_user;
        bool _error_parsed = false;
        std::optional<error> _error;
    public:
        bool muc() {
            if (!_muc)
            {
                auto child = get_children<jabber_org::protocol::muc>("x");
                _muc = child.size() > 0;
            }
            return *_muc;
        }

        std::optional<x>& muc_user() {
            if (!_muc_user_parsed)
            {
                _muc_user_parsed = true;
                auto child = get_children<jabber_org::protocol::muc::user>("x");
                if (child.size() > 0)
                    _muc_user.emplace(child.front().get());
            }
            return _muc_user;
        }

        std::optional<error>& error() {
            if (!_error_parsed)
            {
                _error_parsed = true;
                auto child = get_children("error");
                if (child.size() > 0)
                    _error.emplace(child.front().get());
            }
            return _error;
        }
    };

}

namespace stanza {

    /* Multi-User Chat (XEP-0045) — stanza builder side */
    struct xep0045 {

        // <x xmlns='http://jabber.org/protocol/muc'>
        //   <history maxstanzas='0'/>  — suppress server history (MAM handles catch-up)
        //   <password>...</password>     — optional, sent when the room is
        //     password-protected (XEP-0045 §7.1.4). The plugin receives a
        //     <not-authorized/> error and a 401 status code in muc#user if
        //     the password is wrong or missing.
        // </x>
        struct join_x : virtual public spec {
            join_x() : spec("x") {
                xmlns<jabber_org::protocol::muc>();
                // XEP-0045 §7.1.6: request no server history so MAM catch-up is the
                // sole source of history, avoiding duplicates.
                struct history_elem : virtual public spec {
                    history_elem() : spec("history") {
                        attr("maxstanzas", "0");
                    }
                };
                history_elem h;
                child(h);
            }
            // XEP-0045 §7.1.4: optional room password for protected rooms.
            // Builder chain: .password("hunter2") before .child / .build.
            join_x& password(std::string_view p) {
                struct p_el : virtual public spec {
                    p_el(std::string_view s) : spec("password") { text(s); }
                } pe(p);
                child(pe);
                return *this;
            }
        };

        // <invite to='user@host'><reason>…</reason></invite> (XEP-0045 §7.8.2)
        struct invite_to : virtual public spec {
            explicit invite_to(std::string_view to_jid) : spec("invite") {
                attr("to", to_jid);
            }
            invite_to& reason(std::string_view r) {
                struct r_el : virtual public spec {
                    r_el(std::string_view t) : spec("reason") { text(t); }
                } re(r);
                child(re);
                return *this;
            }
        };

        // <x xmlns='http://jabber.org/protocol/muc#user'/>
        struct muc_user_x : virtual public spec {
            muc_user_x() : spec("x") {
                xmlns<jabber_org::protocol::muc::user>();
            }
            muc_user_x& invite(invite_to& inv) { child(inv); return *this; }
        };

        // stanza::presence mixin — adds a <x xmlns='...muc'><history maxstanzas='0'/></x>
        // child for room joining (XEP-0045 §7.1). Pass an optional password
        // (XEP-0045 §7.1.4) for protected rooms.
        struct presence : virtual public spec {
            presence() : spec("presence") {}

            presence& muc_join() {
                xep0045::join_x x;
                child(x);
                return *this;
            }
            presence& muc_join(std::string_view room_password) {
                xep0045::join_x x;
                if (!room_password.empty())
                    x.password(room_password);
                child(x);
                return *this;
            }
        };

        // stanza::message mixin
        struct message : virtual public spec {
            message() : spec("message") {}

            message& muc_user(xep0045::muc_user_x x) { child(x); return *this; }

            // XEP-0045 §7.8.2: mediated room invitation (message to room@service).
            message& mediated_invite(std::string_view to_jid, std::string_view invite_reason = {}) {
                xep0045::muc_user_x ux;
                xep0045::invite_to inv(to_jid);
                if (!invite_reason.empty())
                    inv.reason(invite_reason);
                ux.invite(inv);
                child(ux);
                return *this;
            }
        };

        // XEP-0045 §10.2/§10.7: room owner actions via the muc#owner namespace.
        // The query wrapper is the IQ payload for both reading the config form
        // (get) and submitting modified values back (set). The destroy payload
        // is used to destroy a room entirely (XEP-0045 §10.7).
        struct xep0045owner {
            // Forward declaration; defined below.
            struct destroy_payload;

            // <query xmlns='http://jabber.org/protocol/muc#owner'>...</query>
            struct query : virtual public spec {
                query() : spec("query") {
                    xmlns<jabber_org::protocol::muc::owner>();
                }
                // Wrap a jabber:x:data form (<x>) as the query child.
                query& form(xep0004::form& f) { child(f); return *this; }
                // Wrap a <destroy/> payload for room destruction.
                query& destroy(destroy_payload& d) { child(d); return *this; }
            };

            // <destroy jid='alt@service'><reason>...</reason><password>...</password></destroy>
            struct destroy_payload : virtual public spec {
                destroy_payload() : spec("destroy") {}
                destroy_payload& jid(std::string_view j) { attr("jid", j); return *this; }
                destroy_payload& reason(std::string_view r) {
                    struct r_el : virtual public spec {
                        r_el(std::string_view t) : spec("reason") { text(t); }
                    } re(r);
                    child(re);
                    return *this;
                }
                destroy_payload& password(std::string_view p) {
                    struct p_el : virtual public spec {
                        p_el(std::string_view t) : spec("password") { text(t); }
                    } pe(p);
                    child(pe);
                    return *this;
                }
            };

            // stanza::iq mixin
            struct iq : virtual public spec {
                iq() : spec("iq") {}

                iq& muc_owner(query& q) { child(q); return *this; }
            };
        };
    };

}
