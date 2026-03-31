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
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "account.hh"
#include "channel.hh"
#include "user.hh"
#include "message.hh"

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
                    prefix = "#";
                    symbol = channel->second.name;
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
    result += weechat_color("chat_nick");
    result += prefix;
    result += symbol;
    result += weechat_color("reset");
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
    regex_t reg;
    regmatch_t groups[max_groups];
    char msgbuf[100];
    const char *cursor;
    size_t offset;

    if ((rc = regcomp(&reg, format_regex, REG_EXTENDED)))
    {
        regerror(rc, &reg, msgbuf, sizeof(msgbuf));
        weechat_printf(
            account->buffer,
            _("%s%s: error compiling message formatting regex: %s"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
            msgbuf);
        return std::string(text);
    }

    std::string decoded_text;
    decoded_text.reserve(MESSAGE_MAX_LENGTH);

    for (cursor = text.data(); regexec(&reg, cursor, max_groups, groups, 0) == 0; cursor += offset)
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

    regfree(&reg);
    return htmldec;
}
