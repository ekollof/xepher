// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <string>
#include <string_view>

#include <strophe.h>

#include "test_export.hh"

namespace weechat {

// Bound-JID queries abstracted from libstrophe for tests and domain logic.
class ConnectionPort {
public:
    virtual ~ConnectionPort() = default;

    [[nodiscard]] virtual bool is_connected() const = 0;
    [[nodiscard]] virtual std::string bound_jid_bare() = 0;
    [[nodiscard]] virtual std::string bound_jid_full() = 0;
    virtual void refresh_bare_cache() = 0;
    virtual void clear_bare_cache() = 0;
};

class LibstropheConnectionPort final : public ConnectionPort {
public:
    LibstropheConnectionPort(xmpp_conn_t *conn, std::string &bare_cache,
                             std::string_view configured_jid);

    [[nodiscard]] bool is_connected() const override;
    [[nodiscard]] std::string bound_jid_bare() override;
    [[nodiscard]] std::string bound_jid_full() override;
    void refresh_bare_cache() override;
    void clear_bare_cache() override;

private:
    xmpp_conn_t *conn_;
    std::string *bare_cache_;
    std::string_view configured_jid_;
};

}  // namespace weechat