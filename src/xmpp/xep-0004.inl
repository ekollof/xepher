// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "node.hh"
#pragma GCC visibility push(default)
#include "ns.hh"
#pragma GCC visibility pop

namespace stanza {

    /* Data Forms (XEP-0004) */
    struct xep0004 {

        // <field var='...' type='...'><value>...</value></field>
        struct field : virtual public spec {
            field(std::string_view var_name) : spec("field") {
                attr("var", var_name);
            }

            field& type(std::string_view t) { attr("type", t); return *this; }
            field& value(std::string_view v) {
                struct value_el : virtual public spec {
                    value_el(std::string_view s) : spec("value") { text(s); }
                };
                value_el ve(v);
                child(ve);
                return *this;
            }
        };

        // Convenience: hidden field (FORM_TYPE etc)
        struct hidden_field : virtual public spec {
            hidden_field(std::string_view var_name, std::string_view val)
                : spec("field")
            {
                attr("var", var_name);
                attr("type", "hidden");
                struct value_el : virtual public spec {
                    value_el(std::string_view s) : spec("value") { text(s); }
                };
                value_el v(val);
                child(v);
            }
        };

        // Convenience: text-single field (default type)
        struct text_field : virtual public spec {
            text_field(std::string_view var_name, std::string_view val)
                : spec("field")
            {
                attr("var", var_name);
                struct value_el : virtual public spec {
                    value_el(std::string_view s) : spec("value") { text(s); }
                };
                value_el v(val);
                child(v);
            }
        };

        // <x xmlns='jabber:x:data' type='...'> ... </x>
        struct form : virtual public spec {
            form(std::string_view type_attr) : spec("x") {
                xmlns<jabber::x::data>();
                attr("type", type_attr);
            }

            form& title(std::string_view s) {
                struct title_el : virtual public spec {
                    title_el(std::string_view t) : spec("title") { text(t); }
                };
                title_el t(s);
                child(t);
                return *this;
            }

            form& instructions(std::string_view s) {
                struct inst_el : virtual public spec {
                    inst_el(std::string_view t) : spec("instructions") { text(t); }
                };
                inst_el i(s);
                child(i);
                return *this;
            }

            form& add_field(const field& f) {
                child(const_cast<field&>(f));
                return *this;
            }
            form& add_hidden(std::string_view var, std::string_view val) {
                hidden_field hf(var, val);
                child(hf);
                return *this;
            }
            form& add_text(std::string_view var, std::string_view val) {
                text_field tf(var, val);
                child(tf);
                return *this;
            }
        };
    };

}
