// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <string>
#include <vector>

#include "stanza_view.hh"
#include "test_export.hh"

namespace xmpp {

struct DataFormFieldInfo {
    std::string var;
    std::string label;
};

// Field var/label pairs from a jabber:x:data <x/> form (skips FORM_TYPE).
[[nodiscard]] XMPP_TEST_EXPORT std::vector<DataFormFieldInfo>
parse_data_form_fields(StanzaView xdata_form);

}  // namespace xmpp