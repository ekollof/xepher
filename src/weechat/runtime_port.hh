// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "test_export.hh"

namespace weechat {

// Host-runtime queries (version string, colours) abstracted for tests.
class RuntimePort {
public:
    virtual ~RuntimePort() = default;

    [[nodiscard]] virtual std::string version_string() = 0;
    [[nodiscard]] virtual const char *color(std::string_view name) = 0;
    [[nodiscard]] virtual const char *prefix(std::string_view name) = 0;
    [[nodiscard]] virtual std::string xmpp_color(std::string_view name) = 0;

    static RuntimePort &default_runtime();
};

class WeechatRuntimePort final : public RuntimePort {
public:
    [[nodiscard]] std::string version_string() override;
    [[nodiscard]] const char *color(std::string_view name) override;
    [[nodiscard]] const char *prefix(std::string_view name) override;
    [[nodiscard]] std::string xmpp_color(std::string_view name) override;
};

}  // namespace weechat