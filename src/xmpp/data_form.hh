// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <string>
#include <expected>
#include <span>
#include <vector>

#include "stanza_view.hh"
#include "test_export.hh"
#include "node.hh"

namespace xmpp {

struct data_form_option {
    std::string label;
    std::string value;
};

struct data_form_field {
    std::string var;
    std::string label;
    std::string type = "text-single";
    std::string description;
    bool required = false;
    bool included = false;
    std::vector<std::string> values;
    std::vector<data_form_option> options;
    [[nodiscard]] bool editable() const { return type != "hidden" && type != "fixed"; }
};

struct data_form {
    std::string type;
    std::string title;
    std::vector<std::string> instructions;
    std::vector<data_form_field> fields;
    std::vector<data_form_field> reported;
    std::vector<std::vector<data_form_field>> items;
};

[[nodiscard]] XMPP_TEST_EXPORT auto parse_data_form(StanzaView form)
    -> std::expected<data_form, std::string>;
[[nodiscard]] XMPP_TEST_EXPORT auto set_data_form_values(
    data_form_field &field, std::span<const std::string> values)
    -> std::expected<void, std::string>;
[[nodiscard]] XMPP_TEST_EXPORT auto submit_data_form(const data_form &form)
    -> std::expected<stanza::xep0004::form, std::string>;
[[nodiscard]] XMPP_TEST_EXPORT auto data_form_lines(const data_form &form)
    -> std::vector<std::string>;

struct DataFormFieldInfo {
    std::string var;
    std::string label;
};

// Field var/label pairs from a jabber:x:data <x/> form (skips FORM_TYPE).
[[nodiscard]] XMPP_TEST_EXPORT std::vector<DataFormFieldInfo>
parse_data_form_fields(StanzaView xdata_form);

}  // namespace xmpp
