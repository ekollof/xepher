// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <stdint.h>
#include <string.h>
#include <strophe.h>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "account.hh"
#include "channel.hh"
#include "buffer.hh"

void buffer__get_account_and_channel(struct t_gui_buffer *buffer,
                                     weechat::account **account,
                                     weechat::channel **channel)
{
    if (!buffer)
        return;

    *account = nullptr;
    *channel = nullptr;

    /* look for a account or channel using this buffer */
    for (auto& ptr_account : weechat::accounts)
    {
        if (ptr_account.second.buffer == buffer)
        {
            if (account)
                *account = &ptr_account.second;
            return;
        }

        for (auto& ptr_channel : ptr_account.second.channels)
        {
            if (ptr_channel.second.buffer == buffer)
            {
                if (account)
                    *account = &ptr_account.second;
                if (channel)
                    *channel = &ptr_channel.second;
                return;
            }
        }
    }
}

char *buffer__encryption_bar_cb(const void *pointer, void *data,
                                struct t_gui_bar_item *item,
                                struct t_gui_window *window,
                                struct t_gui_buffer *buffer,
                                struct t_hashtable *extra_info)
{
    weechat::account *account;
    weechat::channel *channel;

    (void) pointer;
    (void) data;
    (void) item;
    (void) window;
    (void) extra_info;

    account = NULL;
    channel = NULL;

    buffer__get_account_and_channel(buffer, &account, &channel);

    if (!channel)
        return strdup("");

    // Check encryption status
    if (channel->omemo.enabled)
        return strdup("🔒OMEMO");
    else if (channel->pgp.enabled)
        return strdup("🔒PGP");
    else if (channel->transport != weechat::channel::transport::PLAIN)
    {
        std::string status = "🔒";
        status += weechat::channel::transport_name(channel->transport);
        return strdup(status.c_str());
    }
    else
        return strdup("");
}

int buffer__switch_cb(const void *pointer, void *data,
                      const char *signal, const char *type_data,
                      void *signal_data)
{
    (void) pointer;
    (void) data;
    (void) signal;
    (void) type_data;

    // Update encryption bar item when switching buffers
    weechat_bar_item_update("xmpp_encryption");

    struct t_gui_buffer *new_buffer = static_cast<struct t_gui_buffer*>(signal_data);

    // Track the previously-active buffer so we can send <inactive> to it
    static struct t_gui_buffer *prev_buffer = nullptr;

    // Send <inactive> to the channel we just left (if it was an XMPP channel)
    if (prev_buffer && prev_buffer != new_buffer)
    {
        weechat::account *prev_account = nullptr;
        weechat::channel *prev_channel = nullptr;
        buffer__get_account_and_channel(prev_buffer, &prev_account, &prev_channel);
        if (prev_account && prev_channel && prev_account->connected())
        {
            auto *self_user = weechat::user::search(prev_account,
                                                    prev_account->jid_device().data());
            prev_channel->send_inactive(self_user);
            // XEP-0490: flush unread markers when leaving the buffer so that
            // other devices know we have read all pending messages here.
            prev_channel->send_reads();
        }
    }

    // Send <active> to the channel we just switched into (if it is an XMPP channel)
    if (new_buffer)
    {
        weechat::account *new_account = nullptr;
        weechat::channel *new_channel = nullptr;
        buffer__get_account_and_channel(new_buffer, &new_account, &new_channel);
        if (new_account && new_channel && new_account->connected())
        {
            auto *self_user = weechat::user::search(new_account,
                                                    new_account->jid_device().data());
            new_channel->send_active(self_user);
        }
    }

    prev_buffer = new_buffer;

    return WEECHAT_RC_OK;
}

int buffer__nickcmp_cb(const void *pointer, void *data,
                       struct t_gui_buffer *buffer,
                       const char *nick1,
                       const char *nick2)
{
    weechat::account *account;

    (void) data;

    if (pointer)
        account = (weechat::account *)pointer;
    else
        buffer__get_account_and_channel(buffer, &account, NULL);

    if (account)
    {
        return weechat_strcasecmp(nick1, nick2);
    }
    else
    {
        return weechat_strcasecmp(nick1, nick2);
    }
}

int buffer__close_cb(const void *pointer, void *data,
                     struct t_gui_buffer *buffer)
{
    struct t_weechat_plugin *buffer_plugin = NULL;
    weechat::account *ptr_account = NULL;
    weechat::channel *ptr_channel = NULL;

    (void) pointer;
    (void) data;

    // Safety check: if plugin instance is gone, we're shutting down
    if (!weechat::plugin::instance || !weechat::plugin::instance->ptr())
        return WEECHAT_RC_OK;

    buffer_plugin = (struct t_weechat_plugin*)weechat_buffer_get_pointer(buffer, "plugin");
    if (buffer_plugin != weechat_plugin)
        return WEECHAT_RC_OK;
    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    const char* type = weechat_buffer_get_string(buffer, "localvar_type");

    if (weechat_strcasecmp(type, "server") == 0)
    {
        if (ptr_account)
        {
            if (ptr_account->connected())
            {
                ptr_account->disconnect(0);
            }

            ptr_account->buffer = NULL;
        }
    }
    else if (weechat_strcasecmp(type, "channel") == 0)
    {
        if (ptr_account && ptr_channel)
        {
            if (ptr_account->connected())
            {
                // Send unavailable presence to leave MUC
                if (ptr_channel->type == weechat::channel::chat_type::MUC)
                {
                    xmpp_stanza_t *pres = xmpp_presence_new(ptr_account->context);
                    xmpp_stanza_set_type(pres, "unavailable");
                    
                    // Build full JID with resource (room@server/nick)
                    const char* nick = weechat_buffer_get_string(buffer, "localvar_nick");
                    if (nick && nick[0])
                    {
                        xmpp_string_guard node_g(ptr_account->context,
                            xmpp_jid_node(ptr_account->context, ptr_channel->id.data()));
                        xmpp_string_guard domain_g(ptr_account->context,
                            xmpp_jid_domain(ptr_account->context, ptr_channel->id.data()));
                        xmpp_string_guard pres_jid_g(ptr_account->context,
                            xmpp_jid_new(ptr_account->context,
                                node_g.c_str(), domain_g.c_str(), nick));
                        xmpp_stanza_set_to(pres, pres_jid_g.c_str());
                    }
                    else
                    {
                        xmpp_stanza_set_to(pres, ptr_channel->id.data());
                    }
                    
                    xmpp_stanza_set_from(pres, ptr_account->jid().data());
                    
                    ptr_account->connection.send( pres);
                    xmpp_stanza_release(pres);
                }
                
                ptr_account->channels.erase(ptr_channel->name);
            }
        }
    }
    else if (weechat_strcasecmp(type, "private") == 0)
    {
        if (ptr_account && ptr_channel)
        {
            if (ptr_account->connected())
            {
                // Send "gone" chat state when closing PM
                if (ptr_channel->type == weechat::channel::chat_type::PM)
                {
                    auto *user = weechat::user::search(ptr_account, ptr_account->jid_device().data());
                    ptr_channel->send_gone(user);
                }
                
                ptr_account->channels.erase(ptr_channel->name);
            }
        }
    }
    else
    {
    }

    return WEECHAT_RC_OK;
}
