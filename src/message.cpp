// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <strophe.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <regex.h>
#include <string>
#include <vector>
#include <algorithm>
#include <ranges>
#include <fmt/core.h>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "weechat/runtime_port.hh"
#include "account.hh"
#include "channel.hh"
#include "user.hh"
#include "message.hh"
#include "util.hh"
#include "weechat/ui_port.hh"

#include "message_emoji_shortcodes.inl"

// RAII wrapper for POSIX regex_t — calls regfree() on scope exit.
struct regex_guard {
    regex_guard() = default;
    ~regex_guard() { regfree(&reg); }
    regex_guard(const regex_guard&) = delete;
    regex_guard& operator=(const regex_guard&) = delete;
    regex_t reg{};
};

static const char format_regex[] = "<([^>]*?)>";
static const size_t max_groups = 2;

static std::string message__translate_code(weechat::account *account,
                                   const char *code)
{
    decltype(account->channels)::iterator channel;
    weechat::user *user;
    std::string identifier(code);
    std::string alttext;
    std::string symbol;
    const char *prefix;

    auto pipe_pos = identifier.find('|');
    if (pipe_pos != std::string::npos)
    {
        alttext = identifier.substr(pipe_pos + 1);
        identifier.resize(pipe_pos);
    }

    switch (identifier[0])
    {
        case '#': /* channel */
            if (!alttext.empty())
            {
                prefix = "#";
                symbol = alttext;
            }
            else
            {
                channel = account->channels.find(identifier.c_str()+1);
                if (channel != account->channels.end())
                {
                    auto& [_, ch] = *channel;
                    prefix = "#";
                    symbol = ch.name;
                }
                else
                {
                    prefix = "Channel:";
                    symbol = identifier.substr(1);
                }
            }
            break;
        case '@': /* user */
            if (!alttext.empty())
            {
                prefix = "@";
                symbol = alttext;
            }
            else
            {
                user = weechat::user::search(account, identifier.c_str()+1);
                if (user)
                {
                    prefix = "@";
                    symbol = user->profile.display_name;
                }
                else
                {
                    prefix = "@";
                    symbol = identifier.substr(1);
                }
            }
            break;
        case '!': /* special */
            if (!alttext.empty())
            {
                prefix = "@";
                symbol = alttext;
            }
            else
            {
                prefix = "@";
                symbol = identifier.substr(1);
            }
            break;
        default: /* url */
            prefix = "";
            symbol = code;
            break;
    }

    std::string result;
    result += weechat::RuntimePort::default_runtime().color("chat_nick");
    result += prefix;
    result += symbol;
    result += weechat::RuntimePort::default_runtime().color("reset");
    return result;
}

XMPP_TEST_EXPORT void message__htmldecode(char *dest, const char *src, size_t n)
{
    size_t i, j;

    for (i = 0, j = 0; i < n; i++, j++)
        switch (src[i])
        {
            case '\0':
                dest[j] = '\0';
                return;
            case '&':
                if (src[i+1] == 'g' &&
                    src[i+2] == 't' &&
                    src[i+3] == ';')
                {
                    dest[j] = '>';
                    i += 3;
                    break;
                }
                else if (src[i+1] == 'l' &&
                         src[i+2] == 't' &&
                         src[i+3] == ';')
                {
                    dest[j] = '<';
                    i += 3;
                    break;
                }
                else if (src[i+1] == 'a' &&
                         src[i+2] == 'm' &&
                         src[i+3] == 'p' &&
                         src[i+4] == ';')
                {
                    dest[j] = '&';
                    i += 4;
                    break;
                }
                /* fallthrough */
            default:
                dest[j] = src[i];
                break;
        }
    dest[j-1] = '\0';
    return;
}

