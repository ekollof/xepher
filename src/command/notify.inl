// XEP-0492: Chat Notification Settings — /notify command
// /notify [<jid>] [always|on-mention|never]
//
// Without arguments: shows the current setting for the active buffer.
// With JID only: shows the current setting for that JID.
// With JID + level: sets the notification preference, stores it in the
// bookmark's <extensions><notify> and re-publishes via XEP-0402.

int command__notify(const void *pointer, void *data,
                    struct t_gui_buffer *buffer, int argc,
                    char **argv, char **argv_eol)
{
    (void) pointer;
    (void) data;
    (void) argv_eol;

    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;
    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    // Determine target JID: from argument or from current channel.
    std::string target_jid;
    std::string level;

    if (argc >= 2)
    {
        target_jid = argv[1];
    }
    else if (ptr_channel)
    {
        target_jid = ptr_channel->name;
    }
    else
    {
        weechat_printf(ptr_account->buffer,
                       "%s%s: /notify [<jid>] [always|on-mention|never]",
                       weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
        return WEECHAT_RC_OK;
    }

    if (argc >= 3)
    {
        level = argv[2];
        // Validate level
        if (level != "always" && level != "on-mention" && level != "never")
        {
            weechat_printf(ptr_account->buffer,
                           "%s%s: /notify: level must be 'always', 'on-mention', or 'never'",
                           weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
            return WEECHAT_RC_OK;
        }
    }

    // Show current setting if no level given
    if (level.empty())
    {
        auto it = ptr_account->bookmarks.find(target_jid);
        std::string setting = (it != ptr_account->bookmarks.end() && !it->second.notify_setting.empty())
                              ? it->second.notify_setting : "(default)";
        weechat_printf(ptr_account->buffer,
                       "%s%s: notification setting for %s: %s",
                       weechat_prefix("network"), WEECHAT_XMPP_PLUGIN_NAME,
                       target_jid.c_str(), setting.c_str());
        return WEECHAT_RC_OK;
    }

    // Apply the setting to the bookmark and re-publish.
    if (ptr_account->bookmarks.find(target_jid) == ptr_account->bookmarks.end())
    {
        // Create a minimal bookmark entry so we can persist the setting.
        ptr_account->bookmarks[target_jid].jid = target_jid;
    }

    ptr_account->bookmarks[target_jid].notify_setting = level;

    weechat_printf(ptr_account->buffer,
                   "%s%s: notification setting for %s set to '%s'",
                   weechat_prefix("network"), WEECHAT_XMPP_PLUGIN_NAME,
                   target_jid.c_str(), level.c_str());

    ptr_account->send_bookmarks();

    return WEECHAT_RC_OK;
}
