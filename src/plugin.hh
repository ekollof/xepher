// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <vector>
#include <string>

#include "strophe.hh"
#include "test_export.hh"

#define STR(X) #X
#define XSTR(X) STR(X)

#define weechat_plugin (&*weechat::plugin::instance->ptr())
#define WEECHAT_XMPP_PLUGIN_NAME "xmpp"

#ifdef GIT_COMMIT
#define XMPP_PLUGIN_COMMIT XSTR(GIT_COMMIT)
#define WEECHAT_XMPP_PLUGIN_VERSION "0.5.0@" XMPP_PLUGIN_COMMIT
#else//GIT_COMMIT
#define XMPP_PLUGIN_COMMIT "unknown"
#define WEECHAT_XMPP_PLUGIN_VERSION "0.5.0"
#endif//GIT_COMMIT

#define xmpp_printf_date_tags(buffer, date, tags, ...) \
    (weechat_plugin->printf_datetime_tags)(buffer, date, 0, weechat::xmpp_tags(tags).c_str(), __VA_ARGS__)

namespace weechat {
    inline std::string xmpp_tags(const char *tags)
    {
        if (!tags || !tags[0]) return "no_trigger";
        std::string result(tags);
        result += ",no_trigger";
        return result;
    }

    class plugin {
    private:
        struct t_weechat_plugin *m_plugin_ptr; // packed first for hackery

    public:
        XMPP_TEST_EXPORT plugin(struct t_weechat_plugin *plugin_ptr);
        virtual ~plugin();

        void init(int argc, char *argv[]);
        void end();

        XMPP_TEST_EXPORT static std::unique_ptr<plugin> instance;

        inline struct t_weechat_plugin * ptr() { return m_plugin_ptr; };
        inline operator struct t_weechat_plugin *() { return m_plugin_ptr; };

    private:
        static constexpr std::string_view encryption_bar_item_name = "xmpp_encryption";

        struct t_hook *m_process_timer;
        struct t_gui_bar_item *m_encryption_bar_item;
        struct t_hook *m_buffer_switch_hook;
        struct t_hook *m_input_text_changed_hook;
        struct t_hook *m_nick_color_config_hook;
        struct t_hook *m_nick_color_look_hook;

        std::vector<std::string_view> m_args;
    };
};
