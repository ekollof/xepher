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
        XDEBUG("Client state: inactive (idle for {} seconds)", idle_time);
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
        XDEBUG("Client state: active");
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
    if (!ctx->feed_post_upload_id.empty())
    {
        // This upload belongs to a pending feed post (embed tag).
        auto post_it = ptr_account->pending_feed_posts.find(ctx->feed_post_upload_id);
        if (post_it == ptr_account->pending_feed_posts.end())
        {
            // Stale: post was already cleaned up
            return WEECHAT_RC_OK;
        }

        auto &post = post_it->second;
        size_t idx = post.uploads_done;  // index of the just-completed embed

        if (!ctx->success)
        {
            // Upload failed — save draft, notify user, erase
            std::string draft_path = ptr_account->save_feed_draft(post);
            weechat_printf(post.buffer,
                "%s%s: embed upload failed (HTTP %ld): %s",
                weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                ctx->http_code,
                ctx->curl_error.empty() ? "unknown error" : ctx->curl_error.c_str());
            if (!draft_path.empty())
                weechat_printf(post.buffer,
                    "%sDraft saved: %s",
                    weechat_prefix("network"), draft_path.c_str());
            ptr_account->pending_feed_posts.erase(post_it);
            return WEECHAT_RC_OK;
        }

        // Fill in the embed's result fields
        if (idx < post.embeds.size())
        {
            auto &emb       = post.embeds[idx];
            emb.get_url     = ctx->get_url;
            emb.mime        = ctx->content_type;
            emb.size        = ctx->file_size;
            emb.sha256_b64  = ctx->sha256_hash;
            emb.width       = static_cast<int>(ctx->image_width);
            emb.height      = static_cast<int>(ctx->image_height);
        }
        post.uploads_done++;

        if (post.uploads_done < post.embeds.size())
        {
            // More embeds to upload — kick off the next slot request
            auto &next_emb = post.embeds[post.uploads_done];

            // Content-type for next embed
            std::string ct = "application/octet-stream";
            {
                size_t dp = next_emb.filename.find_last_of('.');
                if (dp != std::string::npos)
                {
                    std::string ext = next_emb.filename.substr(dp + 1);
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    if (ext == "jpg" || ext == "jpeg") ct = "image/jpeg";
                    else if (ext == "png")  ct = "image/png";
                    else if (ext == "gif")  ct = "image/gif";
                    else if (ext == "webp") ct = "image/webp";
                    else if (ext == "mp4")  ct = "video/mp4";
                    else if (ext == "webm") ct = "video/webm";
                    else if (ext == "pdf")  ct = "application/pdf";
                    else if (ext == "txt")  ct = "text/plain";
                }
            }
            next_emb.mime = ct;

            // Get file size
            size_t fsz = 0;
            {
                FILE *f = fopen(next_emb.filepath.c_str(), "rb");
                if (f)
                {
                    fseek(f, 0, SEEK_END);
                    long r = ftell(f);
                    fclose(f);
                    if (r > 0) fsz = static_cast<size_t>(r);
                }
            }
            next_emb.size = fsz;

            // Sanitize filename
            std::string san_name;
            for (char c : next_emb.filename)
            {
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')
                    san_name += c;
                else
                    san_name += '-';
            }
            {
                size_t p2 = 0;
                while ((p2 = san_name.find("--", p2)) != std::string::npos)
                    san_name.erase(p2, 1);
            }
            while (!san_name.empty() && san_name.front() == '-') san_name.erase(0, 1);
            while (!san_name.empty() && san_name.back() == '-') san_name.pop_back();
            if (san_name.empty()) san_name = "file";

            // Generate new slot request IQ id
            xmpp_string_guard new_id_g(ptr_account->context,
                                       xmpp_uuid_gen(ptr_account->context));
            std::string new_id = new_id_g.ptr ? new_id_g.ptr : "embed-next";

            // Register upload_request for iq_handler to pick up
            ptr_account->upload_requests[new_id] = {
                new_id,
                next_emb.filepath,
                san_name,
                "",   // channel_id unused for feed posts
                ct,
                fsz,
                ""
            };

            // Move the pending post under the new IQ id key
            xepher::pending_feed_post moved_post = std::move(post);
            ptr_account->pending_feed_posts.erase(post_it);
            ptr_account->pending_feed_posts[new_id] = std::move(moved_post);

            // Send slot request
            xmpp_stanza_t *slot_iq = xmpp_iq_new(ptr_account->context, "get",
                                                   new_id.c_str());
            xmpp_stanza_set_to(slot_iq, ptr_account->upload_service.c_str());

            xmpp_stanza_t *req_el = xmpp_stanza_new(ptr_account->context);
            xmpp_stanza_set_name(req_el, "request");
            xmpp_stanza_set_ns(req_el, "urn:xmpp:http:upload:0");
            xmpp_stanza_set_attribute(req_el, "filename", san_name.c_str());
            auto sz_str = fmt::format("{}", fsz);
            xmpp_stanza_set_attribute(req_el, "size", sz_str.c_str());
            if (!ct.empty())
                xmpp_stanza_set_attribute(req_el, "content-type", ct.c_str());
            xmpp_stanza_add_child(slot_iq, req_el);
            xmpp_stanza_release(req_el);
            ptr_account->connection.send(slot_iq);
            xmpp_stanza_release(slot_iq);

            return WEECHAT_RC_OK;
        }

        // All uploads done — build and publish the Atom entry
        xepher::pending_feed_post finished_post = std::move(post);
        ptr_account->pending_feed_posts.erase(post_it);
        weechat_printf(finished_post.buffer, "%sAll embeds uploaded, publishing post…",
                       weechat_prefix("network"));
        ptr_account->build_and_publish_post(finished_post);
        return WEECHAT_RC_OK;
    }

    // Normal (non-embed) upload result
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
