// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "xmpp/iq_room_config.hh"

namespace xmpp {
auto make_room_config_query(const data_form *form, bool cancel)
    -> std::expected<stanza::spec, std::string>
{
    stanza::xep0045::xep0045owner::query query;
    if (cancel) {
        stanza::xep0004::form cancellation("cancel");
        query.form(cancellation);
    }
    else if (form) {
        auto submission = submit_data_form(*form);
        if (!submission) return std::unexpected(submission.error());
        query.form(*submission);
    }
    return query;
}
}