std::string message__decode(weechat::account *account,
                           std::string_view text)
{
    int rc;
    regex_guard rg;
    regmatch_t groups[max_groups];
    const char *cursor;
    size_t offset;

    if ((rc = regcomp(&rg.reg, format_regex, REG_EXTENDED)))
    {
        std::string msgbuf(100, '\0');
        regerror(rc, &rg.reg, msgbuf.data(), msgbuf.size());
        weechat::UiPort::for_buffer(account->buffer)->printf_error(
            fmt::format(fmt::runtime(_("%s: error compiling message formatting regex: %s")),
                        WEECHAT_XMPP_PLUGIN_NAME, msgbuf));
        return std::string(text);
    }

    std::string decoded_text;
    decoded_text.reserve(MESSAGE_MAX_LENGTH);

    for (cursor = text.data(); regexec(&rg.reg, cursor, max_groups, groups, 0) == 0; cursor += offset)
    {
        offset = groups[0].rm_eo;

        // Text before the match (up to groups[0].rm_so)
        decoded_text.append(cursor, groups[0].rm_so);

        // The matched group content (groups[1].rm_so .. groups[1].rm_eo)
        std::string match(cursor + groups[1].rm_so,
                          groups[1].rm_eo - groups[1].rm_so);

        decoded_text += message__translate_code(account, match.c_str());
    }
    decoded_text += cursor;

    // htmldecode in-place
    std::string htmldec(decoded_text.size(), '\0');
    message__htmldecode(htmldec.data(), decoded_text.c_str(), decoded_text.size() + 1);
    htmldec.resize(std::string_view(htmldec.c_str()).size());

    return htmldec;
}

static const std::pair<std::string_view, std::string_view> k_emoticons[] = {
    {":-)", "😊"}, {":)",  "😊"},
    {":-(", "😢"}, {":(",  "😢"},
    {";-)", "😉"}, {";)",  "😉"},
    {":-D", "😀"}, {":D",  "😀"},
    {":-P", "😛"}, {":P",  "😛"},
    {":-O", "😮"}, {":O",  "😮"},
    {":-/", "😕"}, {":/",  "😕"},
    {":'(", "😢"},
    {"xD",  "😆"}, {"XD",  "😆"},
    {"^_^", "😊"},
    {"<3",  "❤️"},
    {":-*", "😘"}, {":*",  "😘"},
    {"B)",  "😎"}, {"B-)", "😎"},
    {":|",  "😐"}, {":-|", "😐"},
    {"D:",  "😧"},
    {">:(", "😠"}, {">:-(", "😠"},
    {":-\\", "😕"},{":\\", "😕"},
    {"o.O", "😕"}, {"O.o", "😕"},
    {":3",  "😺"},
    {":-3", "😺"},
};

namespace {

[[nodiscard]] bool is_emoji_shortcode_name_char(const unsigned char c) noexcept
{
    return std::islower(c) || std::isdigit(c) || c == '_' || c == '+' || c == '-';
}

} // namespace

