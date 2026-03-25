int weechat::account::idle_timer_cb(const void *pointer, void *data, int remaining_calls)
{
    (void) data;
    (void) remaining_calls;

    if (weechat::g_plugin_unloading || !weechat::plugin::instance)
        return WEECHAT_RC_OK;

    account *account = (weechat::account *)pointer;
    if (!account || !account->connection || !xmpp_conn_is_connected(account->connection))
        return WEECHAT_RC_OK;

    time_t now = time(NULL);
    time_t idle_time = now - account->last_activity;

    // If idle for more than 5 minutes and currently active, send inactive
    if (idle_time > 300 && account->csi_active)
    {
        account->connection.send(stanza::xep0352::inactive()
                                .build(account->context)
                                .get());
        account->csi_active = false;
        weechat_printf(account->buffer, "%sClient state: inactive (idle for %ld seconds)",
                      weechat_prefix("network"), idle_time);
    }

    return WEECHAT_RC_OK;
}

// Client State Indication (XEP-0352) - Activity callback
int weechat::account::activity_cb(const void *pointer, void *data,
                                  const char *signal, const char *type_data,
                                  void *signal_data)
{
    (void) data;
    (void) signal;
    (void) type_data;
    (void) signal_data;

    if (weechat::g_plugin_unloading || !weechat::plugin::instance)
        return WEECHAT_RC_OK;

    account *account = (weechat::account *)pointer;
    if (!account || !account->connection || !xmpp_conn_is_connected(account->connection))
        return WEECHAT_RC_OK;

    // Update last activity timestamp
    account->last_activity = time(NULL);

    // If currently inactive, send active
    if (!account->csi_active)
    {
        account->connection.send(stanza::xep0352::active()
                                .build(account->context)
                                .get());
        account->csi_active = true;
        weechat_printf(account->buffer, "%sClient state: active",
                      weechat_prefix("network"));
    }

    return WEECHAT_RC_OK;
}

// Stream Management (XEP-0198) - Ack timer callback
int weechat::account::sm_ack_timer_cb(const void *pointer, void *data, int remaining_calls)
{
    (void) data;
    (void) remaining_calls;

    if (weechat::g_plugin_unloading || !weechat::plugin::instance)
        return WEECHAT_RC_OK;

    account *account = (weechat::account *)pointer;
    if (!account || !account->connection || !xmpp_conn_is_connected(account->connection))
        return WEECHAT_RC_OK;

    if (!account->sm_enabled)
        return WEECHAT_RC_OK;

    // Request acknowledgement from server
    account->connection.send(stanza::xep0198::request()
                            .build(account->context)
                            .get());

    return WEECHAT_RC_OK;
}

int weechat::account::upload_fd_cb(const void *pointer, void *data, int fd)
{
    (void) data;

    if (weechat::g_plugin_unloading || !weechat::plugin::instance)
        return WEECHAT_RC_OK;

    account *ptr_account = (account *)pointer;
    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    // Find the completion context for this fd
    auto it = ptr_account->pending_uploads.find(fd);
    if (it == ptr_account->pending_uploads.end())
        return WEECHAT_RC_ERROR;

    auto ctx = it->second;

    // Drain the pipe (1 byte signal)
    char sig[1];
    (void)::read(fd, sig, sizeof(sig));

    // Join the worker thread
    if (ctx->worker.joinable())
        ctx->worker.join();

    // Unhook and close fds
    if (ctx->hook)
        weechat_unhook(ctx->hook);
    close(fd);
    if (ctx->pipe_write_fd >= 0)
        close(ctx->pipe_write_fd);

    // Remove from map
    ptr_account->pending_uploads.erase(it);

    // Process result
    if (!ctx->success)
    {
        weechat_printf(ptr_account->buffer,
                        "%s%s: file upload failed (HTTP %ld): %s",
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                        ctx->http_code,
                        ctx->curl_error.empty() ? "unknown error" : ctx->curl_error.c_str());
        return WEECHAT_RC_OK;
    }

    weechat_printf(ptr_account->buffer, "%sFile uploaded! Sharing link…",
                  weechat_prefix("network"));

    auto channel_it = ptr_account->channels.find(ctx->channel_id);
    if (channel_it != ptr_account->channels.end())
    {
        weechat::channel::file_metadata meta;
        meta.filename     = ctx->filename;
        meta.content_type = ctx->content_type;
        meta.size         = ctx->file_size;
        meta.sha256_hash  = ctx->sha256_hash;
        meta.width        = ctx->image_width;
        meta.height       = ctx->image_height;

        channel_it->second.send_message(
            channel_it->second.id,
            ctx->get_url,
            std::optional<std::string>(ctx->get_url),
            std::optional<weechat::channel::file_metadata>(meta)
        );
    }

    return WEECHAT_RC_OK;
}
