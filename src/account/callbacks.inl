int weechat::account::idle_timer_cb(const void *pointer, void *data, int remaining_calls)
{
    (void) data;
    (void) remaining_calls;

    if (weechat::g_plugin_unloading || !weechat::plugin::instance)
        return WEECHAT_RC_OK;

    account *account = (weechat::account *)pointer;
    if (!account || !account->connection || !xmpp_conn_is_connected(account->connection))
        return WEECHAT_RC_OK;

    time_t now = time(nullptr);
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

    // XEP-0319: Last User Interaction in Presence
    // When going idle, broadcast presence with <idle since='...'/>.
    // Only do this once per idle transition (not on every timer tick).
    if (idle_time > 300 && !account->xep0319_idle_sent)
    {
        // Format idle-since as ISO 8601 UTC
        struct tm *tm_idle = gmtime(&account->last_activity);
        std::string since_str = fmt::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}Z",
            tm_idle->tm_year + 1900, tm_idle->tm_mon + 1, tm_idle->tm_mday,
            tm_idle->tm_hour, tm_idle->tm_min, tm_idle->tm_sec);
        struct idle_pres_spec : stanza::spec {
            idle_pres_spec(std::string_view from_jid, std::string_view since) : spec("presence") {
                attr("from", from_jid);
                struct idle_el : stanza::spec {
                    idle_el(std::string_view s) : spec("idle") {
                        xmlns<urn::xmpp::idle::_1>();
                        attr("since", s);
                    }
                } idle_child(since);
                child(idle_child);
            }
        } idle_pres(account->jid(), since_str);
        account->connection.send(idle_pres.build(account->context).get());
        account->xep0319_idle_sent = true;
        XDEBUG("XEP-0319: sent idle presence (since={})", since_str);
    }

    // XEP-0410 / XEP-0199: Sweep stale ping queries — if a ping has not
    // received a result or error within 30 seconds, treat it as a timeout.
    // For MUC self-pings: a dropped ping is functionally equivalent to
    // <not-acceptable/> — we can no longer confirm we are still in the room.
    {
        static constexpr time_t PING_TIMEOUT_SECS = 30;
        std::vector<std::string> stale_ids;
        for (auto &[pid, start_time] : account->user_ping_queries)
        {
            if (now - start_time > PING_TIMEOUT_SECS)
                stale_ids.push_back(pid);
        }
        for (auto &pid : stale_ids)
        {
            account->user_ping_queries.erase(pid);
            weechat_printf(account->buffer,
                           "%sPing %s timed out (no response within %ld s)",
                           weechat_prefix("error"),
                           pid.c_str(),
                           PING_TIMEOUT_SECS);
        }
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
    account->last_activity = time(nullptr);

    // If currently inactive, send active
    if (!account->csi_active)
    {
        account->connection.send(stanza::xep0352::active()
                                .build(account->context)
                                .get());
        account->csi_active = true;
        XDEBUG("Client state: active");
    }

    // XEP-0319: if we previously sent idle presence, send a fresh plain
    // presence (without <idle>) to signal that the user is no longer idle.
    if (account->xep0319_idle_sent)
    {
        struct active_pres_spec : stanza::spec {
            active_pres_spec(std::string_view from_jid) : spec("presence") {
                attr("from", from_jid);
            }
        } active_pres(account->jid());
        account->connection.send(active_pres.build(account->context).get());
        account->xep0319_idle_sent = false;
        XDEBUG("XEP-0319: cleared idle presence (user active)");
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
    {
        XDEBUG("Upload: fd_cb fired but no pending upload for fd {}", fd);
        return WEECHAT_RC_ERROR;
    }

    auto& [_, ctx] = *it;
    XDEBUG("Upload: fd_cb processing upload, success={}", ctx->success);

    // Drain the pipe if worker_done is still set (not already
    // processed by the timer callback).
    if (ctx->worker_done.exchange(false))
    {
        char sig[1];
        (void)::read(fd, sig, sizeof(sig));
        if (ctx->worker.joinable())
            ctx->worker.join();
    }

    // Unhook and close fds (safe even if timer callback already ran)
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

        auto& [_, post] = *post_it;
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
                    std::ranges::transform(ext, ext.begin(), ::tolower);
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
            std::ranges::for_each(next_emb.filename, [&](char c) {
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')
                    san_name += c;
                else
                    san_name += '-';
            });
            {
                size_t p2 = 0;
                while ((p2 = san_name.find("--", p2)) != std::string::npos)
                    san_name.erase(p2, 1);
            }
            while (!san_name.empty() && san_name.front() == '-') san_name.erase(0, 1);
            while (!san_name.empty() && san_name.back() == '-') san_name.pop_back();
            if (san_name.empty()) san_name = "file";

            // Generate new slot request IQ id
            std::string new_id = stanza::uuid(ptr_account->context);

            // Register upload_request for iq_handler to pick up
            ptr_account->upload_requests[new_id] = {
                new_id,
                next_emb.filepath,
                san_name,
                "",   // channel_id unused for feed posts
                ct,
                fsz,
                "",
                false  // not a MUC for feed posts
            };

            // Move the pending post under the new IQ id key
            xepher::pending_feed_post moved_post = std::move(post);
            ptr_account->pending_feed_posts.erase(post_it);
            ptr_account->pending_feed_posts[new_id] = std::move(moved_post);

            // Send slot request (XEP-0363 HTTP Upload)
            struct slot_iq_spec : stanza::spec {
                slot_iq_spec(std::string_view id, std::string_view to,
                             std::string_view fname, std::size_t sz,
                             std::string_view ct) : spec("iq") {
                    attr("type", "get");
                    attr("id", id);
                    attr("to", to);
                    struct request_spec : stanza::spec {
                        request_spec(std::string_view fname, std::size_t sz,
                                     std::string_view ct) : spec("request") {
                            attr("xmlns", "urn:xmpp:http:upload:0");
                            attr("filename", fname);
                            attr("size", fmt::format("{}", sz));
                            if (!ct.empty()) attr("content-type", ct);
                        }
                    } req(fname, sz, ct);
                    child(req);
                }
            } slot_iq(new_id, ptr_account->upload_service, san_name, fsz, ct);
            ptr_account->connection.send(slot_iq.build(ptr_account->context).get());

            return WEECHAT_RC_OK;
        }

        // All uploads done — build and publish the Atom entry
        xepher::pending_feed_post finished_post = std::move(post);
        ptr_account->pending_feed_posts.erase(post_it);
        weechat_printf_date_tags(finished_post.buffer, 0, "no_trigger,notify_none",
                       "%sAll embeds uploaded, publishing post…",
                       weechat_prefix("network"));
        ptr_account->build_and_publish_post(finished_post);
        return WEECHAT_RC_OK;
    }

    // Normal (non-embed) upload result
    if (!ctx->success)
    {
        // Report the error in the channel buffer where the upload was initiated,
        // falling back to the account buffer if the channel is gone.
        struct t_gui_buffer *err_buf = ptr_account->buffer;
        if (auto ch_it = ptr_account->channels.find(ctx->channel_id); ch_it != ptr_account->channels.end())
        {
            auto& [_, ch] = *ch_it;
            err_buf = ch.buffer;
        }
        weechat_printf(err_buf,
                        "%s%s: file upload failed (%s)",
                        weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                        ctx->curl_error.empty() ? "unknown error" : ctx->curl_error.c_str());
        return WEECHAT_RC_OK;
    }

    // Compute the user-visible link (aesgcm://...#ivkey for ESFS/OMEMO uploads so
    // clients can fetch+decrypt the https ciphertext; plain https otherwise).
    // The data/source URI for metadata (SFS/SIMS url-data targets) must always be
    // the raw https (the actual bytes on the upload server, plain or cipher).
    // This ensures that "File uploaded" status and sent body use the correct
    // link (aesgcm for omemo uploads to self/MUC), while meta uses the https
    // for the actual data location. Prevents users seeing/pasting raw https
    // that serves cipher "garbage" for ESFS cases.
    std::string data_url = ctx->get_url;
    std::string visible_link = data_url;
    if (ctx->encrypted && !ctx->esfs_aesgcm_fragment.empty())
    {
        if (visible_link.starts_with("https://"))
            visible_link = "aesgcm://" + visible_link.substr(8);
        else if (visible_link.starts_with("http://"))
            visible_link = "aesgcm://" + visible_link.substr(7);
        visible_link += '#';
        visible_link += ctx->esfs_aesgcm_fragment;
    }

    // Build meta once (used for ch.send or fallback rich stanza).
    weechat::channel::file_metadata meta;
    meta.filename     = ctx->filename;
    meta.content_type = ctx->content_type;
    meta.size         = ctx->file_size;
    meta.sha256_hash  = ctx->sha256_hash;  // plaintext hash (always computed)
    meta.width        = ctx->image_width;
    meta.height       = ctx->image_height;

    // XEP-0448: attach encrypted file sharing info when available.
    if (ctx->encrypted)
    {
        meta.esfs = weechat::channel::file_metadata::esfs_info{
            ctx->esfs_key_b64,
            ctx->esfs_iv_b64,
            ctx->esfs_cipher_hash_b64,
        };
        // For encrypted uploads, use the original plaintext size in the <file> element.
        meta.size = ctx->original_file_size;
    }

    // Always surface a visible text link (in the originating channel buffer when
    // still present). The rich SFS/SIMS send below (when channel found) provides
    // preview metadata for clients; this printf is the fallback for MUC/closed
    // channels or when the send hits service-unavailable (as seen in logs for
    // some MUC uploads).
    struct t_gui_buffer *status_buf = ptr_account->buffer;
    bool sent_rich = false;
    if (auto channel_it = ptr_account->channels.find(ctx->channel_id); channel_it != ptr_account->channels.end())
    {
        auto& [_, ch] = *channel_it;
        status_buf = ch.buffer;

        ch.send_message(
            ch.id,
            visible_link,  // body: aesgcm (or https) for the visible link
            std::optional<std::string>(data_url),  // oob/meta sources: always the https bytes location
            std::optional<weechat::channel::file_metadata>(meta)
        );
        sent_rich = true;
    }

    if (!sent_rich)
    {
        // Fallback: always emit the rich SFS/SIMS (with esfs block if OMEMO/ESFS)
        // even if local channel entry is gone (self PM, /close race, MUC churn).
        // The stanza carries the preview metadata (size/hash/dims/media-type +
        // sources + encrypted for aesgcm clients). Echo will recreate display
        // via message_handler if the buffer is (re)opened. Use plaintext rich
        // (no OMEMO) when no ch state available — same fallback as ch path when
        // bundles not ready. Body uses visible (aesgcm#key for ESFS).
        std::string rich_id = stanza::uuid(ptr_account->context);
        const char *mtype = ctx->is_muc ? "groupchat" : "chat";
        {
            auto rich = weechat::channel::make_file_share_stanza(
                ptr_account->context, ctx->channel_id, mtype, rich_id,
                visible_link, data_url, meta);
            rich.body(visible_link);
            ptr_account->connection.send(rich.build(ptr_account->context).get());
        }
    }

    weechat_printf_date_tags(status_buf, 0, "no_trigger,notify_none",
                  "%sFile uploaded! Sharing link… %s",
                  weechat_prefix("network"), visible_link.c_str());

    return WEECHAT_RC_OK;
}
