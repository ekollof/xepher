// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <strophe.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <iostream>
#include <sstream>
#include <utility>
#include <algorithm>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "xmpp/stanza.hh"
#include "xmpp/xep-0054.inl"
#include "xmpp/xep-0292.inl"
#include "account.hh"
#include "user.hh"
#include "avatar.hh"
#include "channel.hh"
#include "buffer.hh"
#include "message.hh"
#include "command.hh"
#include "sexp/driver.hh"

#define MAM_DEFAULT_DAYS 2
#define STR(X) #X

void command__display_account(weechat::account *account)
{
    int num_channels, num_pv;

    if (account->connected())
    {
        num_channels = 0;//xmpp_account_get_channel_count(account);
        num_pv = 0;//xmpp_account_get_pv_count(account);
        weechat_printf(
            NULL,
            " %s %s%s%s %s(%s%s%s) [%s%s%s]%s, %d %s, %d pv",
            (account->connected()) ? "*" : " ",
            weechat_color("chat_server"),
            account->name.data(),
            weechat_color("reset"),
            weechat_color("chat_delimiters"),
            weechat_color("chat_server"),
            account->jid().data(),
            weechat_color("chat_delimiters"),
            weechat_color("reset"),
            (account->connected()) ? _("connected") : _("not connected"),
            weechat_color("chat_delimiters"),
            weechat_color("reset"),
            num_channels,
            NG_("channel", "channels", num_channels),
            num_pv);
    }
    else
    {
        weechat_printf(
            NULL,
            "   %s%s%s %s(%s%s%s)%s",
            weechat_color("chat_server"),
            account->name.data(),
            weechat_color("reset"),
            weechat_color("chat_delimiters"),
            weechat_color("chat_server"),
            account->jid().data(),
            weechat_color("chat_delimiters"),
            weechat_color("reset"));
    }
}

void command__account_list(int argc, char **argv)
{
    int i, one_account_found;
    char *account_name = NULL;

    for (i = 2; i < argc; i++)
    {
        if (!account_name)
            account_name = argv[i];
    }
    if (!account_name)
    {
        if (!weechat::accounts.empty())
        {
            weechat_printf(NULL, "");
            weechat_printf(NULL, _("All accounts:"));
            for (auto& ptr_account2 : weechat::accounts)
            {
                command__display_account(&ptr_account2.second);
            }
        }
        else
            weechat_printf(NULL, _("No account"));
    }
    else
    {
        one_account_found = 0;
        for (auto& ptr_account2 : weechat::accounts)
        {
            if (weechat_strcasestr(ptr_account2.second.name.data(), account_name))
            {
                if (!one_account_found)
                {
                    weechat_printf(NULL, "");
                    weechat_printf(NULL,
                                   _("Servers with \"%s\":"),
                                   account_name);
                }
                one_account_found = 1;
                command__display_account(&ptr_account2.second);
            }
        }
        if (!one_account_found)
            weechat_printf(NULL,
                           _("No account found with \"%s\""),
                           account_name);
    }
}

