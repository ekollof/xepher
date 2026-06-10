// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "data_form.hh"

namespace xmpp {

std::vector<DataFormFieldInfo> parse_data_form_fields(StanzaView xdata_form)
{
    if (!xdata_form.valid())
        return {};

    std::vector<DataFormFieldInfo> fields;
    for (const auto field : xdata_form)
    {
        if (field.name() != "field")
            continue;
        const std::string var = field.attr_string("var");
        if (var.empty() || var == "FORM_TYPE")
            continue;
        DataFormFieldInfo parsed;
        parsed.var = var;
        parsed.label = field.attr_string("label");
        fields.push_back(std::move(parsed));
    }
    return fields;
}

}  // namespace xmpp