// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <optional>
#include <string>

#include "node.hh"
#pragma GCC visibility push(default)
#include "ns.hh"
#pragma GCC visibility pop

namespace stanza {

    /* Result Set Management (XEP-0059) */
    struct xep0059 {
        // <set xmlns='http://jabber.org/protocol/rsm'>
        //   <max>N</max>
        //   <before/>  or  <before>uid</before>
        //   <after>uid</after>
        // </set>
        struct set : virtual public spec {
            set() : spec("set") {
                xmlns<jabber_org::protocol::rsm>();
            }

            // Limit results to N items
            set& max(unsigned n) {
                struct max_el : virtual public spec {
                    max_el(std::string v) : spec("max") { text(v); }
                };
                max_el el(std::to_string(n));
                child(el);
                return *this;
            }

            // Page backward: <before/> (last page) or <before>uid</before>
            set& before(std::optional<std::string> uid = std::nullopt) {
                struct before_el : virtual public spec {
                    before_el(std::optional<std::string> v) : spec("before") {
                        if (v) text(*v);
                    }
                };
                before_el el(uid);
                child(el);
                return *this;
            }

            // Page forward: <after>uid</after>
            set& after(std::string_view uid) {
                struct after_el : virtual public spec {
                    after_el(std::string_view v) : spec("after") { text(v); }
                };
                after_el el(uid);
                child(el);
                return *this;
            }

            // Positional: <index>N</index>
            set& index(unsigned n) {
                struct index_el : virtual public spec {
                    index_el(std::string v) : spec("index") { text(v); }
                };
                index_el el(std::to_string(n));
                child(el);
                return *this;
            }
        };
    };

}

namespace xml {

    /* Result Set Management parse layer */
    class xep0059 : virtual public node {
    public:
        struct set {
            set(node& n) {
                for (auto& ch : n.get_children("max"))
                    max = ch.get().text;
                for (auto& ch : n.get_children("count"))
                    count = ch.get().text;
                for (auto& ch : n.get_children("first"))
                    first = ch.get().text;
                for (auto& ch : n.get_children("last"))
                    last = ch.get().text;
                for (auto& ch : n.get_children("before"))
                    before = ch.get().text;
                for (auto& ch : n.get_children("after"))
                    after = ch.get().text;
            }

            std::optional<std::string> max;
            std::optional<std::string> count;
            std::optional<std::string> first;
            std::optional<std::string> last;
            std::optional<std::string> before;
            std::optional<std::string> after;
        };

    private:
        std::optional<std::optional<set>> _rsm;
    public:
        std::optional<set>& rsm() {
            if (!_rsm) {
                auto children = get_children<jabber_org::protocol::rsm>("set");
                if (!children.empty())
                    _rsm = set(children.front().get());
                else
                    _rsm.emplace(std::nullopt);
            }
            return *_rsm;
        }
    };

}