void command__add_account(const char *name, const char *jid, const char *password)
{
    weechat::account *account = nullptr;
    if (weechat::account::search(account, name, true))
    {
        weechat_printf(
            NULL,
            _("%s%s: account \"%s\" already exists, can't add it!"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
            name);
        return;
    }

    if (!jid || !password) {
        weechat_printf(
            NULL,
            _("%s%s: jid and password required"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return;
    }

    ;
    account = &weechat::accounts.emplace(
        std::piecewise_construct, std::forward_as_tuple(name),
        std::forward_as_tuple(weechat::config::instance->file, name)).first->second;
    if (!account)
    {
        weechat_printf(
            NULL,
            _("%s%s: unable to add account"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return;
    }

    account->name = name;
    if (jid)
        account->jid(jid);
    if (password)
        account->password(password);
    if (jid) {
        xmpp_string_guard jid_node_g(account->context, xmpp_jid_node(account->context, jid));
        account->nickname(jid_node_g.c_str());
    }

    weechat_printf(
        NULL,
        _("%s: account %s%s%s %s(%s%s%s)%s added"),
        WEECHAT_XMPP_PLUGIN_NAME,
        weechat_color("chat_server"),
        account->name.data(),
        weechat_color("reset"),
        weechat_color("chat_delimiters"),
        weechat_color("chat_server"),
        jid ? jid : "???",
        weechat_color("chat_delimiters"),
        weechat_color("reset"));
}

void command__account_add(struct t_gui_buffer *buffer, int argc, char **argv)
{
    char *name, *jid = NULL, *password = NULL;

    (void) buffer;

    switch (argc)
    {
        case 5:
            password = argv[4];
            // fall through
        case 4:
            jid = argv[3];
            // fall through
        case 3:
            name = argv[2];
            command__add_account(name, jid, password);
            break;
        default:
            weechat_printf(NULL, _("account add: wrong number of arguments"));
            break;
    }
}

int command__connect_account(weechat::account *account)
{
    if (!account)
        return 0;

    if (account->connected())
    {
        weechat_printf(
            NULL,
            _("%s%s: already connected to account \"%s\"!"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
            account->name.data());
    }

    account->connect(true);  // Manual connect from user command

    return 1;
}

int command__account_connect(struct t_gui_buffer *buffer, int argc, char **argv)
{
    int i, connect_ok;
    weechat::account *ptr_account = nullptr;

    (void) buffer;
    (void) argc;
    (void) argv;

    connect_ok = 1;

    for (i = 2; i < argc; i++)
    {
        if (weechat::account::search(ptr_account, argv[i]))
        {
            if (!command__connect_account(ptr_account))
            {
                connect_ok = 0;
            }
        }
        else
        {
            weechat_printf(
                NULL,
                _("%s%s: account not found \"%s\" "
                  "(add one first with: /account add)"),
                weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                argv[i]);
        }
    }

    return (connect_ok) ? WEECHAT_RC_OK : WEECHAT_RC_ERROR;
}

int command__disconnect_account(weechat::account *account)
{
    if (!account)
        return 0;

    if (!account->connected())
    {
        weechat_printf(
            NULL,
            _("%s%s: not connected to account \"%s\"!"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
            account->name.data());
    }

    account->disconnect(0);

    return 1;
}

int command__account_disconnect(struct t_gui_buffer *buffer, int argc, char **argv)
{
    int i, disconnect_ok;
    weechat::account *ptr_account;

    (void) argc;
    (void) argv;

    disconnect_ok = 1;

    if (argc < 2)
    {
        weechat::channel *ptr_channel;

        buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

        if (ptr_account)
        {
            if (!command__disconnect_account(ptr_account))
            {
                disconnect_ok = 0;
            }
        }
    }
    for (i = 2; i < argc; i++)
    {
        if (weechat::account::search(ptr_account, argv[i]))
        {
            if (!command__disconnect_account(ptr_account))
            {
                disconnect_ok = 0;
            }
        }
        else
        {
            weechat_printf(
                NULL,
                _("%s%s: account not found \"%s\" "),
                weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                argv[i]);
        }
    }

    return (disconnect_ok) ? WEECHAT_RC_OK : WEECHAT_RC_ERROR;
}

int command__account_reconnect(struct t_gui_buffer *buffer, int argc, char **argv)
{
    command__account_disconnect(buffer, argc, argv);
    return command__account_connect(buffer, argc, argv);
}

void command__account_delete(struct t_gui_buffer *buffer, int argc, char **argv)
{
    (void) buffer;

    if (argc < 3)
    {
        weechat_printf(
            NULL,
            _("%sToo few arguments for command\"%s %s\" "
              "(help on command: /help %s)"),
            weechat_prefix("error"),
            argv[0], argv[1], argv[0] + 1);
        return;
    }

    weechat::account *account = nullptr;

    if (!weechat::account::search(account, argv[2]))
    {
        weechat_printf(
            NULL,
            _("%s%s: account \"%s\" not found for \"%s\" command"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
            argv[2], "xmpp delete");
        return;
    }
    if (account->connected())
    {
        weechat_printf(
            NULL,
            _("%s%s: you cannot delete account \"%s\" because you"
              "are connected. Try \"/xmpp disconnect %s\" first."),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
            argv[2], argv[2]);
        return;
    }

    std::string account_name = account->name;
    weechat::accounts.erase(account->name);
    weechat_printf(
        NULL,
        _("%s: account %s%s%s has been deleted"),
        WEECHAT_XMPP_PLUGIN_NAME,
        weechat_color("chat_server"),
        !account_name.empty() ? account_name.data() : "???",
        weechat_color("reset"));
}

int command__account(const void *pointer, void *data,
                     struct t_gui_buffer *buffer, int argc,
                     char **argv, char **argv_eol)
{

    (void) pointer;
    (void) data;
    (void) buffer;

    if (argc <= 1 || weechat_strcasecmp(argv[1], "list") == 0)
    {
        command__account_list(argc, argv);
        return WEECHAT_RC_OK;
    }

    if (argc > 1)
    {
        if (weechat_strcasecmp(argv[1], "add") == 0)
        {
            command__account_add(buffer, argc, argv);
            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "connect") == 0)
        {
            command__account_connect(buffer, argc, argv);
            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "disconnect") == 0)
        {
            command__account_disconnect(buffer, argc, argv);
            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "reconnect") == 0)
        {
            command__account_reconnect(buffer, argc, argv);
            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "delete") == 0)
        {
            command__account_delete(buffer, argc, argv);
            return WEECHAT_RC_OK;
        }

        WEECHAT_COMMAND_ERROR;
    }

    return WEECHAT_RC_OK;
}

int command__enter(const void *pointer, void *data,
                   struct t_gui_buffer *buffer, int argc,
                   char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;
    xmpp_stanza_t *pres;
    const char *jid, *pres_jid, *text;

    (void) pointer;
    (void) data;
    (void) argv;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_account->connected())
    {
        weechat_printf(buffer,
                        _("%s%s: you are not connected to server"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    if (argc > 1)
    {
        int n_jid = 0;
        char **jids = weechat_string_split(argv[1], ",", NULL, 0, 0, &n_jid);
        for (int i = 0; i < n_jid; i++)
        {
            xmpp_string_guard jid_bare_g(ptr_account->context,
                xmpp_jid_bare(ptr_account->context, jids[i]));
            jid = jid_bare_g.c_str();
            pres_jid = jids[i];

            if(!xmpp_jid_resource(ptr_account->context, pres_jid))
            {
                xmpp_string_guard pres_node_g(ptr_account->context,
                    xmpp_jid_node(ptr_account->context, jid));
                xmpp_string_guard pres_domain_g(ptr_account->context,
                    xmpp_jid_domain(ptr_account->context, jid));
                const char *nick = ptr_account->nickname().data()
                    && strlen(ptr_account->nickname().data())
                    ? ptr_account->nickname().data()
                    : nullptr;
                xmpp_string_guard fallback_nick_g(ptr_account->context,
                    nick ? nullptr : xmpp_jid_node(ptr_account->context,
                                                   ptr_account->jid().data()));
                xmpp_string_guard pres_jid_g(ptr_account->context,
                    xmpp_jid_new(ptr_account->context,
                        pres_node_g.c_str(),
                        pres_domain_g.c_str(),
                        nick ? nick : fallback_nick_g.c_str()));
                pres_jid = pres_jid_g.c_str();

                if (!ptr_account->channels.contains(jid))
                {
                    ptr_channel = &ptr_account->channels.emplace(
                        std::make_pair(jid, weechat::channel {
                                *ptr_account, weechat::channel::chat_type::MUC, jid, jid
                            })).first->second;
                    ptr_account->load_pgp_keys();
                }
                if (!ptr_channel) {
                    weechat_string_free_split(jids);
                    return WEECHAT_RC_ERROR;
                }

                pres = xmpp_presence_new(ptr_account->context);
                xmpp_stanza_set_to(pres, pres_jid);
                xmpp_stanza_set_from(pres, ptr_account->jid().data());

                xmpp_stanza_t *pres__x = xmpp_stanza_new(ptr_account->context);
                xmpp_stanza_set_name(pres__x, "x");
                xmpp_stanza_set_ns(pres__x, "http://jabber.org/protocol/muc");
                xmpp_stanza_add_child(pres, pres__x);
                xmpp_stanza_release(pres__x);

                ptr_account->connection.send( pres);
                xmpp_stanza_release(pres);
            }
            else
            {
                if (!ptr_account->channels.contains(jid))
                {
                    ptr_channel = &ptr_account->channels.emplace(
                        std::make_pair(jid, weechat::channel {
                                *ptr_account, weechat::channel::chat_type::MUC, jid, jid
                            })).first->second;
                    ptr_account->load_pgp_keys();
                }
                if (!ptr_channel) {
                    weechat_string_free_split(jids);
                    return WEECHAT_RC_ERROR;
                }

                pres = xmpp_presence_new(ptr_account->context);
                xmpp_stanza_set_to(pres, pres_jid);
                xmpp_stanza_set_from(pres, ptr_account->jid().data());

                xmpp_stanza_t *pres__x = xmpp_stanza_new(ptr_account->context);
                xmpp_stanza_set_name(pres__x, "x");
                xmpp_stanza_set_ns(pres__x, "http://jabber.org/protocol/muc");
                xmpp_stanza_add_child(pres, pres__x);
                xmpp_stanza_release(pres__x);

                ptr_account->connection.send( pres);
                xmpp_stanza_release(pres);
            }

            if (argc > 2)
            {
                text = argv_eol[2];

                ptr_channel->send_message(jid, text);
            }

            char buf[16];
            int num = weechat_buffer_get_integer(ptr_channel->buffer, "number");
            snprintf(buf, sizeof(buf), "/buffer %d", num);
            weechat_command(ptr_account->buffer, buf);
        }
        weechat_string_free_split(jids);
    }
    else
    {
        const char *buffer_jid = weechat_buffer_get_string(buffer, "localvar_remote_jid");

        {
            xmpp_string_guard node_g(ptr_account->context,
                xmpp_jid_node(ptr_account->context, buffer_jid));
            xmpp_string_guard domain_g(ptr_account->context,
                xmpp_jid_domain(ptr_account->context, buffer_jid));
            xmpp_string_guard pres_jid_g(ptr_account->context,
                xmpp_jid_new(ptr_account->context, node_g.c_str(), domain_g.c_str(),
                    weechat_buffer_get_string(buffer, "localvar_nick")));
            pres_jid = pres_jid_g.c_str();

            if (!ptr_account->channels.contains(buffer_jid))
                ptr_channel = &ptr_account->channels.emplace(
                    std::make_pair(std::string(buffer_jid), weechat::channel {
                            *ptr_account, weechat::channel::chat_type::MUC, buffer_jid, buffer_jid
                        })).first->second;

            pres = xmpp_presence_new(ptr_account->context);
            xmpp_stanza_set_to(pres, pres_jid);
            xmpp_stanza_set_from(pres, ptr_account->jid().data());

            xmpp_stanza_t *pres__x = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(pres__x, "x");
            xmpp_stanza_set_ns(pres__x, "http://jabber.org/protocol/muc");
            xmpp_stanza_add_child(pres, pres__x);
            xmpp_stanza_release(pres__x);

            ptr_account->connection.send( pres);
            xmpp_stanza_release(pres);
        }
    }

    return WEECHAT_RC_OK;
}

int command__open(const void *pointer, void *data,
                  struct t_gui_buffer *buffer, int argc,
                  char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;
    xmpp_stanza_t *pres;
    char *jid, *text;

    (void) pointer;
    (void) data;
    (void) argv;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_account->connected())
    {
        weechat_printf(buffer,
                        _("%s%s: you are not connected to server"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    if (argc > 1)
    {
        int n_jid = 0;
        char **jids = weechat_string_split(argv[1], ",", NULL, 0, 0, &n_jid);
        for (int i = 0; i < n_jid; i++)
        {
            xmpp_string_guard bare_g(ptr_account->context,
                                     xmpp_jid_bare(ptr_account->context, jids[i]));
            const char *effective_jid = bare_g.c_str();

            // When in a MUC and given a bare nick (no '@'), build the full JID.
            // All three guards outlive the if-block via the enclosing for-body scope.
            xmpp_string_guard node_g(ptr_account->context,
                ptr_channel && !strchr(effective_jid, '@')
                    ? xmpp_jid_node(ptr_account->context, ptr_channel->name.data())
                    : nullptr);
            xmpp_string_guard domain_g(ptr_account->context,
                ptr_channel && !strchr(effective_jid, '@')
                    ? xmpp_jid_domain(ptr_account->context, ptr_channel->name.data())
                    : nullptr);
            xmpp_string_guard full_g(ptr_account->context,
                (node_g && domain_g)
                    ? xmpp_jid_new(ptr_account->context,
                                   node_g.c_str(), domain_g.c_str(), effective_jid)
                    : nullptr);
            if (full_g)
                effective_jid = full_g.c_str();

            jid = const_cast<char*>(effective_jid);

            pres = xmpp_presence_new(ptr_account->context);
            xmpp_stanza_set_to(pres, jid);
            xmpp_stanza_set_from(pres, ptr_account->jid().data());
            ptr_account->connection.send( pres);
            xmpp_stanza_release(pres);

            auto channel = ptr_account->channels.find(jid);
            if (channel == ptr_account->channels.end())
            {
                // Reset MAM timestamp for this channel so history will be fetched
                // (it might be -1 if previously closed)
                ptr_account->mam_cache_set_last_timestamp(jid, 0);
                
                channel = ptr_account->channels.emplace(
                    std::make_pair(std::string(jid), weechat::channel {
                            *ptr_account, weechat::channel::chat_type::PM, jid, jid
                        })).first;
            }

            if (argc > 2)
            {
                text = argv_eol[2];

                channel->second.send_message(jid, text);
            }

            char buf[16];
            int num = weechat_buffer_get_integer(channel->second.buffer, "number");
            snprintf(buf, sizeof(buf), "/buffer %d", num);
            weechat_command(ptr_account->buffer, buf);
        }
        weechat_string_free_split(jids);
    }

    return WEECHAT_RC_OK;
}

int command__msg(const void *pointer, void *data,
                 struct t_gui_buffer *buffer, int argc,
                 char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;
    xmpp_stanza_t *message;
    char *text;

    (void) pointer;
    (void) data;
    (void) argv;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_channel)
    {
        weechat_printf(
            ptr_account->buffer,
            _("%s%s: \"%s\" command can not be executed on a account buffer"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, "msg");
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        weechat_printf(buffer,
                        _("%s%s: you are not connected to server"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    if (argc > 1)
    {
        text = argv_eol[1];

        message = xmpp_message_new(ptr_account->context,
                                   ptr_channel->type == weechat::channel::chat_type::MUC ? "groupchat" : "chat",
                                   ptr_channel->name.data(), NULL);
        xmpp_message_set_body(message, text);
        ptr_account->connection.send( message);
        xmpp_stanza_release(message);
        if (ptr_channel->type != weechat::channel::chat_type::MUC)
            weechat_printf_date_tags(ptr_channel->buffer, 0,
                                     "xmpp_message,message,private,notify_none,self_msg,log1",
                                     "%s\t%s",
                                     weechat::user::search(ptr_account, ptr_account->jid().data())->as_prefix_raw().data(),
                                     text);
    }

    return WEECHAT_RC_OK;
}

int command__me(const void *pointer, void *data,
                struct t_gui_buffer *buffer, int argc,
                char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;
    xmpp_stanza_t *message;

    (void) pointer;
    (void) data;
    (void) argv;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_channel)
    {
        weechat_printf(
            ptr_account->buffer,
            _("%s%s: \"%s\" command can not be executed on a account buffer"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, "me");
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        weechat_printf(buffer,
                        _("%s%s: you are not connected to server"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    if (argc > 1)
    {
        std::string me_text = std::string("/me ") + argv_eol[1];

        message = xmpp_message_new(ptr_account->context,
                                   ptr_channel->type == weechat::channel::chat_type::MUC ? "groupchat" : "chat",
                                   ptr_channel->name.data(), NULL);
        xmpp_message_set_body(message, me_text.data());
        ptr_account->connection.send( message);
        xmpp_stanza_release(message);
        if (ptr_channel->type != weechat::channel::chat_type::MUC)
        {
            auto display_prefix = weechat::user::as_prefix_raw(ptr_account, ptr_account->jid().data());
            if (display_prefix.empty())
                display_prefix = ptr_account->jid();
            weechat_printf_date_tags(ptr_channel->buffer, 0,
                                     "xmpp_message,message,action,private,notify_none,self_msg,log1",
                                     "%s%s %s",
                                     weechat_prefix("action"),
                                     display_prefix.data(),
                                     argv_eol[1]);
        }
    }

    return WEECHAT_RC_OK;
}

int command__invite(const void *pointer, void *data,
                    struct t_gui_buffer *buffer, int argc,
                    char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;
    (void) argv_eol;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_channel || ptr_channel->type != weechat::channel::chat_type::MUC)
    {
        weechat_printf(
            ptr_account->buffer,
            _("%s%s: \"%s\" command can only be executed in a MUC buffer"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, "invite");
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        weechat_printf(buffer,
                        _("%s%s: you are not connected to server"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    if (argc < 2)
    {
        weechat_printf(buffer,
                        _("%s%s: missing argument for \"%s\" command"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, "invite");
        return WEECHAT_RC_OK;
    }

    const char *invitee_jid = argv[1];
    const char *reason = argc > 2 ? argv_eol[2] : nullptr;

    // Build invitation message using XEP-0249
    xmpp_stanza_t *message = xmpp_message_new(ptr_account->context, NULL, invitee_jid, NULL);
    
    xmpp_stanza_t *x = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(x, "x");
    xmpp_stanza_set_ns(x, "jabber:x:conference");
    xmpp_stanza_set_attribute(x, "jid", ptr_channel->name.data());
    if (reason)
        xmpp_stanza_set_attribute(x, "reason", reason);
    
    xmpp_stanza_add_child(message, x);
    xmpp_stanza_release(x);
    
    ptr_account->connection.send(message);
    xmpp_stanza_release(message);

    weechat_printf(buffer,
                    _("%sInvited %s to %s"),
                    weechat_prefix("network"),
                    invitee_jid,
                    ptr_channel->name.data());

    return WEECHAT_RC_OK;
}

int command__mam(const void *pointer, void *data,
                 struct t_gui_buffer *buffer, int argc,
                 char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;
    int days;

    (void) pointer;
    (void) data;
    (void) argv_eol;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_channel)
    {
        weechat_printf(
            ptr_account->buffer,
            _("%s%s: \"%s\" command can not be executed on a account buffer"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, "mam");
        return WEECHAT_RC_OK;
    }

    time_t start, end;
    if (argc > 1)
    {
        errno = 0;
        days = strtol(argv[1], NULL, 10);

        if (errno != 0)
        {
            weechat_printf(
                ptr_channel->buffer,
                _("%s%s: \"%s\" is not a valid number of %s for %s"),
                weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, argv[1], "days", "mam");
            days = MAM_DEFAULT_DAYS;
        }
    }
    else
        days = MAM_DEFAULT_DAYS;
    
    // Calculate time range: (now - days) to now
    end = time(NULL);
    start = end - (days * 24 * 60 * 60);
    
    xmpp_string_guard mam_uuid_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
    const char *mam_uuid = mam_uuid_g.ptr;
    ptr_channel->fetch_mam(mam_uuid, &start, &end, NULL);
    // freed by mam_uuid_g

    return WEECHAT_RC_OK;
}

int command__omemo(const void *pointer, void *data,
                   struct t_gui_buffer *buffer, int argc,
                   char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;
    (void) argv_eol;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
    {
        weechat_printf(buffer,
                       _("%s%s: this command must be run in an XMPP account or channel buffer"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    // Handle subcommands
    if (argc > 1)
    {
        if (weechat_strcasecmp(argv[1], "check") == 0)
        {
            if (!ptr_account->omemo)
            {
                weechat_printf(buffer,
                               _("%s%s: OMEMO not initialized for this account"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                return WEECHAT_RC_OK;
            }

            // Show local state immediately (no async needed)
            ptr_account->omemo.show_status(
                buffer,
                ptr_account->name.data(),
                ptr_channel ? ptr_channel->name.data() : nullptr,
                ptr_channel ? ptr_channel->omemo.enabled : 0);

            // Also show own devices and peer device list if in a channel
            ptr_account->omemo.show_devices(buffer, ptr_account->jid().data());
            if (ptr_channel)
                ptr_account->omemo.show_devices(buffer, ptr_channel->name.data());

            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "republish") == 0)
        {
            if (!ptr_account->omemo)
            {
                weechat_printf(buffer,
                               _("%s%s: OMEMO not initialized for this account"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                return WEECHAT_RC_OK;
            }

            weechat_printf(buffer,
                           _("%sRepublishing OMEMO devicelist and bundle..."),
                           weechat_prefix("network"));

            // Publish devicelist
            xmpp_stanza_t *devicelist_stanza = ptr_account->get_devicelist();
            if (devicelist_stanza)
            {
                ptr_account->connection.send(devicelist_stanza);
                xmpp_stanza_release(devicelist_stanza);
                weechat_printf(buffer,
                               _("%sDevicelist published (device ID: %u)"),
                               weechat_prefix("network"), ptr_account->omemo.device_id);
            }

            // Publish bundle
            std::string from_s(ptr_account->jid());
            xmpp_stanza_t *bundle_stanza = ptr_account->omemo.get_bundle(
                ptr_account->context, from_s.data(), NULL);
            if (bundle_stanza)
            {
                ptr_account->connection.send(bundle_stanza);
                xmpp_stanza_release(bundle_stanza);
                weechat_printf(buffer,
                               _("%sBundle published for device %u"),
                               weechat_prefix("network"), ptr_account->omemo.device_id);
            }
            else
            {
                weechat_printf(buffer,
                               _("%s%s: failed to generate OMEMO bundle"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
            }

            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "reset-keys") == 0)
        {
            if (!ptr_account->omemo)
            {
                weechat_printf(buffer,
                               _("%s%s: OMEMO not initialized for this account"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                return WEECHAT_RC_OK;
            }

            weechat_printf(buffer,
                           _("%s%s: Resetting OMEMO key database to force renegotiation"),
                           weechat_prefix("info"), WEECHAT_XMPP_PLUGIN_NAME);

            try {
                // Clear OMEMO database
                if (ptr_account->omemo.db_env)
                {
                    lmdb::txn txn = lmdb::txn::begin(ptr_account->omemo.db_env);
                    mdb_drop(txn.handle(), ptr_account->omemo.dbi.omemo, 0);
                    txn.commit();
                    weechat_printf(buffer,
                                   _("%s%s: OMEMO key database cleared. Session keys will be renegotiated."),
                                   weechat_prefix("info"), WEECHAT_XMPP_PLUGIN_NAME);
                }
            } catch (const lmdb::error& ex) {
                weechat_printf(buffer,
                               _("%s%s: Failed to reset OMEMO keys: %s"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, ex.what());
            }

            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "status") == 0)
        {
            if (!ptr_account->omemo)
            {
                weechat_printf(buffer,
                               _("%s%s: OMEMO not initialized for this account"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                return WEECHAT_RC_OK;
            }

            ptr_account->omemo.show_status(
                buffer,
                ptr_account->name.data(),
                ptr_channel ? ptr_channel->name.data() : nullptr,
                ptr_channel ? ptr_channel->omemo.enabled : 0);

            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "fingerprint") == 0)
        {
            if (!ptr_account->omemo)
            {
                weechat_printf(buffer,
                               _("%s%s: OMEMO not initialized for this account"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                return WEECHAT_RC_OK;
            }
            // Optional argument: jid to look up peer fingerprint; no argument = own key
            const char *jid = (argc > 2) ? argv[2] : nullptr;
            ptr_account->omemo.show_fingerprint(buffer, jid);
            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "trust") == 0)
        {
            if (!ptr_account->omemo)
            {
                weechat_printf(buffer,
                               _("%s%s: OMEMO not initialized for this account"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                return WEECHAT_RC_OK;
            }
            if (argc < 3)
            {
                weechat_printf(buffer,
                               _("%s%s: usage: /omemo trust <jid>"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                return WEECHAT_RC_OK;
            }
            // Remove stored identity keys → next message triggers TOFU re-store
            ptr_account->omemo.distrust_jid(buffer, argv[2]);
            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "devices") == 0)
        {
            if (!ptr_account->omemo)
            {
                weechat_printf(buffer,
                               _("%s%s: OMEMO not initialized for this account"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                return WEECHAT_RC_OK;
            }
            // Optional argument: jid; default to channel JID or peer JID
            const char *jid = nullptr;
            if (argc > 2)
                jid = argv[2];
            else if (ptr_channel)
                jid = ptr_channel->name.data();
            if (!jid)
            {
                weechat_printf(buffer,
                               _("%s%s: usage: /omemo devices <jid>  (or run in a channel buffer)"),
                               weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                return WEECHAT_RC_OK;
            }
            ptr_account->omemo.show_devices(buffer, jid);
            return WEECHAT_RC_OK;
        }

        WEECHAT_COMMAND_ERROR;
    }

    // Default behavior: enable OMEMO for current channel
    if (!ptr_channel)
    {
        weechat_printf(
            buffer,
            _("%s%s: \"%s\" command requires a channel buffer or use /omemo republish on account buffer"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, "omemo");
        return WEECHAT_RC_OK;
    }

    ptr_channel->omemo.enabled = 1;
    ptr_channel->pgp.enabled = 0;

    ptr_channel->set_transport(weechat::channel::transport::OMEMO, 0);

    weechat_bar_item_update("xmpp_encryption");

    return WEECHAT_RC_OK;
}

int command__pgp(const void *pointer, void *data,
                 struct t_gui_buffer *buffer, int argc,
                 char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;
    char *keyid;

    (void) pointer;
    (void) data;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_channel)
    {
        weechat_printf(
            ptr_account->buffer,
            _("%s%s: \"%s\" command can not be executed on a account buffer"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, "pgp");
        return WEECHAT_RC_OK;
    }

    if (argc > 1)
    {
        if (weechat_strcasecmp(argv[1], "status") == 0)
        {
            weechat_printf(ptr_account->buffer,
                           _("%sPGP Status for channel %s:"),
                           weechat_prefix("info"), ptr_channel->name.data());
            weechat_printf(ptr_account->buffer,
                           _("%s  Encryption: %s"),
                           weechat_prefix("info"), ptr_channel->pgp.enabled ? "ENABLED" : "disabled");

            if (ptr_channel->pgp.ids.empty())
            {
                weechat_printf(ptr_account->buffer,
                               _("%s  No PGP keys configured"),
                               weechat_prefix("info"));
            }
            else
            {
                weechat_printf(ptr_account->buffer,
                               _("%s  Configured keys:"),
                               weechat_prefix("info"));
                for (const auto& key_id : ptr_channel->pgp.ids)
                {
                    weechat_printf(ptr_account->buffer,
                                   _("%s    - %s"),
                                   weechat_prefix("info"), key_id.data());
                }
            }

            return WEECHAT_RC_OK;
        }

        if (weechat_strcasecmp(argv[1], "reset") == 0)
        {
            if (ptr_channel->pgp.ids.empty())
            {
                weechat_printf(ptr_account->buffer,
                               _("%s%s: No PGP keys configured for this channel"),
                               weechat_prefix("info"), WEECHAT_XMPP_PLUGIN_NAME);
                return WEECHAT_RC_OK;
            }

            size_t count = ptr_channel->pgp.ids.size();
            ptr_channel->pgp.ids.clear();
            ptr_account->save_pgp_keys();
            weechat::config::write();

            weechat_printf(ptr_account->buffer,
                           _("%s%s: Removed %zu PGP key(s) from channel '%s'"),
                           weechat_prefix("info"), WEECHAT_XMPP_PLUGIN_NAME,
                           count, ptr_channel->name.data());

            return WEECHAT_RC_OK;
        }

        // Default: add key
        keyid = argv_eol[1];
        ptr_channel->pgp.ids.emplace(keyid);
        ptr_account->save_pgp_keys();
        weechat::config::write();

        weechat_printf(ptr_account->buffer,
                       _("%s%s: Added PGP key '%s' to channel '%s'"),
                       weechat_prefix("info"), WEECHAT_XMPP_PLUGIN_NAME,
                       keyid, ptr_channel->name.data());

        return WEECHAT_RC_OK;
    }

    // Default: enable PGP for current channel
    ptr_channel->omemo.enabled = 0;
    ptr_channel->pgp.enabled = 1;

    ptr_channel->set_transport(weechat::channel::transport::PGP, 0);

    weechat_bar_item_update("xmpp_encryption");

    return WEECHAT_RC_OK;
}

int command__plain(const void *pointer, void *data,
                   struct t_gui_buffer *buffer, int argc,
                   char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;
    (void) argc;
    (void) argv;
    (void) argv_eol;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_channel)
    {
        weechat_printf(
            ptr_account->buffer,
            _("%s%s: \"%s\" command can not be executed on a account buffer"),
            weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, "plain");
        return WEECHAT_RC_OK;
    }

    ptr_channel->omemo.enabled = 0;
    ptr_channel->pgp.enabled = 0;

    ptr_channel->set_transport(weechat::channel::transport::PLAIN, 0);

    weechat_bar_item_update("xmpp_encryption");

    return WEECHAT_RC_OK;
}

int command__xml(const void *pointer, void *data,
                 struct t_gui_buffer *buffer, int argc,
                 char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;
    xmpp_stanza_t *stanza;

    (void) pointer;
    (void) data;
    (void) argv;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_account->connected())
    {
        weechat_printf(buffer,
                        _("%s%s: you are not connected to server"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    if (argc > 1)
    {
        auto parse = [&](sexp::driver& sxml) {
            std::stringstream ss;
            std::string line;
            try {
                return sxml.parse(argv_eol[1], &ss);
            }
            catch (const std::invalid_argument& ex) {
                while (std::getline(ss, line))
                    weechat_printf(nullptr, "%ssxml: %s", weechat_prefix("info"), line.data());
                weechat_printf(nullptr, "%ssxml: %s", weechat_prefix("error"), ex.what());
                return false;
            }
        };
        if (sexp::driver sxml(ptr_account->context); parse(sxml))
        {
            for (auto *stanza : sxml.elements)
            {
                ptr_account->connection.send( stanza);
                xmpp_stanza_release(stanza);
            }
        }
        else
        {
            stanza = xmpp_stanza_new_from_string(ptr_account->context,
                                                argv_eol[1]);
            if (!stanza)
            {
                weechat_printf(nullptr, _("%s%s: Bad XML"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                return WEECHAT_RC_ERROR;
            }

            ptr_account->connection.send( stanza);
            xmpp_stanza_release(stanza);
        }
    }

    return WEECHAT_RC_OK;
}

int command__xmpp(const void *pointer, void *data,
                  struct t_gui_buffer *buffer, int argc,
                  char **argv, char **argv_eol)
{
    (void) pointer;
    (void) data;
    (void) buffer;
    (void) argc;
    (void) argv;
    (void) argv_eol;

    weechat_printf(nullptr,
                   _("%s%s %s [%s]"),
                   weechat_prefix("info"), WEECHAT_XMPP_PLUGIN_NAME,
                   WEECHAT_XMPP_PLUGIN_VERSION, XMPP_PLUGIN_COMMIT);

    return WEECHAT_RC_OK;
}

int command__trap(const void *pointer, void *data,
                  struct t_gui_buffer *buffer, int argc,
                  char **argv, char **argv_eol)
{
    (void) pointer;
    (void) data;
    (void) buffer;
    (void) argc;
    (void) argv;
    (void) argv_eol;

    weechat::account *account = NULL;
    weechat::channel *channel = NULL;

    buffer__get_account_and_channel(buffer, &account, &channel);
    weechat::user::search(account, account->jid_device().data());

    (void) channel; // trap is for debugging only

    __asm("int3");

    return WEECHAT_RC_OK;
}



int command__edit(const void *pointer, void *data,
                  struct t_gui_buffer *buffer, int argc,
                  char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;
    const char *text;

    (void) pointer;
    (void) data;
    (void) argv;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_channel)
    {
        weechat_printf(buffer, "%sxmpp: you must be in a channel to edit messages",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        weechat_printf(buffer, "%sxmpp: you are not connected to server",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    if (argc < 2)
    {
        weechat_printf(buffer, "%sxmpp: missing message text",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    text = argv_eol[1];

    // Find the last message sent by us
    struct t_hdata *hdata_line = weechat_hdata_get("line");
    struct t_hdata *hdata_line_data = weechat_hdata_get("line_data");
    struct t_gui_lines *own_lines = (struct t_gui_lines*)weechat_hdata_pointer(
        weechat_hdata_get("buffer"), buffer, "own_lines");
    
    if (!own_lines)
    {
        weechat_printf(buffer, "%sxmpp: cannot access buffer lines",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    struct t_gui_line *line = (struct t_gui_line*)weechat_hdata_pointer(
        hdata_line, own_lines, "last_line");
    
    std::string last_msg_id;
    
    // Search backwards for our last sent message
    while (line && last_msg_id.empty())
    {
        struct t_gui_line_data *line_data = (struct t_gui_line_data*)weechat_hdata_pointer(
            hdata_line, line, "data");
        
        if (line_data)
        {
            const char *tags = (const char*)weechat_hdata_string(hdata_line_data, line_data, "tags");
            
            // Check if this is our message (has self_msg tag and id_ tag)
            if (tags && strstr(tags, "self_msg") && strstr(tags, "id_"))
            {
                // Extract the message ID from tags
                char **tag_array = weechat_string_split(tags, ",", NULL, 0, 0, NULL);
                if (tag_array)
                {
                    for (int i = 0; tag_array[i]; i++)
                    {
                        if (strncmp(tag_array[i], "id_", 3) == 0)
                        {
                            last_msg_id = tag_array[i] + 3;
                            break;
                        }
                    }
                    weechat_string_free_split(tag_array);
                }
                break;
            }
        }
        
        line = (struct t_gui_line*)weechat_hdata_move(hdata_line, line, -1);
    }
    
    if (last_msg_id.empty())
    {
        weechat_printf(buffer, "%sxmpp: no message found to edit",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    // Send message correction
    xmpp_stanza_t *message = xmpp_message_new(ptr_account->context,
                    ptr_channel->type == weechat::channel::chat_type::MUC
                    ? "groupchat" : "chat",
                    ptr_channel->id.data(), NULL);

    xmpp_string_guard id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
    const char *id = id_g.ptr;
    xmpp_stanza_set_id(message, id);
    // freed by id_g
    xmpp_message_set_body(message, text);

    // Add replace element with original message ID
    xmpp_stanza_t *replace = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(replace, "replace");
    xmpp_stanza_set_ns(replace, "urn:xmpp:message-correct:0");
    xmpp_stanza_set_id(replace, last_msg_id.c_str());
    xmpp_stanza_add_child(message, replace);
    xmpp_stanza_release(replace);

    xmpp_stanza_t *message__store = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(message__store, "store");
    xmpp_stanza_set_ns(message__store, "urn:xmpp:hints");
    xmpp_stanza_add_child(message, message__store);
    xmpp_stanza_release(message__store);

    ptr_account->connection.send( message);
    xmpp_stanza_release(message);
    // last_msg_id freed by std::string destructor

    weechat_printf(buffer, "%sxmpp: message edit sent",
                  weechat_prefix("network"));

    return WEECHAT_RC_OK;
}

int command__retract(const void *pointer, void *data,
                     struct t_gui_buffer *buffer, int argc,
                     char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;
    (void) argv;
    (void) argv_eol;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_channel)
    {
        weechat_printf(buffer, "%sxmpp: you must be in a channel to retract messages",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        weechat_printf(buffer, "%sxmpp: you are not connected to server",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    if (argc > 2)
    {
        weechat_printf(buffer, "%sxmpp: too many arguments (use /retract with no arguments)",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    // Find the last message sent by us
    struct t_hdata *hdata_line = weechat_hdata_get("line");
    struct t_hdata *hdata_line_data = weechat_hdata_get("line_data");
    struct t_gui_lines *own_lines = (struct t_gui_lines*)weechat_hdata_pointer(
        weechat_hdata_get("buffer"), buffer, "own_lines");
    
    if (!own_lines)
    {
        weechat_printf(buffer, "%sxmpp: cannot access buffer lines",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    struct t_gui_line *line = (struct t_gui_line*)weechat_hdata_pointer(
        hdata_line, own_lines, "last_line");
    
    std::string last_msg_id;
    
    // Search backwards for our last sent message
    while (line && last_msg_id.empty())
    {
        struct t_gui_line_data *line_data = (struct t_gui_line_data*)weechat_hdata_pointer(
            hdata_line, line, "data");
        
        if (line_data)
        {
            const char *tags = (const char*)weechat_hdata_string(hdata_line_data, line_data, "tags");
            
            // Check if this is our message (has self_msg tag and id_ tag)
            if (tags && strstr(tags, "self_msg") && strstr(tags, "id_"))
            {
                // Extract the message ID from tags
                char **tag_array = weechat_string_split(tags, ",", NULL, 0, 0, NULL);
                if (tag_array)
                {
                    for (int i = 0; tag_array[i]; i++)
                    {
                        if (strncmp(tag_array[i], "id_", 3) == 0)
                        {
                            last_msg_id = tag_array[i] + 3;
                            break;
                        }
                    }
                    weechat_string_free_split(tag_array);
                }
                break;
            }
        }
        
        line = (struct t_gui_line*)weechat_hdata_move(hdata_line, line, -1);
    }
    
    if (last_msg_id.empty())
    {
        weechat_printf(buffer, "%sxmpp: no message found to retract",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    // Send message retraction (XEP-0424)
    xmpp_stanza_t *message = xmpp_message_new(ptr_account->context,
                    ptr_channel->type == weechat::channel::chat_type::MUC
                    ? "groupchat" : "chat",
                    ptr_channel->id.data(), NULL);

    xmpp_string_guard id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
    const char *id = id_g.ptr;
    xmpp_stanza_set_id(message, id);
    // freed by id_g

    // Add retract element with original message ID
    xmpp_stanza_t *retract = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(retract, "retract");
    xmpp_stanza_set_ns(retract, "urn:xmpp:message-retract:1");
    xmpp_stanza_set_attribute(retract, "id", last_msg_id.c_str());
    xmpp_stanza_add_child(message, retract);
    xmpp_stanza_release(retract);

    // Add fallback body for clients that don't support XEP-0424
    xmpp_stanza_t *body = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(body, "body");
    xmpp_stanza_t *body_text = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_text(body_text, "This message was deleted");
    xmpp_stanza_add_child(body, body_text);
    xmpp_stanza_release(body_text);
    xmpp_stanza_add_child(message, body);
    xmpp_stanza_release(body);

    // Add fallback element to indicate body is fallback
    xmpp_stanza_t *fallback = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(fallback, "fallback");
    xmpp_stanza_set_ns(fallback, "urn:xmpp:fallback:0");
    xmpp_stanza_set_attribute(fallback, "for", "urn:xmpp:message-retract:1");
    xmpp_stanza_add_child(message, fallback);
    xmpp_stanza_release(fallback);

    // Add store hint for MAM
    xmpp_stanza_t *message__store = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(message__store, "store");
    xmpp_stanza_set_ns(message__store, "urn:xmpp:hints");
    xmpp_stanza_add_child(message, message__store);
    xmpp_stanza_release(message__store);

    ptr_account->connection.send(message);
    xmpp_stanza_release(message);
    // last_msg_id freed by std::string destructor

    weechat_printf(buffer, "%sxmpp: message retraction sent",
                  weechat_prefix("network"));

    return WEECHAT_RC_OK;
}

int command__react(const void *pointer, void *data,
                   struct t_gui_buffer *buffer, int argc,
                   char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;
    (void) argv;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_channel)
    {
        weechat_printf(buffer, "%sxmpp: you must be in a channel to react to messages",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        weechat_printf(buffer, "%sxmpp: you are not connected to server",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    if (argc < 2)
    {
        weechat_printf(buffer, "%sxmpp: missing emoji reaction",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    const char *emoji = argv_eol[1];

    // Find the last message in buffer (not from us)
    struct t_hdata *hdata_line = weechat_hdata_get("line");
    struct t_hdata *hdata_line_data = weechat_hdata_get("line_data");
    struct t_gui_lines *own_lines = (struct t_gui_lines*)weechat_hdata_pointer(
        weechat_hdata_get("buffer"), buffer, "own_lines");
    
    if (!own_lines)
    {
        weechat_printf(buffer, "%sxmpp: cannot access buffer lines",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    struct t_gui_line *line = (struct t_gui_line*)weechat_hdata_pointer(
        hdata_line, own_lines, "last_line");
    
    std::string target_msg_id;
    
    // Search backwards for last message with an ID (skip our own messages)
    while (line && target_msg_id.empty())
    {
        struct t_gui_line_data *line_data = (struct t_gui_line_data*)weechat_hdata_pointer(
            hdata_line, line, "data");
        
        if (line_data)
        {
            const char *tags = (const char*)weechat_hdata_string(hdata_line_data, line_data, "tags");
            
            // Look for messages with ID that aren't from us
            if (tags && strstr(tags, "id_") && !strstr(tags, "self_msg"))
            {
                // Extract the message ID from tags
                char **tag_array = weechat_string_split(tags, ",", NULL, 0, 0, NULL);
                if (tag_array)
                {
                    for (int i = 0; tag_array[i]; i++)
                    {
                        if (strncmp(tag_array[i], "id_", 3) == 0)
                        {
                            target_msg_id = tag_array[i] + 3;
                            break;
                        }
                    }
                    weechat_string_free_split(tag_array);
                }
                break;
            }
        }
        
        line = (struct t_gui_line*)weechat_hdata_move(hdata_line, line, -1);
    }
    
    if (target_msg_id.empty())
    {
        weechat_printf(buffer, "%sxmpp: no message found to react to",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    // Send reaction (XEP-0444)
    xmpp_stanza_t *message = xmpp_message_new(ptr_account->context,
                    ptr_channel->type == weechat::channel::chat_type::MUC
                    ? "groupchat" : "chat",
                    ptr_channel->id.data(), NULL);

    xmpp_string_guard msg_id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
    const char *msg_id = msg_id_g.ptr;
    xmpp_stanza_set_id(message, msg_id);
    // freed by msg_id_g

    // Add reactions element
    xmpp_stanza_t *reactions = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(reactions, "reactions");
    xmpp_stanza_set_ns(reactions, "urn:xmpp:reactions:0");
    xmpp_stanza_set_attribute(reactions, "id", target_msg_id.c_str());
    
    // Add reaction element with emoji
    xmpp_stanza_t *reaction = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(reaction, "reaction");
    xmpp_stanza_t *reaction_text = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_text(reaction_text, emoji);
    xmpp_stanza_add_child(reaction, reaction_text);
    xmpp_stanza_release(reaction_text);
    
    xmpp_stanza_add_child(reactions, reaction);
    xmpp_stanza_release(reaction);
    
    xmpp_stanza_add_child(message, reactions);
    xmpp_stanza_release(reactions);

    // Add store hint for MAM
    xmpp_stanza_t *store_hint = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(store_hint, "store");
    xmpp_stanza_set_ns(store_hint, "urn:xmpp:hints");
    xmpp_stanza_add_child(message, store_hint);
    xmpp_stanza_release(store_hint);

    ptr_account->connection.send(message);
    xmpp_stanza_release(message);
    // target_msg_id freed by std::string destructor

    weechat_printf(buffer, "%sxmpp: reaction %s sent",
                  weechat_prefix("network"), emoji);

    return WEECHAT_RC_OK;
}

int command__reply(const void *pointer, void *data,
                   struct t_gui_buffer *buffer, int argc,
                   char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;
    (void) argv;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_channel)
    {
        weechat_printf(buffer, "%sxmpp: you must be in a channel to reply to messages",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        weechat_printf(buffer, "%sxmpp: you are not connected to server",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    if (argc < 2)
    {
        weechat_printf(buffer, "%sxmpp: missing reply message",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    const char *reply_text = argv_eol[1];

    // Find the last message in buffer (not from us)
    void *lines = weechat_hdata_pointer(weechat_hdata_get("buffer"),
                                        buffer, "lines");
    if (!lines)
    {
        weechat_printf(buffer, "%sxmpp: no lines found in buffer",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    void *last_line = weechat_hdata_pointer(weechat_hdata_get("lines"),
                                            lines, "last_line");
    const char *target_id = NULL;
    std::string own_nick = std::string(ptr_account->jid());

    while (last_line)
    {
        void *line_data = weechat_hdata_pointer(weechat_hdata_get("line"),
                                                last_line, "data");
        if (line_data)
        {
            // Check if this message is not from us
            int tags_count = weechat_hdata_integer(weechat_hdata_get("line_data"),
                                                   line_data, "tags_count");
            bool from_self = false;
            char str_tag[24] = {0};
            
            for (int n_tag = 0; n_tag < tags_count; n_tag++)
            {
                snprintf(str_tag, sizeof(str_tag), "%d|tags_array", n_tag);
                const char *tag = weechat_hdata_string(weechat_hdata_get("line_data"),
                                                       line_data, str_tag);
                
                // Check if message is from self
                if (strlen(tag) > strlen("nick_") &&
                    strncmp(tag, "nick_", strlen("nick_")) == 0)
                {
                    const char *nick = tag + strlen("nick_");
                    if (weechat_strcasecmp(nick, own_nick.c_str()) == 0)
                    {
                        from_self = true;
                        break;
                    }
                }
            }
            
            if (!from_self)
            {
                // Found a message not from us - extract its ID
                for (int n_tag = 0; n_tag < tags_count; n_tag++)
                {
                    snprintf(str_tag, sizeof(str_tag), "%d|tags_array", n_tag);
                    const char *tag = weechat_hdata_string(weechat_hdata_get("line_data"),
                                                           line_data, str_tag);
                    if (strlen(tag) > strlen("id_") &&
                        strncmp(tag, "id_", strlen("id_")) == 0)
                    {
                        target_id = tag + strlen("id_");
                        break;
                    }
                }
                
                if (target_id)
                    break;
            }
        }

        last_line = weechat_hdata_pointer(weechat_hdata_get("line"),
                                          last_line, "prev_line");
    }

    if (!target_id)
    {
        weechat_printf(buffer, "%sxmpp: no message found to reply to",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    // Send the reply using XEP-0461
    // The message goes to the channel with a <reply> element
    const char *to = ptr_channel->name.c_str();
    const char *type = (ptr_channel->type == weechat::channel::chat_type::MUC) 
                        ? "groupchat" : "chat";

    xmpp_stanza_t *message = xmpp_message_new(ptr_account->context, type, to, NULL);
    xmpp_string_guard uuid_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
    const char *uuid = uuid_g.ptr;
    xmpp_stanza_set_id(message, uuid);
    // freed by uuid_g

    // Add <body> with reply text
    xmpp_stanza_t *body = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(body, "body");
    xmpp_stanza_t *body_text = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_text(body_text, reply_text);
    xmpp_stanza_add_child(body, body_text);
    xmpp_stanza_add_child(message, body);

    // Add <reply xmlns='urn:xmpp:reply:0' id='target-id' />
    xmpp_stanza_t *reply_elem = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(reply_elem, "reply");
    xmpp_stanza_set_ns(reply_elem, "urn:xmpp:reply:0");
    xmpp_stanza_set_attribute(reply_elem, "id", target_id);
    xmpp_stanza_set_attribute(reply_elem, "to", to);
    xmpp_stanza_add_child(message, reply_elem);

    // Add store hint for MAM
    xmpp_stanza_t *store_hint = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(store_hint, "store");
    xmpp_stanza_set_ns(store_hint, "urn:xmpp:hints");
    xmpp_stanza_add_child(message, store_hint);

    // Add origin-id for XEP-0359
    xmpp_stanza_t *origin_id = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(origin_id, "origin-id");
    xmpp_stanza_set_ns(origin_id, "urn:xmpp:sid:0");
    xmpp_string_guard origin_uuid_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
    const char *origin_uuid = origin_uuid_g.ptr;
    xmpp_stanza_set_attribute(origin_id, "id", origin_uuid);
    // freed by origin_uuid_g
    xmpp_stanza_add_child(message, origin_id);

    ptr_account->connection.send(message);

    xmpp_stanza_release(body_text);
    xmpp_stanza_release(body);
    xmpp_stanza_release(reply_elem);
    xmpp_stanza_release(store_hint);
    xmpp_stanza_release(origin_id);
    xmpp_stanza_release(message);

    weechat_printf(buffer, "%sxmpp: reply sent",
                  weechat_prefix("network"));

    return WEECHAT_RC_OK;
}

int command__moderate(const void *pointer, void *data,
                      struct t_gui_buffer *buffer, int argc,
                      char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;
    (void) argv;
    (void) argv_eol;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_channel)
    {
        weechat_printf(buffer, "%sxmpp: you must be in a MUC channel to moderate messages",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    // XEP-0425 is only for MUC moderation
    if (ptr_channel->type != weechat::channel::chat_type::MUC)
    {
        weechat_printf(buffer, "%sxmpp: message moderation is only available in MUC rooms",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        weechat_printf(buffer, "%sxmpp: you are not connected to server",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    // Optional: reason for moderation
    const char *reason = (argc > 1) ? argv_eol[1] : NULL;

    // Find the last message in buffer (from anyone, including self)
    void *lines = weechat_hdata_pointer(weechat_hdata_get("buffer"),
                                        buffer, "lines");
    if (!lines)
    {
        weechat_printf(buffer, "%sxmpp: no lines found in buffer",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    void *last_line = weechat_hdata_pointer(weechat_hdata_get("lines"),
                                            lines, "last_line");
    const char *target_id = NULL;

    while (last_line)
    {
        void *line_data = weechat_hdata_pointer(weechat_hdata_get("line"),
                                                last_line, "data");
        if (line_data)
        {
            // Extract message ID from tags
            int tags_count = weechat_hdata_integer(weechat_hdata_get("line_data"),
                                                   line_data, "tags_count");
            char str_tag[24] = {0};
            for (int n_tag = 0; n_tag < tags_count; n_tag++)
            {
                snprintf(str_tag, sizeof(str_tag), "%d|tags_array", n_tag);
                const char *tag = weechat_hdata_string(weechat_hdata_get("line_data"),
                                                       line_data, str_tag);
                
                // Skip retracted messages
                if (weechat_strcasecmp(tag, "xmpp_retracted") == 0)
                    break;
                
                if (strlen(tag) > strlen("id_") &&
                    strncmp(tag, "id_", strlen("id_")) == 0)
                {
                    target_id = tag + strlen("id_");
                    break;
                }
            }
            
            if (target_id)
                break;
        }

        last_line = weechat_hdata_pointer(weechat_hdata_get("line"),
                                          last_line, "prev_line");
    }

    if (!target_id)
    {
        weechat_printf(buffer, "%sxmpp: no message found to moderate",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    // Send XEP-0425 moderation request (IQ stanza to the MUC service)
    const char *room_jid = ptr_channel->id.data();
    
    xmpp_stanza_t *iq = xmpp_iq_new(ptr_account->context, "set", NULL);
    xmpp_stanza_set_to(iq, room_jid);
    
    // <apply-to xmlns='urn:xmpp:fasten:0' id='target-message-id'>
    xmpp_stanza_t *apply_to = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(apply_to, "apply-to");
    xmpp_stanza_set_ns(apply_to, "urn:xmpp:fasten:0");
    xmpp_stanza_set_attribute(apply_to, "id", target_id);
    
    // <moderate xmlns='urn:xmpp:message-moderate:1'>
    xmpp_stanza_t *moderate = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(moderate, "moderate");
    xmpp_stanza_set_ns(moderate, "urn:xmpp:message-moderate:1");
    
    // <retract xmlns='urn:xmpp:message-retract:1'/>
    xmpp_stanza_t *retract = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(retract, "retract");
    xmpp_stanza_set_ns(retract, "urn:xmpp:message-retract:1");
    xmpp_stanza_add_child(moderate, retract);
    
    // Optional: <reason>text</reason>
    if (reason)
    {
        xmpp_stanza_t *reason_elem = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(reason_elem, "reason");
        xmpp_stanza_t *reason_text = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_text(reason_text, reason);
        xmpp_stanza_add_child(reason_elem, reason_text);
        xmpp_stanza_add_child(moderate, reason_elem);
        xmpp_stanza_release(reason_text);
        xmpp_stanza_release(reason_elem);
    }
    
    xmpp_stanza_add_child(apply_to, moderate);
    xmpp_stanza_add_child(iq, apply_to);
    
    ptr_account->connection.send(iq);
    
    xmpp_stanza_release(retract);
    xmpp_stanza_release(moderate);
    xmpp_stanza_release(apply_to);
    xmpp_stanza_release(iq);

    weechat_printf(buffer, "%sxmpp: moderation request sent%s",
                  weechat_prefix("network"),
                  reason ? " with reason" : "");

    return WEECHAT_RC_OK;
}

int command__ping(const void *pointer, void *data,
                  struct t_gui_buffer *buffer, int argc,
                  char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;
    xmpp_stanza_t *iq;
    const char *target = NULL;

    (void) pointer;
    (void) data;
    (void) argv_eol;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_account->connected())
    {
        weechat_printf(buffer, "%sxmpp: you are not connected to server",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    // Determine ping target: specified argument, channel JID, or server
    if (argc > 1)
        target = argv[1];
    else if (ptr_channel)
        target = ptr_channel->id.data();
    else
        target = NULL;  // Ping the server

    // Create ping IQ
    xmpp_string_guard id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
    const char *id = id_g.ptr;
    iq = xmpp_iq_new(ptr_account->context, "get", id);
    
    // Track ping time for response measurement
    ptr_account->user_ping_queries[id] = time(NULL);
    
    if (target)
    {
        xmpp_stanza_set_to(iq, target);
        weechat_printf(buffer, "%sSending ping to %s...",
                       weechat_prefix("network"), target);
    }
    else
    {
        weechat_printf(buffer, "%sSending ping to server...",
                       weechat_prefix("network"));
    }
    
    xmpp_stanza_t *ping = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(ping, "ping");
    xmpp_stanza_set_ns(ping, "urn:xmpp:ping");
    
    xmpp_stanza_add_child(iq, ping);
    xmpp_stanza_release(ping);
    
    ptr_account->connection.send( iq);
    xmpp_stanza_release(iq);
    // freed by id_g

    return WEECHAT_RC_OK;
}

int command__mood(const void *pointer, void *data,
                  struct t_gui_buffer *buffer, int argc,
                  char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;
    (void) argv;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_account->connected())
    {
        weechat_printf(buffer, "%sxmpp: you are not connected to server",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    // XEP-0107 mood values (from spec)
    const char *valid_moods[] = {
        "afraid", "amazed", "angry", "amorous", "annoyed", "anxious",
        "aroused", "ashamed", "bored", "brave", "calm", "cautious",
        "cold", "confident", "confused", "contemplative", "contented",
        "cranky", "crazy", "creative", "curious", "dejected", "depressed",
        "disappointed", "disgusted", "dismayed", "distracted", "embarrassed",
        "envious", "excited", "flirtatious", "frustrated", "grumpy", "guilty",
        "happy", "hopeful", "hot", "humbled", "humiliated", "hungry",
        "hurt", "impressed", "in_awe", "in_love", "indignant", "interested",
        "intoxicated", "invincible", "jealous", "lonely", "lucky", "mean",
        "moody", "nervous", "neutral", "offended", "outraged", "playful",
        "proud", "relaxed", "relieved", "remorseful", "restless", "sad",
        "sarcastic", "serious", "shocked", "shy", "sick", "sleepy",
        "spontaneous", "stressed", "strong", "surprised", "thankful",
        "thirsty", "tired", "undefined", "weak", "worried", NULL
    };

    const char *mood = NULL;
    const char *text = NULL;

    if (argc >= 2)
    {
        mood = argv[1];
        if (argc >= 3)
            text = argv_eol[2];
    }

    // Build PEP mood publish stanza
    // <iq type='set' id='...'>
    //   <pubsub xmlns='http://jabber.org/protocol/pubsub'>
    //     <publish node='http://jabber.org/protocol/mood'>
    //       <item>
    //         <mood xmlns='http://jabber.org/protocol/mood'>
    //           <happy/>  <!-- or other mood -->
    //           <text>Feeling great!</text>  <!-- optional -->
    //         </mood>
    //       </item>
    //     </publish>
    //   </pubsub>
    // </iq>

    xmpp_string_guard id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
    const char *id = id_g.ptr;
    xmpp_stanza_t *iq = xmpp_iq_new(ptr_account->context, "set", id);
    // freed by id_g

    // <pubsub xmlns='http://jabber.org/protocol/pubsub'>
    xmpp_stanza_t *pubsub = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(pubsub, "pubsub");
    xmpp_stanza_set_ns(pubsub, "http://jabber.org/protocol/pubsub");

    // <publish node='http://jabber.org/protocol/mood'>
    xmpp_stanza_t *publish = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(publish, "publish");
    xmpp_stanza_set_attribute(publish, "node", "http://jabber.org/protocol/mood");

    // <item>
    xmpp_stanza_t *item = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(item, "item");

    // <mood xmlns='http://jabber.org/protocol/mood'>
    xmpp_stanza_t *mood_elem = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(mood_elem, "mood");
    xmpp_stanza_set_ns(mood_elem, "http://jabber.org/protocol/mood");

    if (mood)
    {
        // Validate mood
        bool valid = false;
        for (int i = 0; valid_moods[i] != NULL; i++)
        {
            if (weechat_strcasecmp(mood, valid_moods[i]) == 0)
            {
                valid = true;
                break;
            }
        }

        if (!valid)
        {
            weechat_printf(buffer, "%sxmpp: invalid mood '%s'", 
                          weechat_prefix("error"), mood);
            weechat_printf(buffer, "%sValid moods: happy, sad, angry, excited, tired, etc.",
                          weechat_prefix("error"));
            
            xmpp_stanza_release(mood_elem);
            xmpp_stanza_release(item);
            xmpp_stanza_release(publish);
            xmpp_stanza_release(pubsub);
            xmpp_stanza_release(iq);
            return WEECHAT_RC_OK;
        }

        // Add mood element (e.g., <happy/>)
        xmpp_stanza_t *mood_value = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(mood_value, mood);
        xmpp_stanza_add_child(mood_elem, mood_value);
        xmpp_stanza_release(mood_value);

        // Add optional text
        if (text)
        {
            xmpp_stanza_t *text_elem = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(text_elem, "text");
            xmpp_stanza_t *text_content = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_text(text_content, text);
            xmpp_stanza_add_child(text_elem, text_content);
            xmpp_stanza_add_child(mood_elem, text_elem);
            xmpp_stanza_release(text_content);
            xmpp_stanza_release(text_elem);
        }
    }
    // If no mood specified, publish empty <mood/> to clear

    xmpp_stanza_add_child(item, mood_elem);
    xmpp_stanza_add_child(publish, item);
    xmpp_stanza_add_child(pubsub, publish);
    xmpp_stanza_add_child(iq, pubsub);

    ptr_account->connection.send(iq);

    xmpp_stanza_release(mood_elem);
    xmpp_stanza_release(item);
    xmpp_stanza_release(publish);
    xmpp_stanza_release(pubsub);
    xmpp_stanza_release(iq);

    if (mood)
    {
        if (text)
            weechat_printf(buffer, "%sxmpp: mood set to '%s': %s",
                          weechat_prefix("network"), mood, text);
        else
            weechat_printf(buffer, "%sxmpp: mood set to '%s'",
                          weechat_prefix("network"), mood);
    }
    else
    {
        weechat_printf(buffer, "%sxmpp: mood cleared",
                      weechat_prefix("network"));
    }

    return WEECHAT_RC_OK;
}

int command__activity(const void *pointer, void *data,
                      struct t_gui_buffer *buffer, int argc,
                      char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;
    (void) argv;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_account->connected())
    {
        weechat_printf(buffer, "%sxmpp: you are not connected to server",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    // XEP-0108 activity categories and specific activities
    // Format: category/specific (e.g., "working/coding")
    const char *valid_categories[] = {
        "doing_chores", "drinking", "eating", "exercising", "grooming",
        "having_appointment", "inactive", "relaxing", "talking", "traveling",
        "working", NULL
    };

    std::string category_s;
    std::string specific_s;
    const char *specific = NULL;
    const char *text = NULL;

    if (argc >= 2)
    {
        // Parse "category" or "category/specific"
        category_s = argv[1];
        std::string::size_type slash_pos = category_s.find('/');
        
        if (slash_pos != std::string::npos)
        {
            specific_s = category_s.substr(slash_pos + 1);
            category_s.resize(slash_pos);
            specific = specific_s.c_str();
        }
        
        if (argc >= 3)
            text = argv_eol[2];
    }

    const char *category = category_s.empty() ? NULL : category_s.c_str();

    // Build PEP activity publish stanza
    // <iq type='set'>
    //   <pubsub xmlns='http://jabber.org/protocol/pubsub'>
    //     <publish node='http://jabber.org/protocol/activity'>
    //       <item>
    //         <activity xmlns='http://jabber.org/protocol/activity'>
    //           <working>  <!-- category -->
    //             <coding/>  <!-- specific, optional -->
    //           </working>
    //           <text>Writing XMPP code</text>  <!-- optional -->
    //         </activity>
    //       </item>
    //     </publish>
    //   </pubsub>
    // </iq>

    xmpp_string_guard id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
    const char *id = id_g.ptr;
    xmpp_stanza_t *iq = xmpp_iq_new(ptr_account->context, "set", id);
    // freed by id_g

    xmpp_stanza_t *pubsub = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(pubsub, "pubsub");
    xmpp_stanza_set_ns(pubsub, "http://jabber.org/protocol/pubsub");

    xmpp_stanza_t *publish = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(publish, "publish");
    xmpp_stanza_set_attribute(publish, "node", "http://jabber.org/protocol/activity");

    xmpp_stanza_t *item = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(item, "item");

    xmpp_stanza_t *activity_elem = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(activity_elem, "activity");
    xmpp_stanza_set_ns(activity_elem, "http://jabber.org/protocol/activity");

    if (category)
    {
        // Validate category
        bool valid = false;
        for (int i = 0; valid_categories[i] != NULL; i++)
        {
            if (weechat_strcasecmp(category, valid_categories[i]) == 0)
            {
                valid = true;
                break;
            }
        }

        if (!valid)
        {
            weechat_printf(buffer, "%sxmpp: invalid activity category '%s'", 
                          weechat_prefix("error"), category);
            weechat_printf(buffer, "%sValid categories: working, relaxing, eating, drinking, traveling, etc.",
                          weechat_prefix("error"));
            
            // category_s freed by std::string destructor
            xmpp_stanza_release(activity_elem);
            xmpp_stanza_release(item);
            xmpp_stanza_release(publish);
            xmpp_stanza_release(pubsub);
            xmpp_stanza_release(iq);
            return WEECHAT_RC_OK;
        }

        // Add category element
        xmpp_stanza_t *category_elem = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(category_elem, category);

        // Add specific activity if provided
        if (specific)
        {
            xmpp_stanza_t *specific_elem = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(specific_elem, specific);
            xmpp_stanza_add_child(category_elem, specific_elem);
            xmpp_stanza_release(specific_elem);
        }

        xmpp_stanza_add_child(activity_elem, category_elem);
        xmpp_stanza_release(category_elem);

        // Add optional text
        if (text)
        {
            xmpp_stanza_t *text_elem = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(text_elem, "text");
            xmpp_stanza_t *text_content = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_text(text_content, text);
            xmpp_stanza_add_child(text_elem, text_content);
            xmpp_stanza_add_child(activity_elem, text_elem);
            xmpp_stanza_release(text_content);
            xmpp_stanza_release(text_elem);
        }

        // category_s freed by std::string destructor
    }
    // If no activity specified, publish empty <activity/> to clear

    xmpp_stanza_add_child(item, activity_elem);
    xmpp_stanza_add_child(publish, item);
    xmpp_stanza_add_child(pubsub, publish);
    xmpp_stanza_add_child(iq, pubsub);

    ptr_account->connection.send(iq);

    xmpp_stanza_release(activity_elem);
    xmpp_stanza_release(item);
    xmpp_stanza_release(publish);
    xmpp_stanza_release(pubsub);
    xmpp_stanza_release(iq);

    if (category)
    {
        if (specific && text)
            weechat_printf(buffer, "%sxmpp: activity set to '%s/%s': %s",
                          weechat_prefix("network"), argv[1], specific, text);
        else if (specific)
            weechat_printf(buffer, "%sxmpp: activity set to '%s/%s'",
                          weechat_prefix("network"), argv[1], specific);
        else if (text)
            weechat_printf(buffer, "%sxmpp: activity set to '%s': %s",
                          weechat_prefix("network"), argv[1], text);
        else
            weechat_printf(buffer, "%sxmpp: activity set to '%s'",
                          weechat_prefix("network"), argv[1]);
    }
    else
    {
        weechat_printf(buffer, "%sxmpp: activity cleared",
                      weechat_prefix("network"));
    }

    return WEECHAT_RC_OK;
}

int command__selfping(const void *pointer, void *data,
                     struct t_gui_buffer *buffer, int argc,
                     char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;
    xmpp_stanza_t *iq;

    (void) pointer;
    (void) data;
    (void) argv;
    (void) argv_eol;
    (void) argc;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_account->connected())
    {
        weechat_printf(buffer, "%sxmpp: you are not connected to server",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    if (!ptr_channel)
    {
        weechat_printf(buffer, "%s%s: this command must be used in a MUC buffer",
                      weechat_prefix("error"),
                      argv[0]);
        return WEECHAT_RC_OK;
    }

    // Check if this is a MUC channel
    const char *buffer_type = weechat_buffer_get_string(ptr_channel->buffer, "localvar_type");
    if (!buffer_type || strcmp(buffer_type, "channel") != 0)
    {
        weechat_printf(buffer, "%s%s: this command must be used in a MUC buffer",
                      weechat_prefix("error"),
                      argv[0]);
        return WEECHAT_RC_OK;
    }

    // Construct our full MUC JID (room@server/nickname)
    std::string muc_jid = std::string(ptr_channel->id) + "/" + std::string(ptr_account->nickname());

    // Send self-ping to our own MUC nickname
    xmpp_string_guard iq_id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
    iq = xmpp_iq_new(ptr_account->context, "get", iq_id_g.ptr);
    xmpp_stanza_set_to(iq, muc_jid.c_str());
    
    xmpp_stanza_t *ping = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(ping, "ping");
    xmpp_stanza_set_ns(ping, "urn:xmpp:ping");
    
    xmpp_stanza_add_child(iq, ping);
    xmpp_stanza_release(ping);

    weechat_printf(buffer, "%sSending MUC self-ping to %s...",
                   weechat_prefix("network"), muc_jid.c_str());

    ptr_account->connection.send(iq);
    xmpp_stanza_release(iq);

    return WEECHAT_RC_OK;
}

int command__whois(const void *pointer, void *data,
                  struct t_gui_buffer *buffer, int argc,
                  char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;
    const char *target = NULL;

    (void) pointer;
    (void) data;
    (void) argv_eol;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_account->connected())
    {
        weechat_printf(buffer, "%sxmpp: you are not connected to server",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    // Determine target: specified argument or current channel
    if (argc > 1)
        target = argv[1];
    else if (ptr_channel && ptr_channel->type == weechat::channel::chat_type::PM)
        target = ptr_channel->id.data();
    else
    {
        weechat_printf(buffer, "%s%s: missing JID argument",
                      weechat_prefix("error"),
                      argv[0]);
        return WEECHAT_RC_OK;
    }

    // Request vCard using XEP-0054 (legacy)
    xmpp_stanza_t *iq = xmpp::xep0054::vcard_request(ptr_account->context, target);
    const char *req_id = xmpp_stanza_get_id(iq);
    if (req_id)
        ptr_account->whois_queries[req_id] = { buffer, std::string(target) };

    // Also request vCard4 via PubSub (XEP-0292) — servers that support it will
    // respond; others will return an error which we silently ignore.
    xmpp_stanza_t *iq4 = xmpp::xep0292::vcard4_request(ptr_account->context, target);
    const char *req_id4 = xmpp_stanza_get_id(iq4);
    if (req_id4)
        ptr_account->whois_queries[req_id4] = { buffer, std::string(target) };

    weechat_printf(buffer, "%sRequesting vCard for %s...",
                   weechat_prefix("network"), target);

    ptr_account->connection.send(iq);
    xmpp_stanza_release(iq);
    ptr_account->connection.send(iq4);
    xmpp_stanza_release(iq4);

    return WEECHAT_RC_OK;
}

// /setvcard field value
// Sets a single field of your own vCard (XEP-0054 IQ set).
// Usage: /setvcard <field> <value>
// Fields: fn, nickname, email, url, desc, org, title, tel, bday, note
int command__setvcard(const void *pointer, void *data,
                      struct t_gui_buffer *buffer, int argc,
                      char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_account->connected())
    {
        weechat_printf(buffer, "%sxmpp: you are not connected to server",
                       weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    if (argc < 3)
    {
        weechat_printf(buffer,
                       "%s%s: usage: /setvcard <field> <value>\n"
                       "  fields: fn, nickname, email, url, desc, org, title, tel, bday, note",
                       weechat_prefix("error"), argv[0]);
        return WEECHAT_RC_OK;
    }

    std::string field(argv[1]);
    std::string value(argv_eol[2]);

    // Validate the field name first
    static const std::vector<std::string> valid_fields = {
        "fn", "nickname", "email", "url", "desc", "org", "title", "tel", "bday", "note"
    };
    if (std::find(valid_fields.begin(), valid_fields.end(), field) == valid_fields.end())
    {
        weechat_printf(buffer,
                       "%s%s: unknown vCard field '%s'\n"
                       "  valid fields: fn, nickname, email, url, desc, org, title, tel, bday, note",
                       weechat_prefix("error"), argv[0], argv[1]);
        return WEECHAT_RC_OK;
    }

    // Fetch our own vCard first so we can merge the single field change without
    // clobbering the rest of the vCard (XEP-0054 IQ set replaces the entire vCard).
    xmpp_stanza_t *iq = xmpp::xep0054::vcard_request(ptr_account->context, nullptr);
    const char *req_id = xmpp_stanza_get_id(iq);
    if (req_id)
        ptr_account->setvcard_queries[req_id] = { buffer, field, value };
    weechat_printf(buffer, "%sFetching current vCard before updating %s...",
                   weechat_prefix("network"), field.c_str());
    ptr_account->connection.send(iq);
    xmpp_stanza_release(iq);

    return WEECHAT_RC_OK;
}

// /setavatar <filepath>
// Publish a local image file as own avatar via XEP-0084 (User Avatar).
// Supported formats: PNG, JPEG, GIF, WEBP (detected from extension).
int command__setavatar(const void *pointer, void *data,
                       struct t_gui_buffer *buffer, int argc,
                       char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;
    (void) argv;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_account->connected())
    {
        weechat_printf(buffer, "%sxmpp: you are not connected to server",
                       weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    if (argc < 2)
    {
        weechat_printf(buffer,
                       "%s%s: usage: /setavatar <filepath>\n"
                       "  filepath: path to a PNG, JPEG, GIF, or WEBP image file",
                       weechat_prefix("error"), argv[0]);
        return WEECHAT_RC_OK;
    }

    std::string filepath(argv_eol[1]);
    weechat::avatar::publish(*ptr_account, filepath);

    return WEECHAT_RC_OK;
}

int command__block(const void *pointer, void *data,
                  struct t_gui_buffer *buffer, int argc,
                  char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;
    xmpp_stanza_t *iq;

    (void) pointer;
    (void) data;
    (void) argv_eol;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_account->connected())
    {
        weechat_printf(buffer, "%sxmpp: you are not connected to server",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    if (argc < 2)
    {
        weechat_printf(buffer, "%s%s: missing JID argument",
                      weechat_prefix("error"),
                      argv[0]);
        return WEECHAT_RC_OK;
    }

    // Block the specified JID(s)
    const char **jids = (const char **)&argv[1];
    int count = argc - 1;
    
    xmpp_string_guard iq_id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
    iq = xmpp_iq_new(ptr_account->context, "set", iq_id_g.ptr);
    
    xmpp_stanza_t *block = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(block, "block");
    xmpp_stanza_set_ns(block, "urn:xmpp:blocking");
    
    for (int i = 0; i < count; i++)
    {
        xmpp_stanza_t *item = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(item, "item");
        xmpp_stanza_set_attribute(item, "jid", jids[i]);
        xmpp_stanza_add_child(block, item);
        xmpp_stanza_release(item);
        
        weechat_printf(buffer, "%sBlocking %s...",
                       weechat_prefix("network"), jids[i]);
    }
    
    xmpp_stanza_add_child(iq, block);
    xmpp_stanza_release(block);
    
    ptr_account->connection.send(iq);
    xmpp_stanza_release(iq);

    return WEECHAT_RC_OK;
}

int command__unblock(const void *pointer, void *data,
                    struct t_gui_buffer *buffer, int argc,
                    char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;
    xmpp_stanza_t *iq;

    (void) pointer;
    (void) data;
    (void) argv_eol;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_account->connected())
    {
        weechat_printf(buffer, "%sxmpp: you are not connected to server",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    {
    xmpp_string_guard iq_id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
    iq = xmpp_iq_new(ptr_account->context, "set", iq_id_g.ptr);
    }
    
    xmpp_stanza_t *unblock = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(unblock, "unblock");
    xmpp_stanza_set_ns(unblock, "urn:xmpp:blocking");
    
    if (argc > 1)
    {
        // Unblock specific JID(s)
        const char **jids = (const char **)&argv[1];
        int count = argc - 1;
        
        for (int i = 0; i < count; i++)
        {
            xmpp_stanza_t *item = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(item, "item");
            xmpp_stanza_set_attribute(item, "jid", jids[i]);
            xmpp_stanza_add_child(unblock, item);
            xmpp_stanza_release(item);
            
            weechat_printf(buffer, "%sUnblocking %s...",
                           weechat_prefix("network"), jids[i]);
        }
    }
    else
    {
        // Empty unblock = unblock all
        weechat_printf(buffer, "%sUnblocking all JIDs...",
                       weechat_prefix("network"));
    }
    
    xmpp_stanza_add_child(iq, unblock);
    xmpp_stanza_release(unblock);
    
    ptr_account->connection.send(iq);
    xmpp_stanza_release(iq);

    return WEECHAT_RC_OK;
}

int command__blocklist(const void *pointer, void *data,
                      struct t_gui_buffer *buffer, int argc,
                      char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;
    xmpp_stanza_t *iq;

    (void) pointer;
    (void) data;
    (void) argv;
    (void) argv_eol;
    (void) argc;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_account->connected())
    {
        weechat_printf(buffer, "%sxmpp: you are not connected to server",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    // Request block list
    xmpp_string_guard iq_id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
    iq = xmpp_iq_new(ptr_account->context, "get", iq_id_g.ptr);
    
    xmpp_stanza_t *blocklist = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(blocklist, "blocklist");
    xmpp_stanza_set_ns(blocklist, "urn:xmpp:blocking");
    
    xmpp_stanza_add_child(iq, blocklist);
    xmpp_stanza_release(blocklist);
    
    weechat_printf(buffer, "%sRequesting block list...",
                   weechat_prefix("network"));
    
    ptr_account->connection.send(iq);
    xmpp_stanza_release(iq);

    return WEECHAT_RC_OK;
}

int command__disco(const void *pointer, void *data,
                   struct t_gui_buffer *buffer, int argc,
                   char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;
    (void) argv_eol;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_account->connected())
    {
        weechat_printf(buffer,
                       _("%s%s: you are not connected to server"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    const char *target = NULL;
    if (argc > 1)
        target = argv[1];
    else
        target = xmpp_jid_domain(ptr_account->context, ptr_account->jid().data());

    xmpp_string_guard id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
    const char *id = id_g.ptr;
    ptr_account->user_disco_queries.insert(id);
    
    xmpp_stanza_t *iq = xmpp_iq_new(ptr_account->context, "get", id);
    xmpp_stanza_set_to(iq, target);
    
    xmpp_stanza_t *query = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(query, "query");
    xmpp_stanza_set_ns(query, "http://jabber.org/protocol/disco#info");
    
    xmpp_stanza_add_child(iq, query);
    xmpp_stanza_release(query);
    
    ptr_account->connection.send( iq);
    xmpp_stanza_release(iq);
    // freed by id_g

    weechat_printf(buffer, "Querying service discovery for %s...", target);

    return WEECHAT_RC_OK;
}

int command__roster(const void *pointer, void *data,
                   struct t_gui_buffer *buffer, int argc,
                   char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;
    (void) argv_eol;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_account->connected())
    {
        weechat_printf(buffer,
                       _("%s%s: you are not connected to server"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    // /roster - display roster
    if (argc == 1)
    {
        if (ptr_account->roster.empty())
        {
            weechat_printf(buffer, "%sRoster is empty", weechat_prefix("network"));
            return WEECHAT_RC_OK;
        }

        weechat_printf(buffer, "");
        weechat_printf(buffer, "%sRoster (%zu contacts):", 
                      weechat_prefix("network"), 
                      ptr_account->roster.size());
        
        for (const auto& [jid, item] : ptr_account->roster)
        {
            std::string display = jid;
            if (!item.name.empty())
                display = item.name + " <" + jid + ">";
            
            std::string groups_str = "";
            if (!item.groups.empty())
            {
                groups_str = " [";
                for (size_t i = 0; i < item.groups.size(); i++)
                {
                    if (i > 0) groups_str += ", ";
                    groups_str += item.groups[i];
                }
                groups_str += "]";
            }
            
            weechat_printf(buffer, "  %s%s%s - %s%s",
                          weechat_color("chat_nick"),
                          display.c_str(),
                          weechat_color("reset"),
                          item.subscription.c_str(),
                          groups_str.c_str());
        }
        
        return WEECHAT_RC_OK;
    }

    // /roster add <jid> [name]
    if (argc >= 3 && weechat_strcasecmp(argv[1], "add") == 0)
    {
        const char *jid = argv[2];
        const char *name = (argc >= 4) ? argv_eol[3] : NULL;

        xmpp_string_guard id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
        const char *id = id_g.ptr;
        xmpp_stanza_t *iq = xmpp_iq_new(ptr_account->context, "set", id);
        // freed by id_g
        
        xmpp_stanza_t *query = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(query, "query");
        xmpp_stanza_set_ns(query, "jabber:iq:roster");
        
        xmpp_stanza_t *item = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(item, "item");
        xmpp_stanza_set_attribute(item, "jid", jid);
        if (name)
            xmpp_stanza_set_attribute(item, "name", name);
        
        xmpp_stanza_add_child(query, item);
        xmpp_stanza_add_child(iq, query);
        
        ptr_account->connection.send( iq);
        
        xmpp_stanza_release(item);
        xmpp_stanza_release(query);
        xmpp_stanza_release(iq);

        weechat_printf(buffer, "%sAdding %s to roster...", 
                      weechat_prefix("network"), jid);

        // Also send presence subscription request
        xmpp_stanza_t *presence = xmpp_presence_new(ptr_account->context);
        xmpp_stanza_set_type(presence, "subscribe");
        xmpp_stanza_set_to(presence, jid);
        ptr_account->connection.send( presence);
        xmpp_stanza_release(presence);

        return WEECHAT_RC_OK;
    }

    // /roster del <jid>
    if (argc >= 3 && (weechat_strcasecmp(argv[1], "del") == 0 || 
                       weechat_strcasecmp(argv[1], "delete") == 0 ||
                       weechat_strcasecmp(argv[1], "remove") == 0))
    {
        const char *jid = argv[2];

        xmpp_string_guard id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
        const char *id = id_g.ptr;
        xmpp_stanza_t *iq = xmpp_iq_new(ptr_account->context, "set", id);
        // freed by id_g
        
        xmpp_stanza_t *query = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(query, "query");
        xmpp_stanza_set_ns(query, "jabber:iq:roster");
        
        xmpp_stanza_t *item = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(item, "item");
        xmpp_stanza_set_attribute(item, "jid", jid);
        xmpp_stanza_set_attribute(item, "subscription", "remove");
        
        xmpp_stanza_add_child(query, item);
        xmpp_stanza_add_child(iq, query);
        
        ptr_account->connection.send( iq);
        
        xmpp_stanza_release(item);
        xmpp_stanza_release(query);
        xmpp_stanza_release(iq);

        weechat_printf(buffer, "%sRemoving %s from roster...", 
                      weechat_prefix("network"), jid);

        return WEECHAT_RC_OK;
    }

    weechat_printf(buffer,
                   _("%s%s: unknown roster command (try /help roster)"),
                   weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);

    return WEECHAT_RC_OK;
}

int command__bookmark(const void *pointer, void *data,
                     struct t_gui_buffer *buffer, int argc,
                     char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;
    (void) argv_eol;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_account->connected())
    {
        weechat_printf(buffer,
                       _("%s%s: you are not connected to server"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    // /bookmark - list bookmarks
    if (argc == 1)
    {
        if (ptr_account->bookmarks.empty())
        {
            weechat_printf(buffer, "%sNo bookmarks", weechat_prefix("network"));
            return WEECHAT_RC_OK;
        }

        weechat_printf(buffer, "");
        weechat_printf(buffer, "%sBookmarks (%zu):", 
                      weechat_prefix("network"), 
                      ptr_account->bookmarks.size());
        
        for (const auto& [jid, bookmark] : ptr_account->bookmarks)
        {
            std::string display = jid;
            if (!bookmark.name.empty())
                display = bookmark.name + " <" + jid + ">";
            
            std::string nick_str = "";
            if (!bookmark.nick.empty())
                nick_str = " (nick: " + bookmark.nick + ")";
            
            std::string autojoin_str = bookmark.autojoin ? " [autojoin]" : "";
            
            weechat_printf(buffer, "  %s%s%s%s%s",
                          weechat_color("chat_nick"),
                          display.c_str(),
                          weechat_color("reset"),
                          nick_str.c_str(),
                          autojoin_str.c_str());
        }
        
        return WEECHAT_RC_OK;
    }

    // /bookmark add [jid] [name]
    if (argc >= 2 && weechat_strcasecmp(argv[1], "add") == 0)
    {
        const char *jid = NULL;
        const char *name = NULL;
        
        if (argc >= 3)
        {
            jid = argv[2];
            name = (argc >= 4) ? argv_eol[3] : NULL;
        }
        else if (ptr_channel && ptr_channel->type == weechat::channel::chat_type::MUC)
        {
            jid = ptr_channel->id.data();
            name = ptr_channel->name.data();
        }
        else
        {
            weechat_printf(buffer, "%sYou must specify a JID or be in a MUC buffer",
                          weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }

        // Add or update bookmark
        ptr_account->bookmarks[jid].jid = jid;
        ptr_account->bookmarks[jid].name = name ? name : "";
        ptr_account->bookmarks[jid].nick = ptr_account->nickname().data();
        ptr_account->bookmarks[jid].autojoin = false;
        
        ptr_account->send_bookmarks();
        
        weechat_printf(buffer, "%sBookmark added: %s", 
                      weechat_prefix("network"), jid);

        return WEECHAT_RC_OK;
    }

    // /bookmark del <jid>
    if (argc >= 3 && (weechat_strcasecmp(argv[1], "del") == 0 || 
                       weechat_strcasecmp(argv[1], "delete") == 0 ||
                       weechat_strcasecmp(argv[1], "remove") == 0))
    {
        const char *jid = argv[2];
        
        if (ptr_account->bookmarks.find(jid) == ptr_account->bookmarks.end())
        {
            weechat_printf(buffer, "%sBookmark not found: %s",
                          weechat_prefix("error"), jid);
            return WEECHAT_RC_OK;
        }
        
        ptr_account->bookmarks.erase(jid);
        ptr_account->send_bookmarks();
        
        weechat_printf(buffer, "%sBookmark removed: %s", 
                      weechat_prefix("network"), jid);

        return WEECHAT_RC_OK;
    }

    // /bookmark autojoin <jid> <on|off>
    if (argc >= 4 && weechat_strcasecmp(argv[1], "autojoin") == 0)
    {
        const char *jid = argv[2];
        const char *value = argv[3];
        
        if (ptr_account->bookmarks.find(jid) == ptr_account->bookmarks.end())
        {
            weechat_printf(buffer, "%sBookmark not found: %s",
                          weechat_prefix("error"), jid);
            return WEECHAT_RC_OK;
        }
        
        bool autojoin = weechat_strcasecmp(value, "on") == 0 || 
                       weechat_strcasecmp(value, "true") == 0 ||
                       weechat_strcasecmp(value, "1") == 0;
        
        ptr_account->bookmarks[jid].autojoin = autojoin;
        ptr_account->send_bookmarks();
        
        weechat_printf(buffer, "%sAutojoin %s for %s", 
                      weechat_prefix("network"),
                      autojoin ? "enabled" : "disabled",
                      jid);

        return WEECHAT_RC_OK;
    }

    weechat_printf(buffer,
                   _("%s%s: unknown bookmark command (try /help bookmark)"),
                   weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);

    return WEECHAT_RC_OK;
}

// XEP-0433: Extended Channel Search
// Send a search IQ to the given service JID with optional keywords.
// Two steps: first request the form (type=get <search/>), then submit it.
// Here we go straight to submitting a minimal form (the form request step
// is optional per the spec and search.jabber.network accepts direct submits).
static void xep0433_send_search(weechat::account *account,
                                struct t_gui_buffer *buffer,
                                const char *service_jid,
                                const char *keywords)
{
    xmpp_string_guard search_id_g(account->context, xmpp_uuid_gen(account->context));
    const char *search_id = search_id_g.ptr;

    weechat::account::channel_search_query_info info;
    info.service_jid = service_jid;
    info.keywords    = keywords ? keywords : "";
    info.buffer      = buffer;
    info.form_requested = false;
    account->channel_search_queries[search_id] = info;

    // Build: <iq type='get' to='service' id='...'><search xmlns='urn:xmpp:channel-search:0:search'>
    //          <x xmlns='jabber:x:data' type='submit'>
    //            <field type='hidden' var='FORM_TYPE'><value>urn:xmpp:channel-search:0:search-params</value></field>
    //            [<field var='q'><value>keywords</value></field>]
    //            <field var='key' type='list-single'><value>{urn:xmpp:channel-search:0:order}nusers</value></field>
    //          </x>
    //        </search></iq>
    xmpp_stanza_t *iq = xmpp_iq_new(account->context, "get", search_id);
    xmpp_stanza_set_to(iq, service_jid);

    xmpp_stanza_t *search_el = xmpp_stanza_new(account->context);
    xmpp_stanza_set_name(search_el, "search");
    xmpp_stanza_set_ns(search_el, "urn:xmpp:channel-search:0:search");

    xmpp_stanza_t *x_form = xmpp_stanza_new(account->context);
    xmpp_stanza_set_name(x_form, "x");
    xmpp_stanza_set_ns(x_form, "jabber:x:data");
    xmpp_stanza_set_attribute(x_form, "type", "submit");

    // FORM_TYPE field
    {
        xmpp_stanza_t *field = xmpp_stanza_new(account->context);
        xmpp_stanza_set_name(field, "field");
        xmpp_stanza_set_attribute(field, "type", "hidden");
        xmpp_stanza_set_attribute(field, "var", "FORM_TYPE");
        xmpp_stanza_t *value = xmpp_stanza_new(account->context);
        xmpp_stanza_set_name(value, "value");
        xmpp_stanza_t *text = xmpp_stanza_new(account->context);
        xmpp_stanza_set_text(text, "urn:xmpp:channel-search:0:search-params");
        xmpp_stanza_add_child(value, text);
        xmpp_stanza_release(text);
        xmpp_stanza_add_child(field, value);
        xmpp_stanza_release(value);
        xmpp_stanza_add_child(x_form, field);
        xmpp_stanza_release(field);
    }

    // Optional keyword search field
    if (keywords && keywords[0])
    {
        xmpp_stanza_t *field = xmpp_stanza_new(account->context);
        xmpp_stanza_set_name(field, "field");
        xmpp_stanza_set_attribute(field, "var", "q");
        xmpp_stanza_t *value = xmpp_stanza_new(account->context);
        xmpp_stanza_set_name(value, "value");
        xmpp_stanza_t *text = xmpp_stanza_new(account->context);
        xmpp_stanza_set_text(text, keywords);
        xmpp_stanza_add_child(value, text);
        xmpp_stanza_release(text);
        xmpp_stanza_add_child(field, value);
        xmpp_stanza_release(value);
        xmpp_stanza_add_child(x_form, field);
        xmpp_stanza_release(field);
    }

    // Sort key field — order by number of users descending
    {
        xmpp_stanza_t *field = xmpp_stanza_new(account->context);
        xmpp_stanza_set_name(field, "field");
        xmpp_stanza_set_attribute(field, "var", "key");
        xmpp_stanza_set_attribute(field, "type", "list-single");
        xmpp_stanza_t *value = xmpp_stanza_new(account->context);
        xmpp_stanza_set_name(value, "value");
        xmpp_stanza_t *text = xmpp_stanza_new(account->context);
        xmpp_stanza_set_text(text, "{urn:xmpp:channel-search:0:order}nusers");
        xmpp_stanza_add_child(value, text);
        xmpp_stanza_release(text);
        xmpp_stanza_add_child(field, value);
        xmpp_stanza_release(value);
        xmpp_stanza_add_child(x_form, field);
        xmpp_stanza_release(field);
    }

    xmpp_stanza_add_child(search_el, x_form);
    xmpp_stanza_release(x_form);
    xmpp_stanza_add_child(iq, search_el);
    xmpp_stanza_release(search_el);

    account->connection.send(iq);
    xmpp_stanza_release(iq);
    // freed by search_id_g
}

int command__list(const void *pointer, void *data,
                  struct t_gui_buffer *buffer, int argc,
                  char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;
    (void) argv_eol;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_account->connected())
    {
        weechat_printf(buffer,
                       _("%s%s: you are not connected to server"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    // Determine service JID and keywords.
    // If argv[1] contains a dot (looks like a domain/JID), treat it as the service.
    // Otherwise treat all args as keywords and use the default public directory.
    const char *service_jid = "search.jabber.network";
    const char *keywords = "";

    if (argc >= 2)
    {
        // Heuristic: if the first arg contains a dot but no spaces, it's a JID/domain
        bool first_arg_is_jid = (strchr(argv[1], '.') != NULL)
                                 && (strchr(argv[1], ' ') == NULL);
        if (first_arg_is_jid)
        {
            service_jid = argv[1];
            keywords = (argc >= 3) ? argv_eol[2] : "";
        }
        else
        {
            keywords = argv_eol[1];
        }
    }

    weechat_printf(buffer, "");
    if (keywords[0])
        weechat_printf(buffer, "%sSearching for MUC rooms matching \"%s\" via %s (XEP-0433)…",
                      weechat_prefix("network"), keywords, service_jid);
    else
        weechat_printf(buffer, "%sSearching for popular MUC rooms via %s (XEP-0433)…",
                      weechat_prefix("network"), service_jid);

    xep0433_send_search(ptr_account, buffer, service_jid, keywords[0] ? keywords : NULL);

    return WEECHAT_RC_OK;
}

// Smart file picker: tries GUI dialogs, then fzf, then gives usage
std::optional<std::string> pick_file_interactive()
{
    // Check for GUI environment
    const char *wayland_display = getenv("WAYLAND_DISPLAY");
    const char *x11_display = getenv("DISPLAY");
    bool has_gui = (wayland_display && wayland_display[0]) || (x11_display && x11_display[0]);
    
    if (has_gui)
    {
        // Try zenity (works on both X11 and Wayland)
        FILE *fp = popen("zenity --file-selection --title='Select file to upload' 2>/dev/null", "r");
        if (fp)
        {
            char path[4096] = {0};
            if (fgets(path, sizeof(path), fp))
            {
                pclose(fp);
                // Remove trailing newline
                size_t len = strlen(path);
                if (len > 0 && path[len-1] == '\n')
                    path[len-1] = '\0';
                if (path[0])
                    return std::string(path);
            }
            pclose(fp);
        }
        
        // Try kdialog (KDE)
        fp = popen("kdialog --getopenfilename ~ 2>/dev/null", "r");
        if (fp)
        {
            char path[4096] = {0};
            if (fgets(path, sizeof(path), fp))
            {
                pclose(fp);
                size_t len = strlen(path);
                if (len > 0 && path[len-1] == '\n')
                    path[len-1] = '\0';
                if (path[0])
                    return std::string(path);
            }
            pclose(fp);
        }
    }
    
    // Try fzf in terminal (works everywhere)
    FILE *fp = popen("fzf --prompt='Select file to upload: ' --preview='file {}' 2>/dev/null", "r");
    if (fp)
    {
        char path[4096] = {0};
        if (fgets(path, sizeof(path), fp))
        {
            pclose(fp);
            size_t len = strlen(path);
            if (len > 0 && path[len-1] == '\n')
                path[len-1] = '\0';
            if (path[0])
                return std::string(path);
        }
        pclose(fp);
    }
    
    return std::nullopt;
}

int command__upload(const void *pointer, void *data,
                    t_gui_buffer *buffer, int argc,
                    char **argv, char **argv_eol)
{
    (void) pointer;
    (void) data;
    (void) argv_eol;

    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;
    
    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_channel)
    {
        weechat_printf(buffer, "%s%s: command \"upload\" must be executed in an xmpp buffer",
                      weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        weechat_printf(buffer, "%s%s: you are not connected to server",
                      weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    std::string filename;
    
    if (argc < 2)
    {
        // Try interactive file picker
        auto picked = pick_file_interactive();
        if (picked)
        {
            filename = *picked;
            weechat_printf(buffer, "%sSelected file: %s",
                          weechat_prefix("network"), filename.c_str());
        }
        else
        {
            weechat_printf(buffer, "%s%s: no file selected. Usage: /upload <filename>",
                          weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
            weechat_printf(buffer, "%sNote: Install zenity, kdialog, or fzf for interactive file picker",
                          weechat_prefix("error"));
            return WEECHAT_RC_OK;
        }
    }
    else
    {
        filename = argv[1];
    }
    
    // Check if file exists and is readable
    FILE *file = fopen(filename.c_str(), "rb");
    if (!file)
    {
        weechat_printf(buffer, "%s%s: cannot open file: %s",
                      weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                      filename.c_str());
        return WEECHAT_RC_OK;
    }
    
    // Get file size
    fseek(file, 0, SEEK_END);
    long filesize = ftell(file);
    fclose(file);
    
    if (filesize <= 0)
    {
        weechat_printf(buffer, "%s%s: file is empty: %s",
                      weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                      filename.c_str());
        return WEECHAT_RC_OK;
    }
    
    // Check against server max size if known
    if (ptr_account->upload_max_size > 0 && (size_t)filesize > ptr_account->upload_max_size)
    {
        weechat_printf(buffer, "%s%s: file too large (max: %zu bytes, file: %ld bytes)",
                      weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                      ptr_account->upload_max_size, filesize);
        return WEECHAT_RC_OK;
    }
    
    // Check if we have discovered upload service
    if (ptr_account->upload_service.empty())
    {
        weechat_printf(buffer, "%s%s: upload service not discovered yet (try reconnecting)",
                      weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }
    
    weechat_printf(buffer, "%sRequesting upload slot for %s (%ld bytes)...",
                  weechat_prefix("network"), filename.c_str(), filesize);
    
    // Generate request ID
    xmpp_string_guard id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
    const char *id = id_g.ptr;
    
    // Extract just the filename (no path)
    size_t last_slash = filename.find_last_of("/\\");
    std::string basename = (last_slash != std::string::npos) 
        ? filename.substr(last_slash + 1) 
        : filename;
    
    // Sanitize filename: allow alphanumeric, dots, dashes, and underscores
    // Some servers are very strict about filenames (XEP-0363 doesn't specify allowed chars)
    std::string sanitized_basename;
    for (char c : basename)
    {
        if ((c >= 'a' && c <= 'z') || 
            (c >= 'A' && c <= 'Z') || 
            (c >= '0' && c <= '9') || 
            c == '.' || c == '-' || c == '_')
        {
            sanitized_basename += c;
        }
        else
        {
            sanitized_basename += '-';  // Replace any other char with dash
        }
    }
    
    // Remove consecutive dashes
    size_t pos = 0;
    while ((pos = sanitized_basename.find("--", pos)) != std::string::npos)
    {
        sanitized_basename.erase(pos, 1);
    }
    
    // Remove leading/trailing dashes
    while (!sanitized_basename.empty() && sanitized_basename[0] == '-')
        sanitized_basename.erase(0, 1);
    while (!sanitized_basename.empty() && sanitized_basename.back() == '-')
        sanitized_basename.pop_back();
    
    weechat_printf(buffer, "%sUsing sanitized filename: %s",
                  weechat_prefix("network"), sanitized_basename.c_str());
    
    // Determine content-type from file extension
    std::string content_type = "application/octet-stream";
    size_t dot_pos = sanitized_basename.find_last_of('.');
    if (dot_pos != std::string::npos)
    {
        std::string ext = sanitized_basename.substr(dot_pos + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        
        if (ext == "jpg" || ext == "jpeg") content_type = "image/jpeg";
        else if (ext == "png") content_type = "image/png";
        else if (ext == "gif") content_type = "image/gif";
        else if (ext == "webp") content_type = "image/webp";
        else if (ext == "mp4") content_type = "video/mp4";
        else if (ext == "webm") content_type = "video/webm";
        else if (ext == "pdf") content_type = "application/pdf";
        else if (ext == "txt") content_type = "text/plain";
        else if (ext == "zip") content_type = "application/zip";
        else if (ext == "tar") content_type = "application/x-tar";
    }
    
    // Get file size
    FILE* f = fopen(filename.c_str(), "rb");
    if (!f)
    {
        weechat_printf(buffer, "%s%s: failed to open file: %s",
                      weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, filename.c_str());
        return WEECHAT_RC_ERROR;
    }
    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fclose(f);
    
    // Store upload request with metadata for SIMS
    ptr_account->upload_requests[id] = {
        id, 
        filename, 
        sanitized_basename, 
        ptr_channel->id,
        content_type,
        file_size,
        ""  // sha256_hash will be calculated during upload
    };
    
    // Build upload slot request (XEP-0363 v0.3.0+)
    xmpp_stanza_t *iq = xmpp_iq_new(ptr_account->context, "get", id);
    xmpp_stanza_set_to(iq, ptr_account->upload_service.c_str());
    
    xmpp_stanza_t *request = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(request, "request");
    xmpp_stanza_set_ns(request, "urn:xmpp:http:upload:0");
    
    // In XEP-0363 v0.3.0+, filename and size are attributes, not child elements
    xmpp_stanza_set_attribute(request, "filename", sanitized_basename.c_str());
    
    char size_str[32];
    snprintf(size_str, sizeof(size_str), "%zu", file_size);
    xmpp_stanza_set_attribute(request, "size", size_str);
    
    // Add content-type attribute if applicable
    if (!content_type.empty())
    {
        xmpp_stanza_set_attribute(request, "content-type", content_type.c_str());
    }
    
    xmpp_stanza_add_child(iq, request);
    xmpp_stanza_release(request);
    
    ptr_account->connection.send(iq);
    xmpp_stanza_release(iq);
    // freed by id_g

    return WEECHAT_RC_OK;
}

// XEP-0224: Attention — send <attention> to get a contact's attention
int command__buzz(const void *pointer, void *data,
                  struct t_gui_buffer *buffer, int argc,
                  char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;
    (void) argc;
    (void) argv;
    (void) argv_eol;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_channel)
    {
        weechat_printf(buffer, "%sxmpp: you must be in a channel to buzz someone",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        weechat_printf(buffer, "%sxmpp: you are not connected to server",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    if (ptr_channel->type == weechat::channel::chat_type::MUC)
    {
        weechat_printf(buffer, "%sxmpp: /buzz can only be used in PM channels",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    // Send <message> with <attention> element (XEP-0224)
    xmpp_stanza_t *message = xmpp_message_new(ptr_account->context, "chat",
                                               ptr_channel->id.data(), NULL);

    xmpp_string_guard id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
    const char *id = id_g.ptr;
    xmpp_stanza_set_id(message, id);
    // freed by id_g

    xmpp_stanza_t *attention = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(attention, "attention");
    xmpp_stanza_set_ns(attention, "urn:xmpp:attention:0");
    xmpp_stanza_add_child(message, attention);
    xmpp_stanza_release(attention);

    ptr_account->connection.send(message);
    xmpp_stanza_release(message);

    weechat_printf(buffer, "%sxmpp: buzz sent to %s",
                  weechat_prefix("network"), ptr_channel->id.data());

    return WEECHAT_RC_OK;
}

// XEP-0382: Spoiler Messages — send a message with a spoiler warning
int command__spoiler(const void *pointer, void *data,
                     struct t_gui_buffer *buffer, int argc,
                     char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;
    (void) argv;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_channel)
    {
        weechat_printf(buffer, "%sxmpp: you must be in a channel to send spoiler messages",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        weechat_printf(buffer, "%sxmpp: you are not connected to server",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    // Usage: /spoiler [hint:] <message>
    // If first arg ends with ':', treat it as a hint
    if (argc < 2)
    {
        weechat_printf(buffer, "%sxmpp: missing message text",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    const char *text = argv_eol[1];

    // Check if "hint: message" form is used
    if (argc >= 3 && argv[1][strlen(argv[1])-1] == ':')
    {
        const char *hint = argv[1];
        // Strip trailing colon
        std::string hint_str(hint, strlen(hint)-1);
        text = argv_eol[2];

        xmpp_stanza_t *message = xmpp_message_new(ptr_account->context,
                        ptr_channel->type == weechat::channel::chat_type::MUC
                        ? "groupchat" : "chat",
                        ptr_channel->id.data(), NULL);

        xmpp_string_guard id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
        const char *id = id_g.ptr;
        xmpp_stanza_set_id(message, id);
        // freed by id_g

        xmpp_message_set_body(message, text);

        xmpp_stanza_t *spoiler = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(spoiler, "spoiler");
        xmpp_stanza_set_ns(spoiler, "urn:xmpp:spoiler:0");
        // Set hint text as text node child
        xmpp_stanza_t *hint_node = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_text(hint_node, hint_str.c_str());
        xmpp_stanza_add_child(spoiler, hint_node);
        xmpp_stanza_release(hint_node);
        xmpp_stanza_add_child(message, spoiler);
        xmpp_stanza_release(spoiler);

        // Add store hint
        xmpp_stanza_t *store = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(store, "store");
        xmpp_stanza_set_ns(store, "urn:xmpp:hints");
        xmpp_stanza_add_child(message, store);
        xmpp_stanza_release(store);

        ptr_account->connection.send(message);
        xmpp_stanza_release(message);
    }
    else
    {
        xmpp_stanza_t *message = xmpp_message_new(ptr_account->context,
                        ptr_channel->type == weechat::channel::chat_type::MUC
                        ? "groupchat" : "chat",
                        ptr_channel->id.data(), NULL);

        xmpp_string_guard id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
        const char *id = id_g.ptr;
        xmpp_stanza_set_id(message, id);
        // freed by id_g

        xmpp_message_set_body(message, text);

        // <spoiler/> with no hint
        xmpp_stanza_t *spoiler = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(spoiler, "spoiler");
        xmpp_stanza_set_ns(spoiler, "urn:xmpp:spoiler:0");
        xmpp_stanza_add_child(message, spoiler);
        xmpp_stanza_release(spoiler);

        // Add store hint
        xmpp_stanza_t *store = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(store, "store");
        xmpp_stanza_set_ns(store, "urn:xmpp:hints");
        xmpp_stanza_add_child(message, store);
        xmpp_stanza_release(store);

        ptr_account->connection.send(message);
        xmpp_stanza_release(message);
    }

    weechat_printf(buffer, "%sxmpp: spoiler message sent",
                  weechat_prefix("network"));

    return WEECHAT_RC_OK;
}

// XEP-0050: Ad-Hoc Commands — list and execute server/component commands
// Usage:
//   /adhoc <jid>                         — list available commands
//   /adhoc <jid> <node>                  — execute a command
//   /adhoc <jid> <node> <sessionid> [field=value ...]  — submit a form step
int command__adhoc(const void *pointer, void *data,
                   struct t_gui_buffer *buffer, int argc,
                   char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;
    (void) argv_eol;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_account->connected())
    {
        weechat_printf(buffer, "%sxmpp: you are not connected to server",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    if (argc < 2)
    {
        weechat_printf(buffer, "%sxmpp: /adhoc <jid> [<node> [<sessionid> [field=value ...]]]",
                      weechat_prefix("error"));
        return WEECHAT_RC_OK;
    }

    const char *target_jid = argv[1];

    if (argc == 2)
    {
        // List available commands via disco#items with commands node
        xmpp_string_guard query_id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
        const char *query_id = query_id_g.ptr;

        weechat::account::adhoc_query_info info;
        info.target_jid = target_jid;
        info.buffer = buffer;
        info.is_list = true;
        ptr_account->adhoc_queries[query_id] = info;

        xmpp_stanza_t *iq = xmpp_iq_new(ptr_account->context, "get", query_id);
        xmpp_stanza_set_to(iq, target_jid);
        xmpp_stanza_t *query_stanza = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(query_stanza, "query");
        xmpp_stanza_set_ns(query_stanza, "http://jabber.org/protocol/disco#items");
        xmpp_stanza_set_attribute(query_stanza, "node",
                                  "http://jabber.org/protocol/commands");
        xmpp_stanza_add_child(iq, query_stanza);
        xmpp_stanza_release(query_stanza);
        ptr_account->connection.send(iq);
        xmpp_stanza_release(iq);
        // freed by query_id_g

        weechat_printf(buffer, "%sxmpp: querying commands on %s…",
                      weechat_prefix("network"), target_jid);
        return WEECHAT_RC_OK;
    }

    const char *node = argv[2];

    if (argc == 3)
    {
        // Execute a command (first step)
        xmpp_string_guard exec_id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
        const char *exec_id = exec_id_g.ptr;

        weechat::account::adhoc_query_info info;
        info.target_jid = target_jid;
        info.buffer = buffer;
        info.is_list = false;
        info.node = node;
        ptr_account->adhoc_queries[exec_id] = info;

        xmpp_stanza_t *iq = xmpp_iq_new(ptr_account->context, "set", exec_id);
        xmpp_stanza_set_to(iq, target_jid);
        xmpp_stanza_t *command = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(command, "command");
        xmpp_stanza_set_ns(command, "http://jabber.org/protocol/commands");
        xmpp_stanza_set_attribute(command, "node", node);
        xmpp_stanza_set_attribute(command, "action", "execute");
        xmpp_stanza_add_child(iq, command);
        xmpp_stanza_release(command);
        ptr_account->connection.send(iq);
        xmpp_stanza_release(iq);
        // freed by exec_id_g

        weechat_printf(buffer, "%sxmpp: executing command %s on %s…",
                      weechat_prefix("network"), node, target_jid);
        return WEECHAT_RC_OK;
    }

    // argc >= 4: submit a form step
    // argv[3] = sessionid, argv[4..] = field=value pairs
    const char *session_id = argv[3];
    xmpp_string_guard submit_id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
    const char *submit_id = submit_id_g.ptr;

    weechat::account::adhoc_query_info info;
    info.target_jid = target_jid;
    info.buffer = buffer;
    info.is_list = false;
    info.node = node;
    info.session_id = session_id;
    ptr_account->adhoc_queries[submit_id] = info;

    xmpp_stanza_t *iq = xmpp_iq_new(ptr_account->context, "set", submit_id);
    xmpp_stanza_set_to(iq, target_jid);
    xmpp_stanza_t *command = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(command, "command");
    xmpp_stanza_set_ns(command, "http://jabber.org/protocol/commands");
    xmpp_stanza_set_attribute(command, "node", node);
    xmpp_stanza_set_attribute(command, "sessionid", session_id);
    xmpp_stanza_set_attribute(command, "action", "execute");

    // Build x data form with provided field=value pairs
    xmpp_stanza_t *x_form = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(x_form, "x");
    xmpp_stanza_set_ns(x_form, "jabber:x:data");
    xmpp_stanza_set_attribute(x_form, "type", "submit");

    for (int i = 4; i < argc; i++)
    {
        // Parse "field=value" pairs
        const char *eq = strchr(argv[i], '=');
        if (!eq) continue;
        std::string field_var(argv[i], eq - argv[i]);
        const char *field_val = eq + 1;

        xmpp_stanza_t *field = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(field, "field");
        xmpp_stanza_set_attribute(field, "var", field_var.c_str());
        xmpp_stanza_t *value = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(value, "value");
        xmpp_stanza_t *value_text = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_text(value_text, field_val);
        xmpp_stanza_add_child(value, value_text);
        xmpp_stanza_release(value_text);
        xmpp_stanza_add_child(field, value);
        xmpp_stanza_release(value);
        xmpp_stanza_add_child(x_form, field);
        xmpp_stanza_release(field);
    }

    xmpp_stanza_add_child(command, x_form);
    xmpp_stanza_release(x_form);
    xmpp_stanza_add_child(iq, command);
    xmpp_stanza_release(command);
    ptr_account->connection.send(iq);
    xmpp_stanza_release(iq);
    // freed by submit_id_g

    weechat_printf(buffer, "%sxmpp: submitting form for command %s (session %s)…",
                  weechat_prefix("network"), node, session_id);
    return WEECHAT_RC_OK;
}

/* -----------------------------------------------------------------------
 * XEP-0045 MUC management commands: /kick, /ban, /topic, /nick
 * ----------------------------------------------------------------------- */

int command__kick(const void *pointer, void *data,
                  struct t_gui_buffer *buffer, int argc,
                  char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_channel || ptr_channel->type != weechat::channel::chat_type::MUC)
    {
        weechat_printf(buffer,
                        _("%s%s: \"%s\" command can only be executed in a MUC buffer"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, "kick");
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        weechat_printf(buffer,
                        _("%s%s: you are not connected to server"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    if (argc < 2)
    {
        weechat_printf(buffer,
                        _("%s%s: missing argument for \"%s\" command"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, "kick");
        return WEECHAT_RC_OK;
    }

    const char *nick = argv[1];
    const char *reason = argc > 2 ? argv_eol[2] : NULL;

    /* IQ set to room: <query xmlns='…muc#admin'><item nick='NICK' role='none'/></query> */
    xmpp_string_guard kick_id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
    xmpp_stanza_t *iq = xmpp_iq_new(ptr_account->context, "set", kick_id_g.c_str());
    xmpp_stanza_set_to(iq, ptr_channel->id.data());

    xmpp_stanza_t *query = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(query, "query");
    xmpp_stanza_set_ns(query, "http://jabber.org/protocol/muc#admin");

    xmpp_stanza_t *item = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(item, "item");
    xmpp_stanza_set_attribute(item, "nick", nick);
    xmpp_stanza_set_attribute(item, "role", "none");

    if (reason)
    {
        xmpp_stanza_t *reason_elem = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(reason_elem, "reason");
        xmpp_stanza_t *reason_text = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_text(reason_text, reason);
        xmpp_stanza_add_child(reason_elem, reason_text);
        xmpp_stanza_release(reason_text);
        xmpp_stanza_add_child(item, reason_elem);
        xmpp_stanza_release(reason_elem);
    }

    xmpp_stanza_add_child(query, item);
    xmpp_stanza_release(item);
    xmpp_stanza_add_child(iq, query);
    xmpp_stanza_release(query);

    ptr_account->connection.send(iq);
    xmpp_stanza_release(iq);

    weechat_printf(buffer, _("%sKicked %s from %s%s%s"),
                   weechat_prefix("network"), nick,
                   ptr_channel->id.data(),
                   reason ? ": " : "",
                   reason ? reason : "");

    return WEECHAT_RC_OK;
}

int command__ban(const void *pointer, void *data,
                 struct t_gui_buffer *buffer, int argc,
                 char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_channel || ptr_channel->type != weechat::channel::chat_type::MUC)
    {
        weechat_printf(buffer,
                        _("%s%s: \"%s\" command can only be executed in a MUC buffer"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, "ban");
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        weechat_printf(buffer,
                        _("%s%s: you are not connected to server"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    if (argc < 2)
    {
        weechat_printf(buffer,
                        _("%s%s: missing argument for \"%s\" command"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, "ban");
        return WEECHAT_RC_OK;
    }

    const char *target_jid = argv[1];
    const char *reason = argc > 2 ? argv_eol[2] : NULL;

    /* IQ set to room: <query xmlns='…muc#admin'><item jid='JID' affiliation='outcast'/></query> */
    xmpp_string_guard ban_id_g(ptr_account->context, xmpp_uuid_gen(ptr_account->context));
    xmpp_stanza_t *iq = xmpp_iq_new(ptr_account->context, "set", ban_id_g.c_str());
    xmpp_stanza_set_to(iq, ptr_channel->id.data());

    xmpp_stanza_t *query = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(query, "query");
    xmpp_stanza_set_ns(query, "http://jabber.org/protocol/muc#admin");

    xmpp_stanza_t *item = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(item, "item");
    xmpp_stanza_set_attribute(item, "jid", target_jid);
    xmpp_stanza_set_attribute(item, "affiliation", "outcast");

    if (reason)
    {
        xmpp_stanza_t *reason_elem = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(reason_elem, "reason");
        xmpp_stanza_t *reason_text = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_text(reason_text, reason);
        xmpp_stanza_add_child(reason_elem, reason_text);
        xmpp_stanza_release(reason_text);
        xmpp_stanza_add_child(item, reason_elem);
        xmpp_stanza_release(reason_elem);
    }

    xmpp_stanza_add_child(query, item);
    xmpp_stanza_release(item);
    xmpp_stanza_add_child(iq, query);
    xmpp_stanza_release(query);

    ptr_account->connection.send(iq);
    xmpp_stanza_release(iq);

    weechat_printf(buffer, _("%sBanned %s from %s%s%s"),
                   weechat_prefix("network"), target_jid,
                   ptr_channel->id.data(),
                   reason ? ": " : "",
                   reason ? reason : "");

    return WEECHAT_RC_OK;
}

int command__topic(const void *pointer, void *data,
                   struct t_gui_buffer *buffer, int argc,
                   char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;
    (void) argv;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_channel || ptr_channel->type != weechat::channel::chat_type::MUC)
    {
        weechat_printf(buffer,
                        _("%s%s: \"%s\" command can only be executed in a MUC buffer"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, "topic");
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        weechat_printf(buffer,
                        _("%s%s: you are not connected to server"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    /* <message type='groupchat' to='room'><subject>TEXT</subject></message>
     * An empty <subject/> clears the topic. */
    const char *new_topic = argc > 1 ? argv_eol[1] : "";

    xmpp_stanza_t *msg = xmpp_message_new(ptr_account->context,
                                           "groupchat", ptr_channel->id.data(), NULL);

    xmpp_stanza_t *subject = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(subject, "subject");

    if (argc > 1)
    {
        xmpp_stanza_t *subject_text = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_text(subject_text, new_topic);
        xmpp_stanza_add_child(subject, subject_text);
        xmpp_stanza_release(subject_text);
    }

    xmpp_stanza_add_child(msg, subject);
    xmpp_stanza_release(subject);

    ptr_account->connection.send(msg);
    xmpp_stanza_release(msg);

    return WEECHAT_RC_OK;
}

int command__muc_nick(const void *pointer, void *data,
                      struct t_gui_buffer *buffer, int argc,
                      char **argv, char **argv_eol)
{
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;
    (void) argv_eol;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_channel || ptr_channel->type != weechat::channel::chat_type::MUC)
    {
        weechat_printf(buffer,
                        _("%s%s: \"%s\" command can only be executed in a MUC buffer"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, "nick");
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        weechat_printf(buffer,
                        _("%s%s: you are not connected to server"),
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    if (argc < 2)
    {
        /* Print current nick */
        const char *current_nick = weechat_buffer_get_string(buffer, "localvar_nick");
        weechat_printf(buffer, _("%sCurrent nick in %s: %s"),
                        weechat_prefix("network"),
                        ptr_channel->id.data(),
                        current_nick ? current_nick : "(unknown)");
        return WEECHAT_RC_OK;
    }

    const char *new_nick = argv[1];

    /* Send presence to room@muc/newnick — the server will respond with
     * a presence from room@muc/newnick that updates our local nick. */
    const char *new_full_jid = xmpp_jid_new(
        ptr_account->context,
        xmpp_jid_node(ptr_account->context, ptr_channel->id.data()),
        xmpp_jid_domain(ptr_account->context, ptr_channel->id.data()),
        new_nick);

    xmpp_stanza_t *pres = xmpp_presence_new(ptr_account->context);
    xmpp_stanza_set_from(pres, ptr_account->jid().data());
    xmpp_stanza_set_to(pres, new_full_jid);

    ptr_account->connection.send(pres);
    xmpp_stanza_release(pres);

    return WEECHAT_RC_OK;
}

void command__init()
{
    auto *hook = weechat_hook_command(
        "account",
        N_("handle xmpp accounts"),
        N_("list"
           " || add <name> <jid> <password>"
           " || connect <account>"
           " || disconnect <account>"
           " || reconnect <account>"
           " || delete <account>"),
        N_("      list: list accounts\n"
           "       add: add a xmpp account\n"
           "   connect: connect to a xmpp account\n"
           "disconnect: disconnect from a xmpp account\n"
           " reconnect: reconnect an xmpp account\n"
           "    delete: delete a xmpp account\n"),
        "list"
        " || add %(xmpp_account)"
        " || connect %(xmpp_account)"
        " || disconnect %(xmpp_account)"
        " || reconnect %(xmpp_account)"
        " || delete %(xmpp_account)",
        &command__account, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /account");

    hook = weechat_hook_command(
        "enter",
        N_("enter an xmpp multi-user-chat (muc)"),
        N_("<jid>"),
        N_("jid: muc to enter"),
        NULL, &command__enter, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /enter");

    // IRC-style alias for /enter
    hook = weechat_hook_command(
        "join",
        N_("join an xmpp multi-user-chat (muc) - IRC alias for /enter"),
        N_("<jid>"),
        N_("jid: muc to join"),
        NULL, &command__enter, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /join");

    hook = weechat_hook_command(
        "open",
        N_("open a direct xmpp chat"),
        N_("<jid>"),
        N_("jid: jid to target, or nick from the current muc"),
        NULL, &command__open, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /open");

    // IRC-style alias for /open
    hook = weechat_hook_command(
        "query",
        N_("open a direct xmpp chat - IRC alias for /open"),
        N_("<jid> [<message>]"),
        N_("   jid: jid to target\nmessage: optional initial message"),
        NULL, &command__open, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /query");

    hook = weechat_hook_command(
        "msg",
        N_("send a xmpp message to the current buffer"),
        N_("<message>"),
        N_("message: message to send"),
        NULL, &command__msg, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /msg");

    hook = weechat_hook_command(
        "me",
        N_("send a xmpp action to the current buffer"),
        N_("<message>"),
        N_("message: message to send"),
        NULL, &command__me, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /me");

    hook = weechat_hook_command(
        "invite",
        N_("invite a user to the current MUC room (XEP-0249)"),
        N_("<jid> [<reason>]"),
        N_("    jid: user to invite\n reason: optional invitation message"),
        NULL, &command__invite, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /invite");

    hook = weechat_hook_command(
        "selfping",
        N_("send a self-ping to verify MUC membership (XEP-0410)"),
        N_(""),
        N_("Send a ping to your own MUC nickname to verify you are still in the room"),
        NULL, &command__selfping, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /selfping");

    hook = weechat_hook_command(
        "mam",
        N_("retrieve mam messages for the current channel"),
        N_("[days]"),
        N_("days: number of days to fetch (default: " STR(MAM_DEFAULT_DAYS) ")"),
        NULL, &command__mam, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /mam");

    hook = weechat_hook_command(
        "omemo",
        N_("manage omemo encryption for current buffer or account"),
        N_("[check|republish|status|reset-keys|fingerprint [<jid>]|trust <jid>|devices [<jid>]]"),
        N_("       check: query server for published OMEMO devicelist and bundle\n"
           "   republish: republish OMEMO devicelist and bundle to server\n"
           "      status: show OMEMO status and device ID\n"
           "  reset-keys: reset OMEMO key database to force renegotiation\n"
           " fingerprint: show hex fingerprint of own identity key, or of a peer JID\n"
           "       trust: remove stored identity key for <jid> to re-accept on next contact\n"
           "     devices: show known device IDs for <jid>\n"
           "\n"
           "Without arguments on a channel buffer: enable OMEMO encryption\n"
           "\n"
           "Examples:\n"
           "  /omemo                   : enable OMEMO for current channel\n"
           "  /omemo check             : verify bundle is published on server\n"
           "  /omemo republish         : republish bundle (fixes missing device keys)\n"
           "  /omemo status            : show OMEMO status and device ID\n"
           "  /omemo reset-keys        : reset OMEMO key database\n"
           "  /omemo fingerprint       : show own identity key fingerprint\n"
           "  /omemo fingerprint alice@example.com : show peer fingerprints\n"
           "  /omemo trust alice@example.com       : re-accept changed keys from alice\n"
           "  /omemo devices alice@example.com     : show alice's known devices"),
        "check"
        " || republish"
        " || status"
        " || reset-keys"
        " || fingerprint %(jabber_jids)"
        " || trust %(jabber_jids)"
        " || devices %(jabber_jids)", &command__omemo, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /omemo");

    hook = weechat_hook_command(
        "pgp",
        N_("manage PGP encryption for current channel"),
        N_("[<keyid> | status | reset]"),
        N_("keyid: recipient keyid (add key)\n"
           "status: show configured PGP keys\n"
           "reset: remove all configured PGP keys"),
        "status || reset", &command__pgp, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /pgp");

    hook = weechat_hook_command(
        "plain",
        N_("set the current buffer to use no encryption"),
        N_(""),
        N_(""),
        NULL, &command__plain, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /plain");

    hook = weechat_hook_command(
        "block",
        N_("block one or more JIDs"),
        N_("<jid> [<jid>...]"),
        N_("jid: JID to block"),
        NULL, &command__block, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /block");

    hook = weechat_hook_command(
        "unblock",
        N_("unblock one or more JIDs (or all if no JID specified)"),
        N_("[<jid> [<jid>...]]"),
        N_("jid: JID to unblock (omit to unblock all)"),
        NULL, &command__unblock, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /unblock");

    hook = weechat_hook_command(
        "blocklist",
        N_("list all blocked JIDs"),
        N_(""),
        N_(""),
        NULL, &command__blocklist, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /blocklist");

    hook = weechat_hook_command(
        "xml",
        N_("send a raw xml stanza"),
        N_("<stanza>"),
        N_("stanza: xml to send"),
        NULL, &command__xml, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /xml");

    hook = weechat_hook_command(
        "xmpp",
        N_("get xmpp plugin version (see /help for general help)"),
        N_(""),
        N_(""),
        NULL, &command__xmpp, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /xmpp");

    hook = weechat_hook_command(
        "trap",
        N_("debug trap (int3)"),
        N_(""),
        N_(""),
        NULL, &command__trap, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /trap");

    hook = weechat_hook_command(
        "ping",
        N_("send an xmpp ping"),
        N_("[jid]"),
        N_("jid: optional target jid (defaults to current channel or server)"),
        NULL, &command__ping, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /ping");

    hook = weechat_hook_command(
        "mood",
        N_("set or clear your mood (XEP-0107)"),
        N_("[<mood> [text]]"),
        N_("  mood: mood value (happy, sad, angry, excited, tired, etc.)\n"
           "  text: optional descriptive text\n\n"
           "Sets your mood which is published via PEP and visible to contacts.\n"
           "Examples:\n"
           "  /mood happy\n"
           "  /mood excited Working on a cool project!\n"
           "  /mood tired\n"
           "  /mood (clears mood)"),
        NULL, &command__mood, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /mood");

    hook = weechat_hook_command(
        "activity",
        N_("set or clear your activity (XEP-0108)"),
        N_("[<category>[/<specific>] [text]]"),
        N_("category: activity category (working, relaxing, eating, drinking, etc.)\n"
           "specific: optional specific activity (e.g., working/coding)\n"
           "    text: optional descriptive text\n\n"
           "Sets your current activity which is published via PEP.\n"
           "Examples:\n"
           "  /activity working\n"
           "  /activity working/coding\n"
           "  /activity working/coding Implementing XEP-0108\n"
           "  /activity relaxing/reading\n"
           "  /activity eating\n"
           "  /activity (clears activity)\n\n"
           "Categories: doing_chores, drinking, eating, exercising, grooming,\n"
           "            having_appointment, inactive, relaxing, talking, traveling, working"),
        NULL, &command__activity, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /activity");

    hook = weechat_hook_command(
        "edit",
        N_("edit the last message sent"),
        N_("<message>"),
        N_("message: new message text to replace the last one"),
        NULL, &command__edit, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /edit");

    hook = weechat_hook_command(
        "retract",
        N_("retract (delete) the last message sent (XEP-0424)"),
        N_(""),
        N_("Retracts the last message you sent in the current buffer.\n"
           "This sends a retraction request to all recipients.\n"
           "Note: Recipients may have already seen the message."),
        NULL, &command__retract, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /retract");

    hook = weechat_hook_command(
        "react",
        N_("react to the last message with an emoji (XEP-0444)"),
        N_("<emoji>"),
        N_("emoji: emoji reaction (e.g., 👍 😀 ❤️ 🎉)\n\n"
           "Reacts to the last message (not yours) in the buffer.\n"
           "Examples:\n"
           "  /react 👍\n"
           "  /react ❤️\n"
           "  /react 😂"),
        NULL, &command__react, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /react");

    hook = weechat_hook_command(
        "reply",
        N_("reply to the last message (XEP-0461)"),
        N_("<message>"),
        N_("message: your reply text\n\n"
           "Replies to the last message (not yours) in the buffer with context.\n"
           "The reply includes a reference to the original message.\n"
           "Examples:\n"
           "  /reply Thanks for the info!\n"
           "  /reply I agree with that"),
        NULL, &command__reply, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /reply");

    hook = weechat_hook_command(
        "moderate",
        N_("moderate (retract) the last message in MUC (XEP-0425)"),
        N_("[reason]"),
        N_("reason: optional reason for moderation\n\n"
           "Moderates (retracts) the last message in the current MUC room.\n"
           "This command is for MUC moderators to remove messages from other users.\n"
           "Use /retract to delete your own messages.\n"
           "Examples:\n"
           "  /moderate\n"
           "  /moderate Spam message\n"
           "  /moderate Violates community guidelines"),
        NULL, &command__moderate, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /moderate");

    hook = weechat_hook_command(
        "disco",
        N_("discover services and features (XEP-0030)"),
        N_("[jid]"),
        N_("jid: optional target jid (defaults to server domain)"),
        NULL, &command__disco, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /disco");

    hook = weechat_hook_command(
        "roster",
        N_("manage XMPP roster (contact list)"),
        N_("|| add <jid> [name] || del <jid>"),
        N_("      : display roster\n"
           "  add : add contact to roster\n"
           "  del : remove contact from roster (also: delete, remove)\n"
           "  jid : Jabber ID of the contact\n"
           " name : optional display name for the contact"),
        "add|del|delete|remove", &command__roster, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /roster");
    
    hook = weechat_hook_command(
        "bookmark",
        N_("manage XMPP bookmarks (XEP-0048)"),
        N_("|| add [jid] [name] || del <jid> || autojoin <jid> <on|off>"),
        N_("         : display bookmarks\n"
           "     add : bookmark current MUC or specified JID\n"
           "     del : remove bookmark (also: delete, remove)\n"
           "autojoin : enable/disable autojoin for a bookmark\n"
           "     jid : Jabber ID of the MUC room\n"
           "    name : optional display name for the bookmark\n"
           "\n"
           "Examples:\n"
           "  /bookmark                              : list all bookmarks\n"
           "  /bookmark add                          : bookmark current MUC\n"
           "  /bookmark add room@conference.example.com My Room\n"
           "  /bookmark del room@conference.example.com\n"
           "  /bookmark autojoin room@conference.example.com on"),
        "add|del|delete|remove|autojoin", &command__bookmark, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /bookmark");
    
    hook = weechat_hook_command(
        "list",
        N_("search for public MUC rooms (XEP-0433)"),
        N_("[[<service>] [keywords]]"),
        N_("service  : search service JID (default: search.jabber.network)\n"
           "keywords : search keywords (optional)\n"
           "\n"
           "Search for public MUC rooms using XEP-0433 Extended Channel Search.\n"
           "Without keywords, shows popular rooms sorted by number of users.\n"
           "\n"
           "Examples:\n"
           "  /list                            : list popular rooms\n"
           "  /list xmpp                       : search for rooms about XMPP\n"
           "  /list linux gaming               : search for linux gaming rooms\n"
           "  /list search.jabber.network xmpp : use specific search service"),
        NULL, &command__list, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /list");

    hook = weechat_hook_command(
        "upload",
        N_("upload a file via HTTP File Upload (XEP-0363)"),
        N_("[<filename>]"),
        N_("filename: path to file to upload (optional)\n\n"
           "If no filename is provided, an interactive file picker will be used.\n"
           "The picker tries GUI dialogs (zenity/kdialog) when X11/Wayland is detected,\n"
           "falls back to fzf in terminal, or prompts for manual entry.\n\n"
           "The file will be uploaded to the server and a URL will be sent in the chat."),
        "%(filename)", &command__upload, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /upload");

    hook = weechat_hook_command(
        "whois",
        N_("get vCard information about an XMPP user (XEP-0054)"),
        N_("[<jid>]"),
        N_("jid: user JID to query (uses current PM channel if not specified)"),
        NULL, &command__whois, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /whois");

    hook = weechat_hook_command(
        "setvcard",
        N_("publish a field of your own vCard (XEP-0054)"),
        N_("<field> <value>"),
        N_("field: vCard field to set (fn, nickname, email, url, desc, org, title, tel, bday, note)\n"
           "value: the value to set for the field\n\n"
           "Publishes a single field of your own vCard via IQ set (XEP-0054).\n"
           "Example: /setvcard fn Alice Smith\n"
           "         /setvcard email alice@example.com\n"
           "Note: only the specified field is sent; other fields are unaffected on\n"
           "      the server only if the server merges — most servers replace the\n"
           "      entire vCard, so run /whois on yourself first to see current values."),
        "fn|nickname|email|url|desc|org|title|tel|bday|note", &command__setvcard, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /setvcard");

    hook = weechat_hook_command(
        "setavatar",
        N_("publish a local image file as your own avatar (XEP-0084)"),
        N_("<filepath>"),
        N_("filepath: path to a PNG, JPEG, GIF, or WEBP image file\n\n"
           "Reads the file, base64-encodes it, and publishes it to your\n"
           "urn:xmpp:avatar:data and urn:xmpp:avatar:metadata PEP nodes.\n"
           "Also updates the <photo> element in subsequent presences (XEP-0153).\n\n"
           "Example:\n"
           "  /setavatar /home/alice/photo.png\n"
           "  /setavatar ~/pictures/avatar.jpg"),
        NULL, &command__setavatar, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /setavatar");

    hook = weechat_hook_command(
        "buzz",
        N_("request a contact's attention (XEP-0224)"),
        N_(""),
        N_("Sends an attention request to the contact in the current PM channel.\n"
           "This is the XMPP equivalent of a 'buzz' or 'nudge'.\n"
           "Note: can only be used in PM channels, not MUC rooms."),
        NULL, &command__buzz, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /buzz");

    hook = weechat_hook_command(
        "spoiler",
        N_("send a spoiler message (XEP-0382)"),
        N_("[<hint:>] <message>"),
        N_("hint   : optional spoiler warning hint (end with ':')\n"
           "message: the spoiler message text\n\n"
           "Sends a message marked as a spoiler. Supporting clients will hide\n"
           "the message body behind a warning.\n\n"
           "Examples:\n"
           "  /spoiler The butler did it.\n"
           "  /spoiler Movie ending: The hero sacrifices himself.\n"
           "  /spoiler TW: violence: The scene is quite graphic."),
        NULL, &command__spoiler, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /spoiler");

    hook = weechat_hook_command(
        "adhoc",
        N_("list and execute XMPP ad-hoc commands (XEP-0050)"),
        N_("<jid>"
           " || <jid> <node>"
           " || <jid> <node> <sessionid> [<field>=<value> ...]"),
        N_("      jid: target JID (server, component, or user)\n"
           "     node: command node URI to execute\n"
           "sessionid: session ID returned by a previous command step\n"
           "field=val: form field values to submit\n\n"
           "Without a node, lists available commands on the target JID.\n"
           "With a node, executes that command (first step).\n"
           "With a sessionid and field=value pairs, submits a form step.\n\n"
           "Examples:\n"
           "  /adhoc conference.example.com\n"
           "  /adhoc example.com http://jabber.org/protocol/admin#get-active-users\n"
           "  /adhoc example.com http://jabber.org/protocol/admin#change-user-password"
           " abc123 username=bob password=newpass"),
        NULL, &command__adhoc, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /adhoc");

    hook = weechat_hook_command(
        "kick",
        N_("kick a user from a MUC room (XEP-0045)"),
        N_("<nick> [<reason>]"),
        N_("  nick: nickname of the user to kick\n"
           "reason: optional reason for the kick\n\n"
           "Requires moderator role in the room.\n\n"
           "Examples:\n"
           "  /kick annoyinguser\n"
           "  /kick spammer Repeatedly posting spam"),
        NULL, &command__kick, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /kick");

    hook = weechat_hook_command(
        "ban",
        N_("ban a user from a MUC room (XEP-0045)"),
        N_("<jid> [<reason>]"),
        N_("   jid: bare JID of the user to ban\n"
           "reason: optional reason for the ban\n\n"
           "Sets the user's affiliation to 'outcast'. Requires admin or owner role.\n\n"
           "Examples:\n"
           "  /ban bad@example.com\n"
           "  /ban troll@example.com Persistent harassment"),
        NULL, &command__ban, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /ban");

    hook = weechat_hook_command(
        "topic",
        N_("set or clear the MUC room topic (XEP-0045)"),
        N_("[<text>]"),
        N_("text: new topic text (omit to clear the topic)\n\n"
           "Examples:\n"
           "  /topic Welcome to #general!\n"
           "  /topic"),
        NULL, &command__topic, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /topic");

    hook = weechat_hook_command(
        "nick",
        N_("change your nickname in the current MUC room (XEP-0045)"),
        N_("[<newnick>]"),
        N_("newnick: new nickname to use (shows current nick if omitted)\n\n"
           "Examples:\n"
           "  /nick mynewnick"),
        NULL, &command__muc_nick, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /nick");
}
