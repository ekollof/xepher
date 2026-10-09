// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// Runs inside an isolated, temporary WeeChat instance without XMPP accounts.
// Use a separate API pointer so this probe cannot replace the XMPP plugin's API.
#define weechat_plugin form_probe_api
#include <weechat/weechat-plugin.h>
#include "ui/form_editor.hh"
#include <cstdlib>
#include <memory>
#include <string>
#include <fmt/core.h>

struct t_weechat_plugin *form_probe_api = nullptr;

#pragma GCC visibility push(default)
extern "C" {
WEECHAT_PLUGIN_NAME("formprobe");
WEECHAT_PLUGIN_DESCRIPTION("Isolated form editor smoke test");
WEECHAT_PLUGIN_AUTHOR("Xepher");
WEECHAT_PLUGIN_VERSION("1");
WEECHAT_PLUGIN_LICENSE("MPL2");

int weechat_plugin_init(struct t_weechat_plugin *api, int, char **)
{
    form_probe_api = api;
    auto ctx = std::unique_ptr<xmpp_ctx_t, decltype(&xmpp_ctx_free)>(xmpp_ctx_new(nullptr, nullptr), xmpp_ctx_free);
    auto raw = stanza_from_string(ctx.get(), "<x xmlns='jabber:x:data' type='form'><field var='name'><required/><value>Default</value></field><field var='password' type='text-private'/></x>");
    auto model = xmpp::parse_data_form(xmpp::StanzaView(raw.get()));
    if (!model) {
        weechat_command(nullptr, "/print -stdout FORM_SMOKE_FAIL");
        return WEECHAT_RC_ERROR;
    }
    bool passed = true;
    int calls = 0;
    int closed = 0;
    auto opened = weechat::ui::form_editor::open("probe", "test", "Form smoke test", std::move(*model),
        {"complete", "cancel"}, "complete", [&](std::string_view action, const xmpp::data_form &answers) -> std::expected<void, std::string> {
            ++calls;
            passed = passed && (action == "complete" || action == "cancel")
                && answers.fields[0].values == std::vector<std::string>{"Changed"}
                && answers.fields[1].values == std::vector<std::string>{"private-test-value"};
            return {};
        }, [&] { ++closed; });
    if (!opened && opened.error().starts_with("Interactive forms require WeeChat")) {
        weechat_command(nullptr, "/print -stdout FORM_SMOKE_SKIP");
        return WEECHAT_RC_OK;
    }
    passed = passed && opened.has_value();
    auto *buffer = weechat_buffer_search("xmpp", "xmpp.form.test");
    passed = passed && buffer;
    if (buffer) {
        weechat_command(buffer, "Changed");
        const auto id = fmt::format("{:p}", static_cast<void *>(buffer));
        auto mask = std::unique_ptr<char, decltype(&std::free)>(weechat_hook_modifier_exec("input_text_display", id.c_str(), "private-test-value"), std::free);
        auto history = std::unique_ptr<char, decltype(&std::free)>(weechat_hook_modifier_exec("history_add", id.c_str(), "private-test-value"), std::free);
        passed = passed && mask && std::string_view(mask.get()) == "******************"
            && history && std::string_view(history.get()).empty();
        weechat_command(buffer, "private-test-value");
        weechat_command(buffer, ":submit");
        passed = passed && calls == 1;
        weechat_command(buffer, ":submit");
        passed = passed && calls == 1;
        weechat::ui::form_editor::failed("test", "Test rejection");
        weechat_command(buffer, ":submit");
        passed = passed && calls == 2;
        weechat::ui::form_editor::failed("test", "Test rejection");
        weechat_command(buffer, ":cancel");
        passed = passed && calls == 3;
    }
    weechat::ui::form_editor::close_owner("probe");
    passed = passed && closed == 1 && !weechat_buffer_search("xmpp", "xmpp.form.test");
    weechat_command(nullptr, passed ? "/print -stdout FORM_SMOKE_PASS" : "/print -stdout FORM_SMOKE_FAIL");
    return WEECHAT_RC_OK;
}

int weechat_plugin_end(struct t_weechat_plugin *) { return WEECHAT_RC_OK; }
}
#pragma GCC visibility pop