XMPP_TEST_EXPORT std::string replace_emoticons(std::string_view text)
{
    // Sort longest-first so multi-char emoticons like ":-)" are matched before ")"
    static const auto &sorted = [] -> decltype(auto) {
        static auto s = std::vector(k_emoticons, k_emoticons + std::size(k_emoticons));
        std::ranges::sort(s, [](auto &a, auto &b) { return a.first.size() > b.first.size(); });
        return s;
    }();

    const auto &shortcodes = emoji_shortcode_map();

    auto after_boundary = [](const char c) -> bool {
        return std::isspace(static_cast<unsigned char>(c))
            || std::ispunct(static_cast<unsigned char>(c));
    };
    auto before_boundary = [](const char c) -> bool {
        return c == ':'
            ? false  // "::)" / "::smile:" — second ':' is not a shortcode opener
            : (std::isspace(static_cast<unsigned char>(c))
               || std::ispunct(static_cast<unsigned char>(c)));
    };

    std::string result;
    result.reserve(text.size());

    for (std::size_t i = 0; i < text.size();)
    {
        const bool at_boundary = (i == 0) || before_boundary(text[i - 1]);

        if (at_boundary)
        {
            const std::string_view tail = text.substr(i);
            const auto emoticon = std::ranges::find_if(sorted, [&](const auto &e) {
                return tail.starts_with(e.first)
                    && (i + e.first.size() >= text.size()
                        || after_boundary(text[i + e.first.size()]));
            });
            if (emoticon != sorted.end())
            {
                result += emoticon->second;
                i += emoticon->first.size();
                continue;
            }

            // GitHub-style shortcodes: :thumbsup:, :+1:, :smile:
            if (text[i] == ':')
            {
                std::size_t j = i + 1;
                while (j < text.size()
                       && is_emoji_shortcode_name_char(static_cast<unsigned char>(text[j])))
                    ++j;

                if (j < text.size() && text[j] == ':' && j > i + 1
                    && (j + 1 >= text.size() || after_boundary(text[j + 1])))
                {
                    const std::string_view name = text.substr(i + 1, j - i - 1);
                    if (const auto it = shortcodes.find(name); it != shortcodes.end())
                    {
                        result += it->second;
                        i = j + 1;
                        continue;
                    }
                }
            }
        }

        result += text[i];
        ++i;
    }
    return result;
}

namespace {

[[nodiscard]] std::string_view trim_trailing_spaces(std::string_view text)
{
    while (!text.empty() && text.back() == ' ')
        text.remove_suffix(1);
    return text;
}

[[nodiscard]] std::string_view last_token(std::string_view text)
{
    const auto pos = text.rfind(' ');
    return pos == std::string_view::npos ? text : text.substr(pos + 1);
}

[[nodiscard]] std::string_view shortcode_name_from_token(std::string_view token)
{
    if (token.starts_with(':'))
        token.remove_prefix(1);
    if (!token.empty() && token.back() == ':')
        token.remove_suffix(1);
    return token;
}

}  // namespace

XMPP_TEST_EXPORT std::optional<std::string_view>
emoji_shortcode_completion_prefix(std::string_view line)
{
    constexpr std::string_view react_cmd = "/react";
    if (line.starts_with(react_cmd))
    {
        std::string_view rest = line.substr(react_cmd.size());
        while (!rest.empty() && rest.front() == ' ')
            rest.remove_prefix(1);
        if (rest.empty())
            return std::string_view{};

        const std::string_view token = last_token(rest);
        if (!token.starts_with(':'))
            return std::nullopt;
        return shortcode_name_from_token(token);
    }

    const std::string_view token = last_token(trim_trailing_spaces(line));
    if (!token.starts_with(':'))
        return std::nullopt;
    return shortcode_name_from_token(token);
}

XMPP_TEST_EXPORT std::string resolve_emoji_shortcode(std::string_view input)
{
    if (input.size() < 3 || input.front() != ':' || input.back() != ':')
        return std::string(input);

    const std::string_view name = input.substr(1, input.size() - 2);
    if (name.empty())
        return std::string(input);

    if (const auto it = emoji_shortcode_map().find(name); it != emoji_shortcode_map().end())
        return std::string(it->second);

    return std::string(input);
}

XMPP_TEST_EXPORT std::vector<std::string>
emoji_shortcode_completions(std::string_view prefix, const std::size_t limit)
{
    std::vector<std::string> codes;
    codes.reserve(std::min(limit, std::size(k_emoji_shortcodes)));

    const auto begin = std::begin(k_emoji_shortcodes);
    const auto end = std::end(k_emoji_shortcodes);
    auto it = std::ranges::lower_bound(
        begin, end, prefix, std::ranges::less{},
        &emoji_shortcode_entry::code);

    for (; it != end && codes.size() < limit; ++it)
    {
        if (!it->code.starts_with(prefix))
            break;
        codes.emplace_back(fmt::format(":{}:", it->code));
    }

    return codes;
}
