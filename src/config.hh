// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <functional>
#include <unordered_map>
#include <optional>
#include <weechat/weechat-plugin.h>
#include "fmt/core.h"
#include "plugin.hh"

#include "config/file.hh"
#include "config/section.hh"
#include "config/account.hh"
#include "config/option.hh"

namespace weechat
{
    class config;
    struct config_file;
    struct config_section;
    struct config_option;

    class config {
    public:
        enum class nick_completion
        {
            SMART_OFF = 0,
            SMART_SPEAKERS,
            SMART_SPEAKERS_HIGHLIGHTS,
        };

        config_file file;

        config_section section_account_default;
        config_section section_account;
        config_section section_look;

        config_account account_default;
        struct {
            config_option debug;
            config_option raw_xml_log;
            config_option nick_completion_smart;
            config_option outgoing_link_preview;
            config_option incoming_link_preview;
            config_option send_chat_states;
            config_option smart_filter;
            config_option smart_filter_delay;
            config_option share_os_info;
            config_option mam_fetch_days;
            config_option mam_max_concurrent;
            config_option highlight_words;
            config_option emoticons;
            config_option icat;
            config_option feeds;
        } look;

    public:
        config();
        ~config();

        static std::optional<config> instance;

    public:
        static bool init();
        static bool read();
        static bool write();
    };

    [[nodiscard]] bool xmpp_feeds_enabled();
}
