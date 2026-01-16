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
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "account.hh"
#include "user.hh"
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

    account->name = strdup(name);
    if (jid)
        account->jid(jid);
    if (password)
        account->password(password);
    if (jid)
        account->nickname(xmpp_jid_node(account->context, jid));

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
    int i, nb_connect, connect_ok;
    weechat::account *ptr_account = nullptr;

    (void) buffer;
    (void) argc;
    (void) argv;

    connect_ok = 1;

    nb_connect = 0;
    for (i = 2; i < argc; i++)
    {
        nb_connect++;
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
    int i, nb_disconnect, disconnect_ok;
    weechat::account *ptr_account;

    (void) argc;
    (void) argv;

    disconnect_ok = 1;

    nb_disconnect = 0;
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
        nb_disconnect++;
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
    char *jid, *pres_jid, *text;

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
            jid = xmpp_jid_bare(ptr_account->context, jids[i]);
            pres_jid = jids[i];

            if(!xmpp_jid_resource(ptr_account->context, pres_jid))
                pres_jid = xmpp_jid_new(
                    ptr_account->context,
                    xmpp_jid_node(ptr_account->context, jid),
                    xmpp_jid_domain(ptr_account->context, jid),
                    ptr_account->nickname().data()
                    && strlen(ptr_account->nickname().data())
                    ? ptr_account->nickname().data()
                    : xmpp_jid_node(ptr_account->context,
                                    ptr_account->jid().data()));

            if (!ptr_account->channels.contains(jid))
            {
                ptr_channel = &ptr_account->channels.emplace(
                    std::make_pair(jid, weechat::channel {
                            *ptr_account, weechat::channel::chat_type::MUC, jid, jid
                        })).first->second;
                ptr_account->load_pgp_keys();
            }
	    if (!ptr_channel) {
		weechat_string_free_split(jids); // raii unique_ptr?
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

            xmpp_send(ptr_account->connection, pres);
            xmpp_stanza_release(pres);

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

        pres_jid = xmpp_jid_new(
            ptr_account->context,
            xmpp_jid_node(ptr_account->context, buffer_jid),
            xmpp_jid_domain(ptr_account->context, buffer_jid),
            weechat_buffer_get_string(buffer, "localvar_nick"));

        if (!ptr_account->channels.contains(buffer_jid))
            ptr_channel = &ptr_account->channels.emplace(
                std::make_pair(jid, weechat::channel {
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

        xmpp_send(ptr_account->connection, pres);
        xmpp_stanza_release(pres);
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
            jid = xmpp_jid_bare(ptr_account->context, jids[i]);
            if (ptr_channel && !strchr(jid, '@'))
            {
                jid = xmpp_jid_new(
                    ptr_account->context,
                    xmpp_jid_node(ptr_account->context, ptr_channel->name.data()),
                    xmpp_jid_domain(ptr_account->context, ptr_channel->name.data()),
                    jid);
            }

            pres = xmpp_presence_new(ptr_account->context);
            xmpp_stanza_set_to(pres, jid);
            xmpp_stanza_set_from(pres, ptr_account->jid().data());
            xmpp_send(ptr_account->connection, pres);
            xmpp_stanza_release(pres);

            auto channel = ptr_account->channels.find(jid);
            if (channel == ptr_account->channels.end())
                channel = ptr_account->channels.emplace(
                    std::make_pair(jid, weechat::channel {
                            *ptr_account, weechat::channel::chat_type::PM, jid, jid
                        })).first;

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
        xmpp_send(ptr_account->connection, message);
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
        text = argv_eol[0];

        message = xmpp_message_new(ptr_account->context,
                                   ptr_channel->type == weechat::channel::chat_type::MUC ? "groupchat" : "chat",
                                   ptr_channel->name.data(), NULL);
        xmpp_message_set_body(message, text);
        xmpp_send(ptr_account->connection, message);
        xmpp_stanza_release(message);
        if (ptr_channel->type != weechat::channel::chat_type::MUC)
            weechat_printf_date_tags(ptr_channel->buffer, 0,
                                     "xmpp_message,message,action,private,notify_none,self_msg,log1",
                                     "%s%s %s",
                                     weechat_prefix("action"),
                                     weechat::user::search(ptr_account, ptr_account->jid().data())->as_prefix_raw().data(),
                                     strlen(text) > strlen("/me ") ? text+4 : "");
    }

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
    
    ptr_channel->fetch_mam(xmpp_uuid_gen(ptr_account->context), &start, &end, NULL);

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
    (void) argv;

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
        keyid = argv_eol[1];

        ptr_channel->pgp.ids.emplace(keyid);
        ptr_account->save_pgp_keys();
        weechat::config::write();
    }
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
                xmpp_send(ptr_account->connection, stanza);
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

            xmpp_send(ptr_account->connection, stanza);
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
    weechat::user *user = NULL;

    buffer__get_account_and_channel(buffer, &account, &channel);
    weechat::user::search(account, account->jid_device().data());

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
    
    char *last_msg_id = NULL;
    
    // Search backwards for our last sent message
    while (line && !last_msg_id)
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
                            last_msg_id = strdup(tag_array[i] + 3);
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
    
    if (!last_msg_id)
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

    char *id = xmpp_uuid_gen(ptr_account->context);
    xmpp_stanza_set_id(message, id);
    xmpp_free(ptr_account->context, id);
    xmpp_message_set_body(message, text);

    // Add replace element with original message ID
    xmpp_stanza_t *replace = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(replace, "replace");
    xmpp_stanza_set_ns(replace, "urn:xmpp:message-correct:0");
    xmpp_stanza_set_id(replace, last_msg_id);
    xmpp_stanza_add_child(message, replace);
    xmpp_stanza_release(replace);

    xmpp_stanza_t *message__store = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(message__store, "store");
    xmpp_stanza_set_ns(message__store, "urn:xmpp:hints");
    xmpp_stanza_add_child(message, message__store);
    xmpp_stanza_release(message__store);

    xmpp_send(ptr_account->connection, message);
    xmpp_stanza_release(message);
    
    free(last_msg_id);

    weechat_printf(buffer, "%sxmpp: message edit sent",
                  weechat_prefix("network"));

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
    char *id = xmpp_uuid_gen(ptr_account->context);
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
    
    xmpp_send(ptr_account->connection, iq);
    xmpp_stanza_release(iq);
    xmpp_free(ptr_account->context, id);

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

    char *id = xmpp_uuid_gen(ptr_account->context);
    ptr_account->user_disco_queries.insert(id);
    
    xmpp_stanza_t *iq = xmpp_iq_new(ptr_account->context, "get", id);
    xmpp_stanza_set_to(iq, target);
    
    xmpp_stanza_t *query = xmpp_stanza_new(ptr_account->context);
    xmpp_stanza_set_name(query, "query");
    xmpp_stanza_set_ns(query, "http://jabber.org/protocol/disco#info");
    
    xmpp_stanza_add_child(iq, query);
    xmpp_stanza_release(query);
    
    xmpp_send(ptr_account->connection, iq);
    xmpp_stanza_release(iq);
    xmpp_free(ptr_account->context, id);

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

        char *id = xmpp_uuid_gen(ptr_account->context);
        xmpp_stanza_t *iq = xmpp_iq_new(ptr_account->context, "set", id);
        xmpp_free(ptr_account->context, id);
        
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
        
        xmpp_send(ptr_account->connection, iq);
        
        xmpp_stanza_release(item);
        xmpp_stanza_release(query);
        xmpp_stanza_release(iq);

        weechat_printf(buffer, "%sAdding %s to roster...", 
                      weechat_prefix("network"), jid);

        // Also send presence subscription request
        xmpp_stanza_t *presence = xmpp_presence_new(ptr_account->context);
        xmpp_stanza_set_type(presence, "subscribe");
        xmpp_stanza_set_to(presence, jid);
        xmpp_send(ptr_account->connection, presence);
        xmpp_stanza_release(presence);

        return WEECHAT_RC_OK;
    }

    // /roster del <jid>
    if (argc >= 3 && (weechat_strcasecmp(argv[1], "del") == 0 || 
                       weechat_strcasecmp(argv[1], "delete") == 0 ||
                       weechat_strcasecmp(argv[1], "remove") == 0))
    {
        const char *jid = argv[2];

        char *id = xmpp_uuid_gen(ptr_account->context);
        xmpp_stanza_t *iq = xmpp_iq_new(ptr_account->context, "set", id);
        xmpp_free(ptr_account->context, id);
        
        xmpp_stanza_t *query = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(query, "query");
        xmpp_stanza_set_ns(query, "jabber:iq:roster");
        
        xmpp_stanza_t *item = xmpp_stanza_new(ptr_account->context);
        xmpp_stanza_set_name(item, "item");
        xmpp_stanza_set_attribute(item, "jid", jid);
        xmpp_stanza_set_attribute(item, "subscription", "remove");
        
        xmpp_stanza_add_child(query, item);
        xmpp_stanza_add_child(iq, query);
        
        xmpp_send(ptr_account->connection, iq);
        
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

int command__list(const void *pointer, void *data,
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
        weechat_printf(buffer,
                       _("%s%s: you are not connected to server"),
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    // Build search query
    const char *keywords = (argc >= 2) ? argv_eol[1] : "";
    
    // Build JSON payload
    std::string json_payload = "{\"keywords\":[";
    if (strlen(keywords) > 0)
    {
        json_payload += "\"" + std::string(keywords) + "\"";
    }
    json_payload += "],\"min_users\":1}";
    
    weechat_printf(buffer, "");
    if (strlen(keywords) > 0)
        weechat_printf(buffer, "%sSearching for MUC rooms matching: %s",
                      weechat_prefix("network"), keywords);
    else
        weechat_printf(buffer, "%sSearching for popular MUC rooms...",
                      weechat_prefix("network"));
    
    // Execute search using WeeChat's url_transfer
    struct t_hashtable *options = weechat_hashtable_new(8,
        WEECHAT_HASHTABLE_STRING, WEECHAT_HASHTABLE_STRING,
        NULL, NULL);
    
    weechat_hashtable_set(options, "httpheader", "Content-Type: application/json");
    weechat_hashtable_set(options, "postfields", json_payload.data());
    
    // Use a hook for async URL fetch
    char *url = strdup("https://search.jabber.network/api/1.0/search");
    
    struct t_hook *hook = weechat_hook_process_hashtable(
        fmt::format("url:{}", url).data(),
        options,
        30000, // 30 second timeout
        [](const void *pointer, void *data, const char *command,
           int return_code, const char *out, const char *err) -> int
        {
            (void) data;
            (void) command;
            (void) err;
            
            struct t_gui_buffer *buffer = (struct t_gui_buffer *)pointer;
            
            if (return_code != WEECHAT_HOOK_PROCESS_CHILD)
            {
                if (return_code < 0)
                {
                    weechat_printf(buffer, "%sRoom search failed: network error",
                                  weechat_prefix("error"));
                    return WEECHAT_RC_OK;
                }
                
                if (!out || strlen(out) == 0)
                {
                    weechat_printf(buffer, "%sNo results from room search",
                                  weechat_prefix("error"));
                    return WEECHAT_RC_OK;
                }
                
                // Parse JSON response (simple parsing since we can't rely on external libs)
                std::string response(out);
                
                // Look for "items": [ ... ]
                size_t items_pos = response.find("\"items\":");
                if (items_pos == std::string::npos)
                {
                    weechat_printf(buffer, "%sNo rooms found",
                                  weechat_prefix("network"));
                    return WEECHAT_RC_OK;
                }
                
                // Simple parsing - look for room objects
                weechat_printf(buffer, "");
                weechat_printf(buffer, "%sMUC Rooms:", weechat_prefix("network"));
                weechat_printf(buffer, "");
                
                size_t pos = items_pos;
                int count = 0;
                while ((pos = response.find("\"address\":", pos)) != std::string::npos && count < 20)
                {
                    pos += 11; // skip "address":"
                    size_t end_pos = response.find("\"", pos);
                    if (end_pos == std::string::npos) break;
                    
                    std::string address = response.substr(pos, end_pos - pos);
                    
                    // Find name
                    std::string name = "";
                    size_t name_pos = response.find("\"name\":", pos);
                    if (name_pos != std::string::npos && name_pos < pos + 500)
                    {
                        name_pos += 8;
                        if (response[name_pos - 1] == '"')
                        {
                            size_t name_end = response.find("\"", name_pos);
                            if (name_end != std::string::npos)
                                name = response.substr(name_pos, name_end - name_pos);
                        }
                    }
                    
                    // Find nusers
                    std::string nusers = "";
                    size_t nusers_pos = response.find("\"nusers\":", pos);
                    if (nusers_pos != std::string::npos && nusers_pos < pos + 500)
                    {
                        nusers_pos += 9;
                        size_t nusers_end = response.find_first_of(",}", nusers_pos);
                        if (nusers_end != std::string::npos)
                        {
                            std::string nusers_str = response.substr(nusers_pos, nusers_end - nusers_pos);
                            if (nusers_str != "null")
                                nusers = nusers_str + " users";
                        }
                    }
                    
                    // Find description  
                    std::string description = "";
                    size_t desc_pos = response.find("\"description\":", pos);
                    if (desc_pos != std::string::npos && desc_pos < pos + 500)
                    {
                        desc_pos += 15;
                        if (response[desc_pos - 1] == '"')
                        {
                            size_t desc_end = response.find("\"", desc_pos);
                            if (desc_end != std::string::npos)
                            {
                                description = response.substr(desc_pos, desc_end - desc_pos);
                                if (description.length() > 60)
                                    description = description.substr(0, 57) + "...";
                            }
                        }
                    }
                    
                    // Print room info
                    std::string display = address;
                    if (!name.empty() && name != "null")
                        display = name + " <" + address + ">";
                    
                    std::string info = "";
                    if (!nusers.empty())
                        info = " (" + nusers + ")";
                    
                    weechat_printf(buffer, "  %s%s%s%s",
                                  weechat_color("chat_nick"),
                                  display.c_str(),
                                  weechat_color("reset"),
                                  info.c_str());
                    
                    if (!description.empty() && description != "null")
                        weechat_printf(buffer, "    %s", description.c_str());
                    
                    count++;
                    pos = end_pos;
                }
                
                if (count == 0)
                {
                    weechat_printf(buffer, "%sNo rooms found",
                                  weechat_prefix("network"));
                }
                else
                {
                    weechat_printf(buffer, "");
                    weechat_printf(buffer, "%sUse /enter <address> to join a room",
                                  weechat_prefix("network"));
                }
            }
            
            return WEECHAT_RC_OK;
        },
        buffer, NULL);
    
    free(url);
    weechat_hashtable_free(options);
    
    if (!hook)
    {
        weechat_printf(buffer, "%sFailed to start room search",
                      weechat_prefix("error"));
    }

    return WEECHAT_RC_OK;
}

void command__init()
{
    struct t_hook *hook;

    hook = weechat_hook_command(
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

    hook = weechat_hook_command(
        "open",
        N_("open a direct xmpp chat"),
        N_("<jid>"),
        N_("jid: jid to target, or nick from the current muc"),
        NULL, &command__open, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /open");

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
        "mam",
        N_("retrieve mam messages for the current channel"),
        N_("[days]"),
        N_("days: number of days to fetch (default: " STR(MAM_DEFAULT_DAYS) ")"),
        NULL, &command__mam, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /mam");

    hook = weechat_hook_command(
        "omemo",
        N_("set the current buffer to use omemo encryption"),
        N_(""),
        N_(""),
        NULL, &command__omemo, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /omemo");

    hook = weechat_hook_command(
        "pgp",
        N_("set the current buffer to use pgp encryption (with a given target pgp key)"),
        N_("<keyid>"),
        N_("keyid: recipient keyid"),
        NULL, &command__pgp, NULL, NULL);
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
        "edit",
        N_("edit the last message sent"),
        N_("<message>"),
        N_("message: new message text to replace the last one"),
        NULL, &command__edit, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /edit");

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
        N_("search for public MUC rooms"),
        N_("[keywords]"),
        N_("keywords : search keywords (optional)\n"
           "\n"
           "Search for public MUC rooms using the search.jabber.network directory.\n"
           "Without keywords, shows popular rooms.\n"
           "\n"
           "Examples:\n"
           "  /list                 : list popular rooms\n"
           "  /list xmpp            : search for rooms about XMPP\n"
           "  /list linux gaming    : search for linux gaming rooms"),
        NULL, &command__list, NULL, NULL);
    if (!hook)
        weechat_printf(NULL, "Failed to setup command /list");
}
