// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <vector>
#include <string>

#include "node.hh"
#pragma GCC visibility push(default)
#include "ns.hh"
#pragma GCC visibility pop

namespace stanza {

    /* Room Activity Indicators (XEP-0437) */
    struct xep0437 {

        // <activity>room@service</activity>
        struct activity : virtual public spec {
            activity(std::string_view room_jid) : spec("activity") {
                text(room_jid);
            }
        };

        // <rai xmlns='urn:xmpp:rai:0'> ... </rai>
        struct rai : virtual public spec {
            rai() : spec("rai") {
                xmlns<urn::xmpp::rai::_0>();
            }

            rai& add_activity(std::string_view room_jid) {
                xep0437::activity a(room_jid);
                child(a);
                return *this;
            }
        };

        // Presence mixin: subscribe to RAI for a MUC service
        struct presence : virtual public spec {
            presence() : spec("presence") {}
            presence& rai_indicator() {
                xep0437::rai r;
                child(r);
                return *this;
            }
        };
    };

}

namespace xml {

    /* Room Activity Indicators parse layer (XEP-0437) */
    class xep0437 : virtual public node {
    private:
        std::optional<std::optional<std::vector<std::string>>> _activities;
    public:
        std::optional<std::vector<std::string>>& activities() {
            if (!_activities) {
                auto rai_children = get_children<urn::xmpp::rai::_0>("rai");
                if (!rai_children.empty()) {
                    std::vector<std::string> result;
                    for (auto& rai_node : rai_children) {
                        for (auto& act : rai_node.get().get_children("activity")) {
                            if (auto t = act.get().get_attr("jid")) {
                                result.push_back(*t);
                            } else if (!act.get().text.empty()) {
                                result.push_back(act.get().text);
                            }
                        }
                    }
                    _activities = std::move(result);
                } else {
                    _activities.emplace(std::nullopt);
                }
            }
            return *_activities;
        }
    };

}
