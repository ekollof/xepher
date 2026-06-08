// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <ctime>
#include <string>
#include <vector>

#include "../src/weechat/runtime_port.hh"
#include "../src/weechat/ui_port.hh"

namespace test_weechat {

class CapturingUiPort final : public weechat::UiPort {
public:
    struct dated_line {
        std::time_t date{};
        std::string tags;
        std::string msg;
    };

    std::vector<std::string> lines;
    std::vector<std::string> errors;
    std::vector<std::string> info;
    std::vector<std::string> network;
    std::vector<dated_line> dated;

    void printf(std::string_view msg) override { lines.emplace_back(msg); }
    void printf_error(std::string_view msg) override { errors.emplace_back(msg); }
    void printf_info(std::string_view msg) override { info.emplace_back(msg); }
    void printf_network(std::string_view msg) override { network.emplace_back(msg); }
    void printf_date_tags(std::time_t date, std::string_view tags, std::string_view msg) override
    {
        dated.push_back({date, std::string(tags), std::string(msg)});
    }
};

class StubRuntimePort final : public weechat::RuntimePort {
public:
    explicit StubRuntimePort(std::string version = "test-version")
        : version_(std::move(version))
    {}

    [[nodiscard]] std::string version_string() override { return version_; }
    [[nodiscard]] const char *color(std::string_view) override { return ""; }

private:
    std::string version_;
};

}  // namespace test_weechat