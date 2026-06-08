bool weechat::connection::iq_handler(xmpp_stanza_t *stanza, bool top_level)
{
    // SM counter incremented in libstrophe wrapper, not here
    // top_level parameter kept for nested/recursive calls

    (void) top_level;
    append_raw_xml_trace(account, "RECV", stanza);

    xmpp_stanza_t *reply, *query, *fin;
    xmpp_stanza_t         *storage, *conference, *nick;

    auto binding = std::make_unique<xml::iq>(account.context, stanza);
    const char *id = xmpp_stanza_get_id(stanza);
    const char *from = xmpp_stanza_get_from(stanza);
    const char *to = xmpp_stanza_get_to(stanza);
    const char *type = xmpp_stanza_get_attribute(stanza, "type");
    // Keep own JID alive for the duration of this handler.
    // account.jid() returns a temporary std::string; storing .data() on it
    // immediately produces a dangling pointer.  Cache it once here.
    const std::string own_jid = account.jid();

    // XEP-0060: on publish success, re-fetch so the feed buffer updates
    // immediately without a manual /feed refresh.
    // - publish/reply: re-fetch only the single new item by ID.
    // - retract:       re-fetch the full node (the item is gone; fetching it
    //                  by ID returns nothing, so we need the current page).
    // Called at every publish-result erase site.
    auto trigger_publish_refetch = [&](const char *iq_id) {
        auto pub_it = account.pubsub_publish_ids.find(iq_id);
        if (pub_it == account.pubsub_publish_ids.end())
        {
            account.pubsub_publish_ids.erase(iq_id);
            return;
        }
        auto& [_, pub] = *pub_it;
        std::string pub_service   = pub.service;
        std::string pub_node      = pub.node;
        std::string pub_item_id   = pub.item_id;
        bool        pub_is_retract = pub.is_retract;
        account.pubsub_publish_ids.erase(pub_it);

        if (pub_service.empty() || pub_node.empty())
            return;

        std::string rf_uid = stanza::uuid(account.context);
        stanza::xep0060::items its(pub_node);
        if (!pub_is_retract && !pub_item_id.empty())
        {
            // Publish path: request only the single newly-published item.
            its.item(stanza::xep0060::item().id(pub_item_id));
        }
        // Retract path: no <item id> filter → server returns current page.
        stanza::xep0060::pubsub ps;
        ps.items(its);
        account.connection.send(stanza::iq()
            .from(account.jid())
            .to(pub_service)
            .type("get")
            .id(rf_uid)
            .xep0060()
            .pubsub(ps)
            .build(account.context)
            .get());
        account.pubsub_fetch_ids[rf_uid] = {pub_service, pub_node, "", 0};
    };
    
    if (handle_ping_iq_event(stanza, own_jid))
        return true;

    // XEP-0441: MAM Preferences — handle <prefs xmlns='urn:xmpp:mam:2'> result/error
    if (id && account.mam_prefs_queries.contains(id))
    {
        struct t_gui_buffer *prefs_buf = account.mam_prefs_queries[id];
        if (!prefs_buf) prefs_buf = account.buffer;
        account.mam_prefs_queries.erase(id);

        xmpp_stanza_t *prefs_el = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "prefs", "urn:xmpp:mam:2");

        if (type && weechat_strcasecmp(type, "error") == 0)
        {
            xmpp_stanza_t *err = xmpp_stanza_get_child_by_name(stanza, "error");
            const char *err_type = err ? xmpp_stanza_get_attribute(err, "type") : "unknown";
            weechat_printf(prefs_buf,
                "%sMAM preferences: server returned error (%s) — feature may not be supported",
                weechat_prefix("error"), err_type);
        }
        else if (prefs_el)
        {
            const char *def = xmpp_stanza_get_attribute(prefs_el, "default");
            weechat_printf(prefs_buf,
                "%sMAM preferences: default=%s%s%s",
                weechat_prefix("network"),
                weechat_color("bold"), def ? def : "(unset)", weechat_color("-bold"));

            // Always list
            xmpp_stanza_t *always_el = xmpp_stanza_get_child_by_name(prefs_el, "always");
            if (always_el)
            {
                std::string jids_str;
                for (xmpp_stanza_t *jid_el = xmpp_stanza_get_children(always_el);
                     jid_el; jid_el = xmpp_stanza_get_next(jid_el))
                {
                    const char *jn = xmpp_stanza_get_name(jid_el);
                    if (!jn || std::string_view(jn) != "jid") continue;
                    const std::string jid_txt = stanza_element_text(jid_el);
                    if (!jid_txt.empty())
                    {
                        if (!jids_str.empty()) jids_str += ", ";
                        jids_str += jid_txt;
                    }
                }
                weechat_printf(prefs_buf, "%s  always: %s",
                    weechat_prefix("network"),
                    jids_str.empty() ? "(empty)" : jids_str.c_str());
            }

            // Never list
            xmpp_stanza_t *never_el = xmpp_stanza_get_child_by_name(prefs_el, "never");
            if (never_el)
            {
                std::string jids_str;
                for (xmpp_stanza_t *jid_el = xmpp_stanza_get_children(never_el);
                     jid_el; jid_el = xmpp_stanza_get_next(jid_el))
                {
                    const char *jn = xmpp_stanza_get_name(jid_el);
                    if (!jn || std::string_view(jn) != "jid") continue;
                    const std::string jid_txt = stanza_element_text(jid_el);
                    if (!jid_txt.empty())
                    {
                        if (!jids_str.empty()) jids_str += ", ";
                        jids_str += jid_txt;
                    }
                }
                weechat_printf(prefs_buf, "%s  never:  %s",
                    weechat_prefix("network"),
                    jids_str.empty() ? "(empty)" : jids_str.c_str());
            }

            weechat_printf(prefs_buf, "%sMAM preferences updated successfully",
                weechat_prefix("network"));
        }
        return true;
    }

    // Handle vCard responses (XEP-0054)
    xmpp_stanza_t *vcard = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "vCard", "vcard-temp");
    if (vcard && type && weechat_strcasecmp(type, "result") == 0)
    {
        const char *from_jid = from ? from : own_jid.c_str();

        // Check if this is a /setvcard read-merge response (self-fetch before update).
        if (id)
        {
            if (auto sv_it = account.setvcard_queries.find(id); sv_it != account.setvcard_queries.end())
            {
                auto& [_, sv] = *sv_it;
                struct t_gui_buffer *sv_buf = sv.buffer;

                // Build a vcard_fields struct pre-populated from the server's vCard.
                auto ctext = [&](xmpp_stanza_t *parent, const char *name) -> std::string {
                    xmpp_stanza_t *ch = xmpp_stanza_get_child_by_name(parent, name);
                    return ch ? stanza_element_text(ch) : std::string {};
                };
                ::xmpp::xep0054::vcard_fields f;
                f.fn       = ctext(vcard, "FN");
                f.nickname = ctext(vcard, "NICKNAME");
                f.url      = ctext(vcard, "URL");
                f.desc     = ctext(vcard, "DESC");
                f.bday     = ctext(vcard, "BDAY");
                f.note     = ctext(vcard, "NOTE");
                f.title    = ctext(vcard, "TITLE");
                {
                    xmpp_stanza_t *org_el = xmpp_stanza_get_child_by_name(vcard, "ORG");
                    if (org_el) f.org = ctext(org_el, "ORGNAME");
                }
                {
                    xmpp_stanza_t *email_el = xmpp_stanza_get_child_by_name(vcard, "EMAIL");
                    if (email_el) f.email = ctext(email_el, "USERID");
                }
                {
                    xmpp_stanza_t *tel_el = xmpp_stanza_get_child_by_name(vcard, "TEL");
                    if (tel_el) f.tel = ctext(tel_el, "NUMBER");
                }

                // Apply the requested override.
                const std::string &fld = sv.field;
                const std::string &val = sv.value;
                if      (fld == "fn")       f.fn       = val;
                else if (fld == "nickname") f.nickname = val;
                else if (fld == "email")    f.email    = val;
                else if (fld == "url")      f.url      = val;
                else if (fld == "desc")     f.desc     = val;
                else if (fld == "org")      f.org      = val;
                else if (fld == "title")    f.title    = val;
                else if (fld == "tel")      f.tel      = val;
                else if (fld == "bday")     f.bday     = val;
                else if (fld == "note")     f.note     = val;

                // Publish the merged vCard.
                xmpp_stanza_t *set_iq = ::xmpp::xep0054::vcard_set(account.context, f);
                account.connection.send(set_iq);
                xmpp_stanza_release(set_iq);
                weechat_printf(sv_buf, "%svCard field %s updated",
                               weechat_prefix("network"), fld.c_str());

                account.setvcard_queries.erase(sv_it);
                return true;
            }
        }

        // Determine which buffer to print into: the one that issued /whois, or
        // the account buffer for auto-fetched vCards (XEP-0153 trigger).
        struct t_gui_buffer *target_buf = account.buffer;
        bool is_whois = false;
        if (id)
        {
            if (auto it = account.whois_queries.find(id); it != account.whois_queries.end())
            {
                auto& [_, w] = *it;
                target_buf = w.buffer;
                account.whois_queries.erase(it);
                is_whois = true;
            }
        }

        // Helper: get direct text content of a child element
        auto child_text = [&](xmpp_stanza_t *parent, const char *name) -> std::string {
            xmpp_stanza_t *child = xmpp_stanza_get_child_by_name(parent, name);
            return child ? stanza_element_text(child) : std::string {};
        };

        // Helper: print a labelled line only if value is non-empty
        auto print_field = [&](const char *label, const std::string &val) {
            if (!val.empty())
                weechat_printf(target_buf, "  %s%s%s %s",
                               weechat_color("bold"), label,
                               weechat_color("reset"), val.c_str());
        };

        if (is_whois)
        {
            weechat_printf(target_buf, "%svCard for %s:",
                           weechat_prefix("network"), from_jid);
        }
        else
        {
            XDEBUG("vCard auto-fetched for {}", from_jid);
        }

        std::string fn       = child_text(vcard, "FN");
        std::string nickname = child_text(vcard, "NICKNAME");
        std::string url      = child_text(vcard, "URL");
        std::string desc     = child_text(vcard, "DESC");
        std::string bday     = child_text(vcard, "BDAY");
        std::string note     = child_text(vcard, "NOTE");
        std::string jabbid   = child_text(vcard, "JABBERID");
        std::string title    = child_text(vcard, "TITLE");
        std::string role_vc  = child_text(vcard, "ROLE");

        // ORG: <ORG><ORGNAME>…</ORGNAME></ORG>
        std::string org;
        xmpp_stanza_t *org_el = xmpp_stanza_get_child_by_name(vcard, "ORG");
        if (org_el) org = child_text(org_el, "ORGNAME");

        // EMAIL: <EMAIL><USERID>…</USERID></EMAIL>  (first occurrence)
        std::string email_val;
        xmpp_stanza_t *email_el = xmpp_stanza_get_child_by_name(vcard, "EMAIL");
        if (email_el) email_val = child_text(email_el, "USERID");

        // TEL: <TEL><NUMBER>…</NUMBER></TEL>  (first occurrence)
        std::string tel;
        xmpp_stanza_t *tel_el = xmpp_stanza_get_child_by_name(vcard, "TEL");
        if (tel_el) tel = child_text(tel_el, "NUMBER");

        // ADR: <ADR><STREET>…</STREET><LOCALITY>…</LOCALITY><CTRY>…</CTRY></ADR>
        std::string adr;
        xmpp_stanza_t *adr_el = xmpp_stanza_get_child_by_name(vcard, "ADR");
        if (adr_el)
        {
            for (const char *part : {"STREET", "LOCALITY", "REGION", "PCODE", "CTRY"})
            {
                std::string p = child_text(adr_el, part);
                if (!p.empty())
                {
                    if (!adr.empty()) adr += ", ";
                    adr += p;
                }
            }
        }

        if (is_whois)
        {
            print_field("Full name:",    fn);
            print_field("Nickname:",     nickname);
            print_field("Birthday:",     bday);
            print_field("Organisation:", org);
            print_field("Title:",        title);
            print_field("Role:",         role_vc);
            print_field("Email:",        email_val);
            print_field("Phone:",        tel);
            print_field("Address:",      adr);
            print_field("URL:",          url);
            print_field("JID:",          jabbid);
            print_field("Note:",         note);
            print_field("Description:",  desc);
        }

        // Store into user profile for future reference
        weechat::user *u = weechat::user::search(&account, from_jid);
        if (u)
        {
            if (!fn.empty())       u->profile.fn        = fn;
            if (!nickname.empty()) u->profile.nickname  = nickname;
            if (!email_val.empty()) u->profile.email    = email_val;
            if (!url.empty())      u->profile.url       = url;
            if (!desc.empty())     u->profile.description = desc;
            if (!org.empty())      u->profile.org       = org;
            if (!title.empty())    u->profile.title     = title;
            if (!tel.empty())      u->profile.tel       = tel;
            if (!bday.empty())     u->profile.bday      = bday;
            if (!note.empty())     u->profile.note      = note;
            if (!jabbid.empty())   u->profile.jabberid  = jabbid;
            u->profile.vcard_fetched = true;
        }

        return true;
    }

    // Handle vCard4 PubSub responses (XEP-0292)
    // Arrives as: <iq type='result'><pubsub xmlns='..pubsub'><items node='urn:xmpp:vcard4'>
    //               <item id='current'><vcard xmlns='urn:ietf:params:xml:ns:vcard-4.0'>…</vcard>
    {
        xmpp_stanza_t *pubsub_vc4 = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "pubsub", "http://jabber.org/protocol/pubsub");
        if (pubsub_vc4 && type && weechat_strcasecmp(type, "result") == 0)
        {
            xmpp_stanza_t *items = xmpp_stanza_get_child_by_name(pubsub_vc4, "items");
            if (items)
            {
                const char *node = xmpp_stanza_get_attribute(items, "node");
                if (node && std::string_view(node) == NS_VCARD4_PUBSUB)
                {
                    const char *from_jid = from ? from : own_jid.c_str();

                    if (id)
                        account.whois_queries.erase(id);

                    xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(items, "item");
                    if (item)
                    {
                        xmpp_stanza_t *vcard4 = xmpp_stanza_get_child_by_name_and_ns(
                            item, "vcard", NS_VCARD4);
                        if (vcard4)
                        {
                            XDEBUG("vCard4 auto-fetched for {}", from_jid);
                            return true;
                        }
                    }
                }
            }
        }
    }

    if (handle_avatar_pubsub_iq_event(stanza, own_jid))
        return true;

    if (handle_pubsub_feed_iq_event(stanza))
        return true;

    // XEP-0363: HTTP File Upload - handle upload slot response
    xmpp_stanza_t *slot = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "slot", "urn:xmpp:http:upload:0");
    
    if (slot && id && type && weechat_strcasecmp(type, "result") == 0)
    {
        XDEBUG("Upload slot response received (id: {})", id);
        
        if (auto req_it = account.upload_requests.find(id); req_it != account.upload_requests.end())
        {
            auto& [_, req] = *req_it;
            XDEBUG("Found matching upload request");
            // Extract PUT and GET URLs
            xmpp_stanza_t *put_elem = xmpp_stanza_get_child_by_name(slot, "put");
            xmpp_stanza_t *get_elem = xmpp_stanza_get_child_by_name(slot, "get");
            
            const char *put_url = put_elem ? xmpp_stanza_get_attribute(put_elem, "url") : nullptr;
            const char *get_url = get_elem ? xmpp_stanza_get_attribute(get_elem, "url") : nullptr;
            
            XDEBUG("PUT URL: {}", put_url ? put_url : "(null)");
            XDEBUG("GET URL: {}", get_url ? get_url : "(null)");
            
            // Extract PUT headers (XEP-0363 §9.2: only Authorization, Cookie, Expires
            // are permitted — forwarding arbitrary headers is a header-injection risk).
            // Also strip any CR/LF from values to prevent HTTP header injection.
            static constexpr std::string_view allowed_headers[] = {
                "authorization", "cookie", "expires"
            };
            std::vector<std::string> put_headers;
            if (put_elem)
            {
                xmpp_stanza_t *header = xmpp_stanza_get_child_by_name(put_elem, "header");
                while (header)
                {
                    const char *name = xmpp_stanza_get_attribute(header, "name");
                    const std::string value = stanza_element_text(header);
                    if (name)
                    {
                        // Only forward headers explicitly listed in §9.2
                        bool allowed = std::ranges::any_of(allowed_headers, [&](const auto& allowed_name) {
                            return weechat_strcasecmp(name, allowed_name.data()) == 0;
                        });
                        if (allowed)
                        {
                            // Strip CR and LF to prevent HTTP header injection
                            std::string safe_value = value;
                            std::erase_if(safe_value, [](char c) { return c == '\r' || c == '\n'; });
                            std::string header_str = fmt::format("{}: {}", name, safe_value);
                            put_headers.push_back(header_str);
                            XDEBUG("PUT header: {}", header_str);
                        }
                        else
                        {
                            XDEBUG("PUT header '{}' not in XEP-0363 §9.2 allowlist — dropped", name);
                        }
                    }
                    header = xmpp_stanza_get_next(header);
                }
            }
            
            if (put_url && get_url)
            {
                weechat_printf_date_tags(account.buffer, 0, "no_trigger,notify_none",
                              "%sUpload slot received, uploading file...",
                              weechat_prefix("network"));
                
                // Verify file exists (non-blocking, no FILE* needed)
                if (access(req.filepath.c_str(), R_OK) != 0)
                {
                    XDEBUG("Upload: file not found/readable: {}", req.filepath);
                    weechat_printf(account.buffer, "%s%s: failed to open file for upload: %s",
                                  weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                                  req.filepath.c_str());
                    account.upload_requests.erase(req_it);
                    return 1;
                }
                XDEBUG("Upload: file verified, filename='{}'", req.filename);
                
                // Get Content-Type from filename extension
                std::string filename = req.filename;
                std::string content_type = "application/octet-stream";
                size_t dot_pos = filename.find_last_of('.');
                if (dot_pos != std::string::npos)
                {
                    std::string ext = filename.substr(dot_pos + 1);
                    std::ranges::transform(ext, ext.begin(), ::tolower);
                    
                    if (ext == "jpg" || ext == "jpeg") content_type = "image/jpeg";
                    else if (ext == "png") content_type = "image/png";
                    else if (ext == "gif") content_type = "image/gif";
                    else if (ext == "webp") content_type = "image/webp";
                    else if (ext == "mp4") content_type = "video/mp4";
                    else if (ext == "webm") content_type = "video/webm";
                    else if (ext == "pdf") content_type = "application/pdf";
                    else if (ext == "txt") content_type = "text/plain";
                }
                XDEBUG("Upload: content_type={} starting async PUT", content_type);
                
                // Async HTTP PUT upload via pipe + worker thread.
                // The worker thread does all blocking I/O (file read, SHA-256,
                // curl PUT) and writes 1 byte to the pipe write-end when done.
                // weechat_hook_fd fires on the read-end in the main thread,
                // which processes the result and sends the XMPP message.

                int pipe_fds[2];
                if (pipe(pipe_fds) != 0)
                {
                    XDEBUG("Upload: pipe() failed");
                    weechat_printf(account.buffer, "%s%s: failed to create pipe for upload",
                                  weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                    account.upload_requests.erase(req_it);
                    return 1;
                }

                // Build the completion context (everything the callback needs)
                auto ctx = std::make_shared<weechat::account::upload_completion>();
                ctx->channel_id    = req.channel_id;
                ctx->is_muc        = req.is_muc;
                ctx->filename      = req.filename;
                ctx->local_path    = req.filepath;
                ctx->content_type  = content_type;
                ctx->pipe_write_fd = pipe_fds[1];

                // XEP-0448: If the destination channel has OMEMO active, encrypt the file
                // before uploading so it arrives as an Encrypted File Share stanza.
                {
                    if (auto ch_it = account.channels.find(ctx->channel_id); ch_it != account.channels.end())
                    {
                        auto& [_, ch] = *ch_it;
                        if (ch.omemo.enabled)
                            ctx->encrypted = true;
                    }
                }

                // If this upload was triggered by an embed tag in a pending feed post,
                // mark the context so upload_fd_cb routes to the feed-post path.
                if (account.pending_feed_posts.contains(id))
                    ctx->feed_post_upload_id = id;

                // Copy strings that will be used by the worker thread (the
                // upload_requests entry will be erased below, so we must copy
                // before erasing).
                std::string filepath_copy  = req.filepath;
                std::string put_url_copy   = put_url;
                std::string get_url_copy   = get_url;

                // Erase the upload_requests entry now (before thread starts)
                account.upload_requests.erase(req_it);

                // Register WeeChat hook on the read-end fd
                ctx->hook = weechat_hook_fd(pipe_fds[0], 1, 0, 0,
                                            &weechat::account::upload_fd_cb,
                                            &account, nullptr);

                // Store in pending_uploads keyed by read-end fd
                account.pending_uploads[pipe_fds[0]] = ctx;

                XDEBUG("Upload: launching worker thread for {} (read fd={})", filepath_copy, pipe_fds[0]);

                // Capture everything needed for the thread by value
                std::shared_ptr<weechat::account::upload_completion> ctx_copy = ctx;
                std::vector<std::string> put_headers_copy = put_headers;
                std::string content_type_copy = content_type;

                ctx->worker = std::thread([ctx_copy, filepath_copy,
                                           put_url_copy, get_url_copy,
                                           put_headers_copy, content_type_copy,
                                           ctx_ptr = static_cast<xmpp_ctx_t*>(
                                               account.context),
                                           conn_ptr = &account.connection]()
                {
                    // No XDEBUG here: this runs in the upload worker thread. XDEBUG
                    // (and xmpp_debug_is_on) call WeeChat APIs which are not safe
                    // from worker threads and can cause lockups/deadlocks (esp. with
                    // Python hooks or when debug is enabled for upload tracing).
                    auto &c = *ctx_copy;

                    // Helper: base64-encode a byte buffer without raw BIO pointers.
                    auto base64_encode_bytes = [](std::span<const std::uint8_t> data)
                        -> std::string {
                        if (data.empty()) return {};
                        const int encoded_size =
                            4 * static_cast<int>((data.size() + 2) / 3) + 1;
                        std::string encoded(
                            static_cast<std::size_t>(encoded_size), '\0');
                        const int written = weechat_string_base_encode(
                            "64",
                            reinterpret_cast<const char *>(data.data()),
                            static_cast<int>(data.size()),
                            encoded.data());
                        if (written <= 0)
                            return {};
                        encoded.resize(
                            static_cast<std::size_t>(written));
                        return encoded;
                    };

                    // Open file with RAII guard
                    auto file_deleter = [](FILE *f) { if (f) fclose(f); };
                    std::unique_ptr<FILE, decltype(file_deleter)>
                        upload_file_guard(fopen(filepath_copy.c_str(), "rb"), file_deleter);
                    FILE *upload_file = upload_file_guard.get();
                    if (!upload_file)
                    {
                        c.success   = false;
                        c.curl_error = "failed to open file";
                        ::write(c.pipe_write_fd, "x", 1);
                        return;
                    }

                    // Get file size — check fseek/ftell for non-seekable streams
                    if (fseek(upload_file, 0, SEEK_END) != 0)
                    {
                        c.success    = false;
                        c.curl_error = "failed to seek to end of file";
                        ::write(c.pipe_write_fd, "x", 1);
                        return;
                    }
                    long file_size = ftell(upload_file);
                    if (file_size < 0)
                    {
                        c.success    = false;
                        c.curl_error = "failed to determine file size";
                        ::write(c.pipe_write_fd, "x", 1);
                        return;
                    }
                    c.file_size = static_cast<size_t>(file_size);

                    // XEP-0446 <date>: capture file modification time (UTC ISO-8601).
                    {
                        struct stat st;
                        if (stat(filepath_copy.c_str(), &st) == 0)
                            c.file_date = fmt::format(
                                "{:%Y-%m-%dT%H:%M:%SZ}", fmt::gmtime(st.st_mtime));
                    }

                    // Rewind so subsequent operations (hash, dims, upload read) use
                    // the exact same open file descriptor/content as the size we just
                    // measured. This guarantees the SHA-256 we advertise matches the
                    // bytes we actually PUT, even if the path on disk is replaced or
                    // deleted after we opened it. Also keeps the guard alive for the
                    // final upload read (no close/reopen for the plain case).
                    if (fseek(upload_file, 0, SEEK_SET) != 0)
                    {
                        c.success    = false;
                        c.curl_error = "failed to rewind file after measuring size";
                        ::write(c.pipe_write_fd, "x", 1);
                        return;
                    }

                    // XEP-0300 §4: compute multiple hashes in one pass for agility.
                    // SHA-256 + SHA-512 are universally supported (OpenSSL and LibreSSL).
                    unsigned char hash[EVP_MAX_MD_SIZE];
                    unsigned int  hash_len = 0;
                    {
                        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>
                            sha256_ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
                        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>
                            sha512_ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
                        EVP_DigestInit_ex(sha256_ctx.get(), EVP_sha256(), nullptr);
                        EVP_DigestInit_ex(sha512_ctx.get(), EVP_sha512(), nullptr);
                        unsigned char buf[8192];
                        size_t bytes_read;
                        do {
                            bytes_read = fread(buf, 1, sizeof(buf), upload_file);
                            if (bytes_read > 0) {
                                EVP_DigestUpdate(sha256_ctx.get(), buf, bytes_read);
                                EVP_DigestUpdate(sha512_ctx.get(), buf, bytes_read);
                            }
                        } while (!feof(upload_file) && !ferror(upload_file));

                        // SHA-256
                        EVP_DigestFinal_ex(sha256_ctx.get(), hash, &hash_len);
                        c.hashes.push_back(
                            {"sha-256",
                             base64_encode_bytes(std::span<const std::uint8_t>(
                                 hash, hash_len))});

                        // SHA-512
                        EVP_DigestFinal_ex(sha512_ctx.get(), hash, &hash_len);
                        c.hashes.push_back(
                            {"sha-512",
                             base64_encode_bytes(std::span<const std::uint8_t>(
                                 hash, hash_len))});
                    } // contexts freed here
                    // Note: upload_file is now at EOF; we will rewind before giving
                    // it to curl (plain case) or using it for encrypt pt read.

                    // Parse image dimensions from file header (JPEG / PNG).
                    // We only need the first few hundred bytes — open a short read.
                    {
                        std::unique_ptr<FILE, decltype(file_deleter)>
                            dim_file_guard(fopen(filepath_copy.c_str(), "rb"), file_deleter);
                        FILE *dim_file = dim_file_guard.get();
                        if (dim_file)
                        {
                            unsigned char hdr[24] = {};
                            size_t hdr_read = fread(hdr, 1, sizeof(hdr), dim_file);
                            if (hdr_read >= 8
                                && hdr[0] == 0x89 && hdr[1] == 'P' && hdr[2] == 'N'
                                && hdr[3] == 'G'  && hdr[4] == 0x0D&& hdr[5] == 0x0A
                                && hdr[6] == 0x1A && hdr[7] == 0x0A)
                            {
                                // PNG: IHDR chunk starts at byte 8 (4-byte length + "IHDR")
                                // Width at [16..19], Height at [20..23], big-endian.
                                if (hdr_read >= 24)
                                {
                                    c.image_width  = (static_cast<size_t>(hdr[16]) << 24)
                                                   | (static_cast<size_t>(hdr[17]) << 16)
                                                   | (static_cast<size_t>(hdr[18]) <<  8)
                                                   |  static_cast<size_t>(hdr[19]);
                                    c.image_height = (static_cast<size_t>(hdr[20]) << 24)
                                                   | (static_cast<size_t>(hdr[21]) << 16)
                                                   | (static_cast<size_t>(hdr[22]) <<  8)
                                                   |  static_cast<size_t>(hdr[23]);
                                }
                            }
                            else if (hdr_read >= 3
                                     && hdr[0] == 0xFF && hdr[1] == 0xD8 && hdr[2] == 0xFF)
                            {
                                // JPEG: scan for SOF0/SOF1/SOF2 markers (0xFF 0xC0-0xC2)
                                // which contain height and width.
                                unsigned char buf2[65536];
                                fseek(dim_file, 2, SEEK_SET);
                                size_t total = fread(buf2, 1, sizeof(buf2), dim_file);
                                for (size_t i = 0; i + 8 < total; ++i)
                                {
                                    if (buf2[i] == 0xFF
                                        && (buf2[i+1] == 0xC0
                                            || buf2[i+1] == 0xC1
                                            || buf2[i+1] == 0xC2))
                                    {
                                        // SOF marker: FF Cx LL LL PP HH HH WW WW
                                        // Height at [i+5..i+6], Width at [i+7..i+8]
                                        if (i + 8 < total)
                                        {
                                            c.image_height = (static_cast<size_t>(buf2[i+5]) << 8)
                                                            |  static_cast<size_t>(buf2[i+6]);
                                            c.image_width  = (static_cast<size_t>(buf2[i+7]) << 8)
                                            |  static_cast<size_t>(buf2[i+8]);
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                    } // dim_file closed here

                    // XEP-0448: AES-256-GCM encryption for OMEMO-enabled channels.
                    // When active: encrypt the plaintext file into a tmpfile, then
                    // upload the ciphertext (+ 16-byte GCM auth tag) in its place.
                    // The key, IV, and ciphertext hash are stored in the context for
                    // the callback to embed in the <encrypted xmlns='urn:xmpp:esfs:0'> stanza.
                    std::string esfs_tmpfile_path; // track tmp path for deletion
                    if (c.encrypted)
                    {
                        c.original_file_size = static_cast<size_t>(file_size);

                        // Generate 256-bit key and 96-bit (12-byte) IV.
                        unsigned char aes_key[32] = {};
                        unsigned char aes_iv[12]  = {};
                        if (RAND_bytes(aes_key, sizeof(aes_key)) != 1
                            || RAND_bytes(aes_iv, sizeof(aes_iv)) != 1)
                        {
                            c.success    = false;
                            c.curl_error = "esfs: RAND_bytes failed for key/IV generation";
                            ::write(c.pipe_write_fd, "x", 1);
                            return;
                        }

                        // Base64-encode key and IV (reuses worker-thread helper).
                        c.esfs_key_b64 = base64_encode_bytes(aes_key);
                        c.esfs_iv_b64  = base64_encode_bytes(aes_iv);

                        // Build hex(iv_bytes) + hex(key_bytes) for aesgcm:// URL fragment.
                        // Format: 24 hex chars (12-byte IV) + 64 hex chars (32-byte key).
                        {
                            static constexpr char hex_chars[] = "0123456789abcdef";
                            std::string frag;
                            frag.reserve(24 + 64);
                            for (int i = 0; i < 12; ++i) {
                                frag += hex_chars[(aes_iv[i] >> 4) & 0xf];
                                frag += hex_chars[ aes_iv[i]       & 0xf];
                            }
                            for (int i = 0; i < 32; ++i) {
                                frag += hex_chars[(aes_key[i] >> 4) & 0xf];
                                frag += hex_chars[ aes_key[i]       & 0xf];
                            }
                            c.esfs_aesgcm_fragment = std::move(frag);
                        }

                        // Open plaintext source.
                        auto file_deleter_enc = [](FILE *f) { if (f) fclose(f); };
                        std::unique_ptr<FILE, decltype(file_deleter_enc)>
                            pt_guard(fopen(filepath_copy.c_str(), "rb"), file_deleter_enc);
                        if (!pt_guard)
                        {
                            c.success    = false;
                            c.curl_error = "esfs: failed to open plaintext for encryption";
                            ::write(c.pipe_write_fd, "x", 1);
                            return;
                        }

                        // Create a secure temporary file for ciphertext.
                        char tmp_tmpl[] = "/tmp/xepher-esfs-XXXXXX";
                        int tmp_fd = mkstemp(tmp_tmpl);
                        if (tmp_fd < 0)
                        {
                            c.success    = false;
                            c.curl_error = "esfs: mkstemp failed";
                            ::write(c.pipe_write_fd, "x", 1);
                            return;
                        }
                        esfs_tmpfile_path = tmp_tmpl;

                        // Wrap the raw fd in a FILE* (RAII via unique_ptr).
                        std::unique_ptr<FILE, decltype(file_deleter_enc)>
                            ct_guard(fdopen(tmp_fd, "wb"), file_deleter_enc);
                        if (!ct_guard)
                        {
                            close(tmp_fd);
                            ::unlink(esfs_tmpfile_path.c_str());
                            c.success    = false;
                            c.curl_error = "esfs: fdopen failed on tmpfile";
                            ::write(c.pipe_write_fd, "x", 1);
                            return;
                        }

                        // AES-256-GCM encryption using OpenSSL EVP.
                        std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>
                            enc_ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
                        if (!enc_ctx
                            || EVP_EncryptInit_ex(enc_ctx.get(), EVP_aes_256_gcm(),
                                                  nullptr, nullptr, nullptr) != 1
                            || EVP_CIPHER_CTX_ctrl(enc_ctx.get(), EVP_CTRL_GCM_SET_IVLEN,
                                                   12, nullptr) != 1
                            || EVP_EncryptInit_ex(enc_ctx.get(), nullptr,
                                                  nullptr, aes_key, aes_iv) != 1)
                        {
                            ::unlink(esfs_tmpfile_path.c_str());
                            c.success    = false;
                            c.curl_error = "esfs: EVP init failed";
                            ::write(c.pipe_write_fd, "x", 1);
                            return;
                        }

                        // Initialize SHA-256 over ciphertext for later hash field.
                        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>
                            ct_sha_ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
                        EVP_DigestInit_ex(ct_sha_ctx.get(), EVP_sha256(), nullptr);

                        unsigned char in_buf[8192];
                        unsigned char out_buf[8192 + 16]; // EVP may expand by block size
                        long ciphertext_len = 0;
                        bool enc_ok = true;
                        while (!feof(pt_guard.get()) && !ferror(pt_guard.get()))
                        {
                            size_t n = fread(in_buf, 1, sizeof(in_buf), pt_guard.get());
                            if (n == 0) break;
                            int out_len = 0;
                            if (EVP_EncryptUpdate(enc_ctx.get(), out_buf, &out_len,
                                                  in_buf, static_cast<int>(n)) != 1)
                            {
                                enc_ok = false;
                                break;
                            }
                            if (out_len > 0)
                            {
                                EVP_DigestUpdate(ct_sha_ctx.get(), out_buf,
                                                 static_cast<size_t>(out_len));
                                fwrite(out_buf, 1, static_cast<size_t>(out_len), ct_guard.get());
                                ciphertext_len += out_len;
                            }
                        }
                        if (!enc_ok)
                        {
                            ::unlink(esfs_tmpfile_path.c_str());
                            c.success    = false;
                            c.curl_error = "esfs: EVP_EncryptUpdate failed";
                            ::write(c.pipe_write_fd, "x", 1);
                            return;
                        }

                        // Finalize encryption (no output for GCM).
                        int final_len = 0;
                        EVP_EncryptFinal_ex(enc_ctx.get(), out_buf, &final_len);
                        if (final_len > 0)
                        {
                            EVP_DigestUpdate(ct_sha_ctx.get(), out_buf,
                                             static_cast<size_t>(final_len));
                            fwrite(out_buf, 1, static_cast<size_t>(final_len), ct_guard.get());
                            ciphertext_len += final_len;
                        }

                        // Append 16-byte GCM authentication tag.
                        unsigned char tag[16] = {};
                        EVP_CIPHER_CTX_ctrl(enc_ctx.get(), EVP_CTRL_GCM_GET_TAG, 16, tag);
                        EVP_DigestUpdate(ct_sha_ctx.get(), tag, sizeof(tag));
                        fwrite(tag, 1, sizeof(tag), ct_guard.get());
                        ciphertext_len += 16;

                        // Hash of ciphertext (including tag).
                        unsigned char ct_hash[EVP_MAX_MD_SIZE];
                        unsigned int  ct_hash_len = 0;
                        EVP_DigestFinal_ex(ct_sha_ctx.get(), ct_hash, &ct_hash_len);
                        c.esfs_cipher_hash_b64 = base64_encode_bytes(
                            std::span<const std::uint8_t>(ct_hash, ct_hash_len));

                        // Flush + close tmp write handle; fopen again for reading.
                        ct_guard.reset(); // closes fdopen'd FILE* (which closes tmp_fd)

                        file_size = ciphertext_len;
                        // Replace upload_file_guard with a read handle on the tmpfile.
                        upload_file_guard.reset(fopen(esfs_tmpfile_path.c_str(), "rb"));
                        upload_file = upload_file_guard.get();
                        if (!upload_file)
                        {
                            ::unlink(esfs_tmpfile_path.c_str());
                            c.success    = false;
                            c.curl_error = "esfs: failed to reopen tmpfile for upload";
                            ::write(c.pipe_write_fd, "x", 1);
                            return;
                        }
                        // Update file_size in context for <file> metadata (ciphertext size).
                        c.file_size = static_cast<size_t>(file_size);
                    }
                    else
                    {
                        // Rewind the handle we used for size+hash (now at EOF) so
                        // curl reads the same bytes we hashed. The guard from the
                        // initial open is still alive and will close it after the
                        // PUT.
                        if (fseek(upload_file, 0, SEEK_SET) != 0)
                        {
                            c.success    = false;
                            c.curl_error = "failed to rewind file for upload";
                            ::write(c.pipe_write_fd, "x", 1);
                            return;
                        }
                        // upload_file and guard are already set from the size open;
                        // no new fopen needed.
                    }

                    // Initialize curl
                    CURL *curl = curl_easy_init();
                    if (!curl)
                    {
                        // upload_file_guard will close the file on return
                        c.success    = false;
                        c.curl_error = "failed to initialize curl";
                        ::write(c.pipe_write_fd, "x", 1);
                        return;
                    }

                    // Required for multithreaded use: suppress SIGALRM/SIGPIPE
                    // delivery from within libcurl (e.g. OpenSSL alarm-based
                    // DNS timeouts, or SIGPIPE on broken connections).
                    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

                    curl_easy_setopt(curl, CURLOPT_URL, put_url_copy.c_str());
                    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
                    curl_easy_setopt(curl, CURLOPT_READDATA, upload_file);
                    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,
                                     static_cast<curl_off_t>(file_size));

                    // Discard the response body (e.g. HTML error pages from
                    // 4xx/5xx responses) — we only care about the HTTP status code.
                    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                        +[](char *, size_t size, size_t nmemb, void *) -> size_t {
                            return size * nmemb;
                        });

                    struct curl_slist *headers = nullptr;
                    headers = curl_slist_append(
                        headers,
                        fmt::format("Content-Type: {}", content_type_copy).c_str());
                    std::ranges::for_each(put_headers_copy, [&](const auto &hdr) {
                        headers = curl_slist_append(headers, hdr.c_str());
                    });
                    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

                    CURLcode res = curl_easy_perform(curl);

                    long http_code = 0;
                    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

                    curl_slist_free_all(headers);
                    curl_easy_cleanup(curl);
                    // upload_file_guard closes the file on scope exit; no manual fclose needed

                    // XEP-0448: remove temporary ciphertext file after upload.
                    if (!esfs_tmpfile_path.empty())
                        ::unlink(esfs_tmpfile_path.c_str());

                    // Remove source snapshot temp (created in command__upload at the
                    // exact moment the user invoked /upload). This guarantees the
                    // bytes we hashed, sized, and PUT are the ones from the selected
                    // file, regardless of later changes to the original path.
                    if (filepath_copy.rfind("/tmp/xepher-upload-", 0) == 0)
                        ::unlink(filepath_copy.c_str());

                    c.http_code = http_code;
                    c.get_url   = get_url_copy;
                    if (res != CURLE_OK)
                    {
                        c.success    = false;
                        c.curl_error = curl_easy_strerror(res);
                    }
                    else if (http_code != 201)
                    {
                        c.success    = false;
                        c.curl_error = fmt::format("HTTP {}", http_code);
                    }
                    else
                    {
                        c.success = true;
                        // Note: the actual result message (rich SFS/SIMS with correct
                        // visible_link/aesgcm body + meta, and proper groupchat for MUC)
                        // is posted from fd_cb in main thread via ch.send_message (and
                        // status printf with visible url to ch.buffer). We no longer
                        // send a minimal always-https "chat" from worker, as that caused
                        // type errors/rejects in MUC and wrong link type for ESFS.
                    }

                    // Signal the main thread via pipe and atomic flag.
                    // The pipe wakes weechat_hook_fd in the normal case;
                    // the atomic flag lets the timer callback detect
                    // completion even when the event loop is stalled
                    // (Python deadlocks, etc.).
                    c.worker_done.store(true);
                    // No XDEBUG here (see comment at thread start): worker thread must
                    // not call any weechat_* APIs.
                    ::write(c.pipe_write_fd, "x", 1);
                });
            }
            else
            {
                weechat_printf(account.buffer, "%s%s: upload failed - missing PUT or GET URL",
                              weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                account.upload_requests.erase(req_it);
            }
        }
        else
        {
            weechat_printf(account.buffer, "%s%s: upload slot response for unknown request ID: %s",
                          weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, id);
        }
    }
    else if (id && account.upload_requests.contains(id))
    {
        weechat_printf(account.buffer, "%s%s: upload slot response malformed or wrong type (type: %s)",
                      weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, type ? type : "(null)");
    }
    
    // XEP-0060: clean up publish tracking on bare <iq type='result'/> (no pubsub child)
    // and trigger a single-item re-fetch so the buffer updates immediately.
    if (id && type && weechat_strcasecmp(type, "result") == 0)
        trigger_publish_refetch(id);

    // XEP-0313: MAM query error handling with stale-cursor recovery
    if (id && type && weechat_strcasecmp(type, "error") == 0)
    {
        weechat::account::mam_query failed_mam_query;
        if (account.mam_query_search(&failed_mam_query, id))
        {
            const bool is_global_query = failed_mam_query.with.empty();
            bool recovered = false;

            if (is_global_query)
            {
                xmpp_stanza_t *err_elem = xmpp_stanza_get_child_by_name(stanza, "error");
                if (err_elem && xmpp_stanza_get_child_by_name_and_ns(
                        err_elem, "item-not-found",
                        "urn:ietf:params:xml:ns:xmpp-stanzas"))
                {
                    weechat_printf(account.buffer,
                                   "%sGlobal MAM cursor stale (item-not-found) — "
                                   "clearing cursor and retrying with time-based query",
                                   weechat_prefix("network"));
                    account.mam_cursor_clear("global");
                    account.mam_query_remove(failed_mam_query.id);
                    account.release_mam_slot();

                    time_t now = time(nullptr);
                    time_t fetch_days = weechat::config::instance
                        ? static_cast<time_t>(weechat::config::instance->look.mam_fetch_days.integer())
                        : 3;
                    time_t start = now - (fetch_days * 86400);
                    std::string retry_id = stanza::uuid(account.context);
                    account.add_mam_query(retry_id.c_str(), "",
                                          std::optional<time_t>(start),
                                          std::optional<time_t>(now));

                    stanza::xep0059::set rsm_set;
                    rsm_set.max(50);

                    stanza::xep0313::query retry_q;
                    stanza::xep0313::x_filter xf;
                    xf.start(fmt::format("{:%Y-%m-%dT%H:%M:%SZ}", fmt::gmtime(start)));
                    retry_q.filter(xf).rsm(rsm_set);

                    this->send(stanza::iq()
                        .type("set")
                        .id(retry_id)
                        .xep0313()
                        .query(retry_q)
                        .build(account.context)
                        .get());

                    recovered = true;
                }
            }

            if (!recovered)
            {
                weechat_printf(account.buffer,
                               "%sMAM query %s failed (IQ error) — ending catchup%s",
                               weechat_prefix("error"),
                               failed_mam_query.id.c_str(),
                               is_global_query ? " and flushing deferred OMEMO key-transports" : "");
                account.mam_query_remove(failed_mam_query.id);
                account.release_mam_slot();
                if (is_global_query)
                {
                    account.omemo.global_mam_catchup = false;
                    account.omemo.process_postponed_key_transports(account);
                    account.omemo.process_postponed_bundle_republish(account);
                }
            }
        }

        if (auto req_it = account.upload_requests.find(id); req_it != account.upload_requests.end())
        {
            xmpp_stanza_t *error_elem = xmpp_stanza_get_child_by_name(stanza, "error");
            const char *error_type = error_elem ? xmpp_stanza_get_attribute(error_elem, "type") : nullptr;
            
            // Try to get error text
            std::string error_msg = "Upload slot request failed";
            if (error_elem)
            {
                xmpp_stanza_t *text_elem = xmpp_stanza_get_child_by_name(error_elem, "text");
                if (text_elem)
                {
                    const std::string text = stanza_element_text(text_elem);
                    if (!text.empty())
                        error_msg = fmt::format("Upload slot request failed: {}", text);
                }
                else
                {
                    // Try to get error condition
                    xmpp_stanza_t *child = xmpp_stanza_get_children(error_elem);
                    while (child)
                    {
                        const char *name = xmpp_stanza_get_name(child);
                        if (name && std::string_view(name) != "text")
                        {
                            error_msg = fmt::format("Upload slot request failed: {}", name);
                            break;
                        }
                        child = xmpp_stanza_get_next(child);
                    }
                }
            }
            
            weechat_printf(account.buffer, "%s%s: %s (type: %s)",
                          weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                          error_msg.c_str(), error_type ? error_type : "unknown");
            
            account.upload_requests.erase(req_it);
        }

        // XEP-0442: if a disco#info IQ to a pubsub service returned an error,
        // flush deferred feeds via XEP-0060 plain items IQ (fallback path).
        // Without this, the deferred feeds hang forever since the success path
        // (inside the `if (query && type)` block) is only entered for type=result.
        if (id && account.pubsub_mam_disco_queries.contains(id))
        {
            std::string svc_jid = account.pubsub_mam_disco_queries[id];
            account.pubsub_mam_disco_queries.erase(id);

            if (auto def_it = account.pubsub_mam_deferred_feeds.find(svc_jid); def_it != account.pubsub_mam_deferred_feeds.end())
            {
                auto& [_, deferred] = *def_it;
                const int max_items = 20;
                for (const auto &feed_key : deferred)
                {
                    auto slash = feed_key.find('/');
                    if (slash == std::string::npos) continue;
                    std::string node_name = feed_key.substr(slash + 1);

                    std::string fuid = stanza::uuid(account.context);
                    stanza::xep0060::items def_its(node_name);
                    def_its.max_items(max_items);
                    stanza::xep0060::pubsub def_ps;
                    def_ps.items(def_its);
                    account.pubsub_fetch_ids[fuid] = {svc_jid, node_name, {}, max_items};
                    account.connection.send(stanza::iq()
                        .from(account.jid())
                        .to(svc_jid)
                        .type("get")
                        .id(fuid)
                        .xep0060()
                        .pubsub(def_ps)
                        .build(account.context)
                        .get());
                }
                account.pubsub_mam_deferred_feeds.erase(def_it);
            }
        }

        // XEP-0060: pubsub publish error — report to the originating buffer.
        {
            if (auto pub_it = account.pubsub_publish_ids.find(id); pub_it != account.pubsub_publish_ids.end())
            {
                auto& [_, ctx] = *pub_it;
                xmpp_stanza_t *error_elem = xmpp_stanza_get_child_by_name(stanza, "error");
                const std::string error_cond = error_elem
                    ? ::xmpp::iq_error_text(::xmpp::StanzaView(error_elem)) : "unknown error";
                struct t_gui_buffer *err_buf = ctx.buffer ? ctx.buffer : account.buffer;
                weechat_printf(err_buf,
                    "%s%s: publish failed for %s/%s (item %s): %s",
                    weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                    ctx.service.c_str(), ctx.node.c_str(), ctx.item_id.c_str(),
                    error_cond.c_str());
                account.pubsub_publish_ids.erase(pub_it);
            }
        }

        // XEP-0060: pubsub subscribe/unsubscribe error
        {
            if (auto sub_it = account.pubsub_subscribe_queries.find(id); sub_it != account.pubsub_subscribe_queries.end())
            {
                auto& [_, sub] = *sub_it;
                xmpp_stanza_t *error_elem = xmpp_stanza_get_child_by_name(stanza, "error");
                const std::string error_cond = error_elem
                    ? ::xmpp::iq_error_text(::xmpp::StanzaView(error_elem)) : "unknown error";
                struct t_gui_buffer *fb = sub.buffer ? sub.buffer : account.buffer;
                weechat_printf(fb,
                    "%s%s: subscribe to %s failed: %s",
                    weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                    sub.feed_key.c_str(), error_cond.c_str());
                account.pubsub_subscribe_queries.erase(sub_it);
            }
        }
        {
            if (auto unsub_it = account.pubsub_unsubscribe_queries.find(id); unsub_it != account.pubsub_unsubscribe_queries.end())
            {
                auto& [_, unsub] = *unsub_it;
                xmpp_stanza_t *error_elem = xmpp_stanza_get_child_by_name(stanza, "error");
                const std::string error_cond = error_elem
                    ? ::xmpp::iq_error_text(::xmpp::StanzaView(error_elem)) : "unknown error";
                struct t_gui_buffer *fb = unsub.buffer ? unsub.buffer : account.buffer;
                weechat_printf(fb,
                    "%s%s: unsubscribe from %s failed: %s",
                    weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                    unsub.feed_key.c_str(), error_cond.c_str());
                account.pubsub_unsubscribe_queries.erase(unsub_it);
            }
        }

        // XEP-0060: pubsub item-fetch error (e.g. forbidden, item-not-found)
        {
            if (auto fetch_it = account.pubsub_fetch_ids.find(id); fetch_it != account.pubsub_fetch_ids.end())
            {
                auto& [_, fi] = *fetch_it;
                const std::string &err_service = fi.service;
                const std::string &err_node    = fi.node;

                xmpp_stanza_t *error_elem = xmpp_stanza_get_child_by_name(stanza, "error");
                const std::string error_cond = error_elem
                    ? ::xmpp::iq_error_text(::xmpp::StanzaView(error_elem)) : "unknown error";

                // Report the error in the feed buffer if it already exists,
                // otherwise fall back to the account buffer.
                std::string feed_key = fmt::format("{}/{}", err_service, err_node);
                struct t_gui_buffer *err_buf = account.buffer;
                if (auto ch_it = account.channels.find(feed_key); ch_it != account.channels.end())
                {
                    auto& [_, ch] = *ch_it;
                    err_buf = ch.buffer;
                }

                weechat_printf(err_buf,
                    "%s%s: cannot fetch feed %s/%s: %s",
                    weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                    err_service.c_str(), err_node.c_str(),
                    error_cond.c_str());

                account.pubsub_fetch_ids.erase(fetch_it);
            }
        }
    }
    
    // XEP-0191: Blocking Command
    xmpp_stanza_t *blocklist = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "blocklist", "urn:xmpp:blocking");
    xmpp_stanza_t *block = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "block", "urn:xmpp:blocking");
    xmpp_stanza_t *unblock = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "unblock", "urn:xmpp:blocking");
    
    if (blocklist && type && weechat_strcasecmp(type, "result") == 0)
    {
        // Handle blocklist response — populate the picker if open, else print inline.
        xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(blocklist, "item");

        if (account.blocklist_picker)
        {
            // Picker is open: feed entries into it.
            using picker_t = weechat::ui::picker<std::string>;
            if (!item)
            {
                // No blocked JIDs — add a non-selectable placeholder row.
                account.blocklist_picker->add_entry(
                    picker_t::entry{"", "(no blocked JIDs)", "", false});
            }
            else
            {
                while (item)
                {
                    const char *jid = xmpp_stanza_get_attribute(item, "jid");
                    if (jid)
                        account.blocklist_picker->add_entry(
                            picker_t::entry{std::string(jid), std::string(jid), "", true});
                    item = xmpp_stanza_get_next(item);
                }
            }
        }
        else
        {
            // Picker not open (e.g. called without /blocklist picker path) — print inline.
            if (item)
            {
                weechat_printf(account.buffer, "%sBlocked JIDs:",
                              weechat_prefix("network"));
                while (item)
                {
                    const char *jid = xmpp_stanza_get_attribute(item, "jid");
                    if (jid)
                        weechat_printf(account.buffer, "  %s", jid);
                    item = xmpp_stanza_get_next(item);
                }
            }
            else
            {
                weechat_printf(account.buffer, "%sNo JIDs blocked",
                              weechat_prefix("network"));
            }
        }

        return true;
    }
    
    if (block && type && weechat_strcasecmp(type, "result") == 0)
    {
        weechat_printf(account.buffer, "%sBlock request successful",
                      weechat_prefix("network"));
        return true;
    }
    
    if (unblock && type && weechat_strcasecmp(type, "result") == 0)
    {
        weechat_printf(account.buffer, "%sUnblock request successful",
                      weechat_prefix("network"));
        return true;
    }

    // XEP-0191: server-pushed block/unblock IQ sets (§8.4, §8.5)
    if (block && type && weechat_strcasecmp(type, "set") == 0)
    {
        xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(block, "item");
        while (item)
        {
            const char *jid = xmpp_stanza_get_attribute(item, "jid");
            if (jid)
                weechat_printf(account.buffer, "%s%s was blocked",
                               weechat_prefix("network"), jid);
            item = xmpp_stanza_get_next(item);
        }
        // Acknowledge the server push
        account.connection.send(stanza::iq()
            .type("result")
            .id(id ? id : "")
            .to(from ? from : "")
            .from(to ? to : "")
            .build(account.context)
            .get());
        return true;
    }

    if (unblock && type && weechat_strcasecmp(type, "set") == 0)
    {
        xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(unblock, "item");
        if (item)
        {
            while (item)
            {
                const char *jid = xmpp_stanza_get_attribute(item, "jid");
                if (jid)
                    weechat_printf(account.buffer, "%s%s was unblocked",
                                   weechat_prefix("network"), jid);
                item = xmpp_stanza_get_next(item);
            }
        }
        else
        {
            weechat_printf(account.buffer, "%sAll JIDs unblocked",
                           weechat_prefix("network"));
        }
        // Acknowledge the server push
        account.connection.send(stanza::iq()
            .type("result")
            .id(id ? id : "")
            .to(from ? from : "")
            .from(to ? to : "")
            .build(account.context)
            .get());
        return true;
    }

    // XEP-0045 §10.2/§10.5/§10.7: muc#owner / muc#admin owner-tracked IQ results.
    // Match by IQ id only — muc#owner responses carry muc#owner, not disco#items.
    const char *owner_stanza_id = xmpp_stanza_get_id(stanza);
    if (owner_stanza_id && account.muc_owner_queries.contains(owner_stanza_id))
    {
        auto info = account.muc_owner_queries[owner_stanza_id];
        account.muc_owner_queries.erase(owner_stanza_id);
        struct t_gui_buffer *out = info.buffer ? info.buffer : account.buffer;

        xmpp_stanza_t *owner_q = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "query", "http://jabber.org/protocol/muc#owner");

        // Error path: print a friendly message and return.
        if (type && weechat_strcasecmp(type, "error") == 0)
        {
            std::string err = "server error";
            if (auto err_el = xmpp_stanza_get_child_by_name(stanza, "error"))
            {
                if (xmpp_stanza_get_child_by_name(err_el, "forbidden"))
                    err = "permission denied (you must be a room owner)";
                else if (xmpp_stanza_get_child_by_name(err_el, "not-allowed"))
                    err = "not allowed (room configuration is locked)";
                else if (xmpp_stanza_get_child_by_name(err_el, "item-not-found"))
                    err = "room not found";
                else if (xmpp_stanza_get_child_by_name(err_el, "conflict"))
                    err = "conflict";
                else if (xmpp_stanza_get_child_by_name(err_el, "not-acceptable"))
                    err = "not acceptable";
            }
            const char *what = "operation";
            switch (info.kind)
            {
                case weechat::account::muc_owner_kind::config_get:  what = "fetch room config form"; break;
                case weechat::account::muc_owner_kind::config_set:  what = "submit room config";    break;
                case weechat::account::muc_owner_kind::destroy:     what = "destroy room";          break;
                case weechat::account::muc_owner_kind::aff_set:     what = "set affiliation";       break;
            }
            weechat_printf(out, "%s%s: %s failed: %s",
                           weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                           what, err.c_str());
            return true;
        }

        if (type && weechat_strcasecmp(type, "result") == 0)
        {
            switch (info.kind)
            {
                case weechat::account::muc_owner_kind::config_get:
                {
                    if (auto ch_it = account.channels.find(info.room_jid);
                        ch_it != account.channels.end())
                    {
                        auto& [_, ch] = *ch_it;
                        weechat::channel::room_config_form form;

                        xmpp_stanza_t *xdata = owner_q
                            ? xmpp_stanza_get_child_by_name_and_ns(
                                  owner_q, "x", "jabber:x:data")
                            : nullptr;
                        if (xdata)
                        {
                            if (const char *sid = xmpp_stanza_get_attribute(xdata, "sessionid"))
                                form.sessionid = sid;
                            xmpp_stanza_t *field = xmpp_stanza_get_child_by_name(xdata, "field");
                            while (field)
                            {
                                weechat::channel::room_config_field f;
                                if (const char *var = xmpp_stanza_get_attribute(field, "var"))
                                    f.var = var;
                                if (const char *typ = xmpp_stanza_get_attribute(field, "type"))
                                    f.type = typ;
                                if (const char *lbl = xmpp_stanza_get_attribute(field, "label"))
                                    f.label = lbl;
                                // xmpp_stanza_get_text_ptr only works on raw text
                                // nodes; <value> elements need get_text (see caps
                                // hash builder ~L3486).
                                for (xmpp_stanza_t *v = xmpp_stanza_get_children(field);
                                     v; v = xmpp_stanza_get_next(v))
                                {
                                    const char *vname = xmpp_stanza_get_name(v);
                                    if (!vname || weechat_strcasecmp(vname, "value") != 0)
                                        continue;
                                    f.values.push_back(stanza_element_text(v));
                                }
                                if (!f.var.empty())
                                    form.fields.push_back(std::move(f));
                                field = xmpp_stanza_get_next(field);
                            }
                        }

                        // If /setmodes --confirm is waiting on this form
                        // (no cached form was available at submit time),
                        // take the pending diff, apply it to the form,
                        // and send the submit. Single-shot apply.
                        auto pending = ch.take_pending_setmodes();
                        if (pending.has_value())
                        {
                            // Apply the diff in place.
                            for (int i = 0; i < 7; ++i)
                            {
                                if (pending->want_set[i] || pending->want_clear[i])
                                {
                                    // Map index → field var
                                    static constexpr const char *const vars[7] = {
                                        "muc#roomconfig_moderatedroom",
                                        "muc#roomconfig_membersonly",
                                        "muc#roomconfig_passwordprotectedroom",
                                        "muc#roomconfig_publicroom",
                                        "muc#roomconfig_persistentroom",
                                        "muc#roomconfig_whois",
                                        "muc#roomconfig_whois",
                                    };
                                    std::string val;
                                    if (i == 5 && pending->want_set[5])       val = "anyone";
                                    else if (i == 6 && pending->want_set[6])  val = "moderators";
                                    else if (i == 5 || i == 6)                val = "moderators"; // want_clear
                                    else if (i == 3)                          val = pending->want_set[i] ? "0" : "1"; // p=hidden
                                    else                                     val = pending->want_set[i] ? "1" : "0";
                                    for (auto &fld : form.fields)
                                    {
                                        if (fld.var == vars[i])
                                        {
                                            fld.values = { val };
                                            break;
                                        }
                                    }
                                }
                            }
                            // +k sets roomsecret; -k clears it (Prosody and
                            // others use roomsecret, not passwordprotectedroom).
                            if (pending->want_set[2] || pending->want_clear[2])
                            {
                                for (auto &fld : form.fields)
                                {
                                    if (fld.var == "muc#roomconfig_roomsecret")
                                    {
                                        fld.values = { pending->want_set[2]
                                                           ? pending->password
                                                           : std::string{} };
                                        break;
                                    }
                                }
                            }

                            ch.store_config_form(form);  // cache the mutated form

                            weechat::channel::prepare_room_config_submit(form);

                            // Build the submit.
                            stanza::xep0004::form submit("submit");
                            submit.add_hidden("FORM_TYPE",
                                "http://jabber.org/protocol/muc#roomconfig");
                            for (const auto &fld : form.fields)
                            {
                                if (!weechat::channel::include_room_config_field_in_submit(fld))
                                    continue;
                                stanza::xep0004::field fd(fld.var);
                                if (!fld.type.empty()) fd.type(fld.type);
                                for (const auto &v : fld.values)
                                    fd.value(v);
                                submit.add_field(fd);
                            }

                            std::string set_id = stanza::uuid(account.context);
                            weechat::account::muc_owner_query_info set_info{
                                info.room_jid,
                                out,
                                weechat::account::muc_owner_kind::config_set
                            };
                            account.muc_owner_queries[set_id] = set_info;

                            stanza::xep0045::xep0045owner::query q;
                            q.form(submit);
                            auto set_iq = stanza::iq().type("set")
                                              .to(info.room_jid).id(set_id);
                            set_iq.muc_owner(q);
                            account.connection.send(
                                set_iq.build(account.context).get());

                            weechat_printf(out, "%s%s: room config form fetched and "
                                           "/setmodes diff applied — submit sent",
                                           weechat_prefix("network"),
                                           WEECHAT_XMPP_PLUGIN_NAME);
                            return true;
                        }

                        // No pending diff — just cache the form and
                        // tell the user.
                        ch.store_config_form(std::move(form));
                        weechat_printf(out, "%s%s: room config form cached for %s — "
                                       "use /setmodes to apply changes",
                                       weechat_prefix("network"),
                                       WEECHAT_XMPP_PLUGIN_NAME,
                                       info.room_jid.c_str());
                    }
                    else
                    {
                        weechat_printf(out, "%s%s: room config received for %s but channel no longer exists",
                                       weechat_prefix("error"),
                                       WEECHAT_XMPP_PLUGIN_NAME,
                                       info.room_jid.c_str());
                    }
                    return true;
                }
                case weechat::account::muc_owner_kind::config_set:
                {
                    weechat_printf(out, "%s%s: room config submitted for %s",
                                   weechat_prefix("network"),
                                   WEECHAT_XMPP_PLUGIN_NAME,
                                   info.room_jid.c_str());
                    if (auto ch_it = account.channels.find(info.room_jid);
                        ch_it != account.channels.end())
                    {
                        auto& ch = ch_it->second;
                        ch.clear_config_form();
                        // Refresh disco#info so /modes and the status bar
                        // show the new state. Drop the idempotency marker
                        // so the query actually goes out, then send it.
                        account.muc_modes_fetched.erase(info.room_jid);
                        std::string disco_id = stanza::uuid(account.context);
                        account.muc_modes_queries[disco_id] = info.room_jid;
                        account.connection.send(
                            stanza::iq().type("get")
                                .to(info.room_jid).id(disco_id)
                                .xep0030().query()
                                .build(account.context).get());
                    }
                    return true;
                }
                case weechat::account::muc_owner_kind::destroy:
                {
                    weechat_printf(out, "%s%s: room %s destroyed",
                                   weechat_prefix("network"),
                                   WEECHAT_XMPP_PLUGIN_NAME,
                                   info.room_jid.c_str());
                    return true;
                }
                case weechat::account::muc_owner_kind::aff_set:
                {
                    weechat_printf(out, "%s%s: affiliation change applied for %s",
                                   weechat_prefix("network"),
                                   WEECHAT_XMPP_PLUGIN_NAME,
                                   info.room_jid.c_str());
                    return true;
                }
            }
        }
        return true;
    }

    // XEP-0030: Service Discovery - disco#items response
    xmpp_stanza_t *items_query = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "query", "http://jabber.org/protocol/disco#items");
    
    if (items_query && type && weechat_strcasecmp(type, "result") == 0)
    {
        // docs/planning-muc-omemo.md §2.2: disco#items result for a joined MUC?
        // If so, populate the channel's members map with every nick from the
        // room's item list. Real JIDs (if visible) were already recorded from
        // presence <x xmlns='muc#user'><item jid='...'/></x>. Having the full
        // occupant list here makes all_occupants_have_real_jid() reliable.
        if (from)
        {
            std::string from_bare = ::jid(nullptr, from).bare;
            if (auto ch_it = account.channels.find(from_bare); ch_it != account.channels.end())
            {
                auto& [_, ch] = *ch_it;
                if (ch.type == weechat::channel::chat_type::MUC)
                {
                    xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(items_query, "item");
                    while (item)
                    {
                        const char *item_jid = xmpp_stanza_get_attribute(item, "jid");
                        if (item_jid)
                        {
                            std::string nick = ::jid(nullptr, item_jid).resource;
                            if (!nick.empty())
                            {
                                ch.add_member(nick.c_str(), nullptr, std::nullopt);
                            }
                        }
                        item = xmpp_stanza_get_next(item);
                    }

                // docs/planning-muc-omemo.md §2.3: Now that we have the full occupant
                // list from disco#items, request devicelists for any that have a
                // visible real_jid (idempotent — the request path early-returns if
                // already in flight or recently requested).
                for (const auto& [occ_id, member] : ch.members)
                {
                    if (member.real_jid && !member.real_jid->empty())
                    {
                        account.omemo.request_axolotl_devicelist(account, *member.real_jid);
                    }
                }

                return true; // occupant list handled; do not mis-treat nicks as upload services
                }
            }
        }

        // XEP-0045 §9.3 / docs/planning-muc-omemo.md §2.2 (admin affiliation fallback):
        // If this is a muc#admin result for a MUC we are in, the <item> children
        // contain real JIDs (jid attr) + nicks when the requester has sufficient
        // affiliation. Feed them through add_member so the central real_jid logic
        // (and automatic devicelist request) runs.
        xmpp_stanza_t *admin_query = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "query", "http://jabber.org/protocol/muc#admin");
        if (admin_query && type && weechat_strcasecmp(type, "result") == 0)
        {
            if (from)
            {
                std::string from_bare = ::jid(nullptr, from).bare;
                if (auto ch_it = account.channels.find(from_bare); ch_it != account.channels.end())
                {
                    auto& [_, ch] = *ch_it;
                    if (ch.type == weechat::channel::chat_type::MUC)
                    {
                        xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(admin_query, "item");
                        while (item)
                        {
                            const char *nick = xmpp_stanza_get_attribute(item, "nick");
                            const char *real_jid = xmpp_stanza_get_attribute(item, "jid");
                            if (nick && real_jid)
                            {
                                ch.add_member(nick, nullptr,
                                    std::optional<std::string_view>(real_jid));
                            }
                            item = xmpp_stanza_get_next(item);
                        }
                    }
                    // Do not return here — let other admin result processing (if any)
                    // or fallthrough continue; this is discovery only.
                }
            }
        }

        // Look for HTTP upload service in items
        xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(items_query, "item");
        while (item)
        {
            const char *item_jid = xmpp_stanza_get_attribute(item, "jid");
            if (item_jid)
            {
                // Query this item for its features
                std::string disco_info_id = stanza::uuid(account.context);
                account.upload_disco_queries[disco_info_id] = item_jid;
                
                account.connection.send(stanza::iq()
                            .from(account.jid())
                            .to(item_jid)
                            .type("get")
                            .id(disco_info_id)
                            .xep0030()
                            .query()
                            .build(account.context)
                            .get());
            }
            item = xmpp_stanza_get_next(item);
        }

        // XEP-0050: Ad-Hoc Commands — handle list and execute/form results
        const char *items_node = xmpp_stanza_get_attribute(items_query, "node");
        bool is_commands_node = items_node
            && std::string_view(items_node) == "http://jabber.org/protocol/commands";
        const char *iq_id = xmpp_stanza_get_id(stanza);
        bool is_adhoc_query = iq_id && account.adhoc_queries.contains(iq_id);

        if (is_commands_node && is_adhoc_query)
        {
            auto &adhoc_info = account.adhoc_queries[iq_id];
            struct t_gui_buffer *adhoc_buf = adhoc_info.buffer
                ? adhoc_info.buffer : account.buffer;

            // For inline (non-picker) path, print header first.
            if (!adhoc_info.picker)
                weechat_printf_date_tags(adhoc_buf, 0, "xmpp_adhoc,notify_none",
                                         "%s%sCommands available on %s%s:",
                                         weechat_prefix("network"),
                                         weechat_color("bold"),
                                         adhoc_info.target_jid.c_str(),
                                         weechat_color("reset"));

            xmpp_stanza_t *cmd_item = xmpp_stanza_get_child_by_name(items_query, "item");
            int count = 0;
            while (cmd_item)
            {
                const char *cmd_node = xmpp_stanza_get_attribute(cmd_item, "node");
                const char *cmd_name = xmpp_stanza_get_attribute(cmd_item, "name");
                const char *cmd_jid  = xmpp_stanza_get_attribute(cmd_item, "jid");

                if (adhoc_info.picker)
                {
                    // Picker path: add entry (value = node URI, label = friendly name)
                    using picker_t = weechat::ui::picker<std::string>;
                    std::string label = cmd_name ? cmd_name : (cmd_node ? cmd_node : "(unnamed)");
                    std::string sublabel = cmd_node ? cmd_node : "";
                    adhoc_info.picker->add_entry(
                        picker_t::entry{cmd_node ? std::string(cmd_node) : "",
                                        label, sublabel, true});
                }
                else
                {
                    // Inline print path
                    weechat_printf_date_tags(adhoc_buf, 0, "xmpp_adhoc,notify_none",
                                             "%s  %s%-40s%s  %s%s",
                                             weechat_prefix("network"),
                                             weechat_color("bold"),
                                             cmd_name ? cmd_name : "(unnamed)",
                                             weechat_color("reset"),
                                             cmd_node ? cmd_node : "",
                                             cmd_jid && cmd_jid != adhoc_info.target_jid
                                                 ? fmt::format(" [{}]", cmd_jid).c_str() : "");
                }
                count++;
                cmd_item = xmpp_stanza_get_next(cmd_item);
            }

            if (!adhoc_info.picker)
            {
                if (count == 0)
                    weechat_printf_date_tags(adhoc_buf, 0, "xmpp_adhoc,notify_none",
                                             "%s  (no commands available)",
                                             weechat_prefix("network"));
                else
                    weechat_printf_date_tags(adhoc_buf, 0, "xmpp_adhoc,notify_none",
                                             "%s  Use /adhoc %s <node> to execute a command",
                                             weechat_prefix("network"),
                                             adhoc_info.target_jid.c_str());
            }
            else if (count == 0)
            {
                using picker_t = weechat::ui::picker<std::string>;
                adhoc_info.picker->add_entry(
                    picker_t::entry{"", "(no commands available)", "", false});
            }

            account.adhoc_queries.erase(iq_id);
        }

        // XEP-0060: /feed <service> — auto-fetch all discovered nodes
        if (iq_id && account.pubsub_disco_queries.contains(iq_id))
        {
            std::string feed_service = account.pubsub_disco_queries[iq_id];
            account.pubsub_disco_queries.erase(iq_id);

            int node_count = 0;
            xmpp_stanza_t *disco_item = xmpp_stanza_get_child_by_name(items_query, "item");
            while (disco_item)
            {
                const char *node_attr = xmpp_stanza_get_attribute(disco_item, "node");
                if (node_attr)
                {
                    std::string node_name(node_attr);

                    // XEP-0472 §4.1: skip comment sub-nodes — they are per-post
                    // threads, not independent feeds, and are always empty at the
                    // top-level disco level.
                    static constexpr std::string_view comments_prefix =
                        "urn:xmpp:microblog:0:comments/";
                    if (node_name.starts_with(comments_prefix))
                    {
                        disco_item = xmpp_stanza_get_next(disco_item);
                        continue;
                    }

                    std::string feed_key = fmt::format("{}/{}", feed_service, node_name);

                    // Ensure FEED buffer exists
                    auto [disco_ch_it, disco_inserted] = account.channels.try_emplace(
                        feed_key,
                        account,
                        weechat::channel::chat_type::FEED,
                        feed_key,
                        feed_key);
                    if (disco_inserted)
                        account.feed_open_register(feed_key);

                    // Fetch items for this node (with RSM <set> for paging)
                    // RSM <set><max>20</max><before/></set> or <before>cursor</before>
                    std::string cursor_key = fmt::format("pubsub:{}", feed_key);
                    std::string saved_cursor = account.mam_cursor_get(cursor_key);
                     stanza::xep0060::items disco_its(node_name);
                     disco_its.max_items(20);
                     stanza::xep0059::set disco_rset;
                     disco_rset.max(20).before(saved_cursor.empty()
                         ? std::nullopt : std::optional<std::string>{saved_cursor});
                     stanza::xep0060::pubsub disco_ps;
                    disco_ps.items(disco_its).rsm(disco_rset);
                    std::string uid = stanza::uuid(account.context);
                    account.pubsub_fetch_ids[uid] = {feed_service, node_name, "", 20};
                    this->send(stanza::iq()
                        .from(account.jid())
                        .to(feed_service)
                        .type("get")
                        .id(uid)
                        .xep0060()
                        .pubsub(disco_ps)
                        .build(account.context)
                        .get());
                    node_count++;
                }
                disco_item = xmpp_stanza_get_next(disco_item);
            }

            weechat_printf(account.buffer,
                           "%sFeed discovery on %s: fetching %d node(s)",
                           weechat_prefix("network"),
                           feed_service.c_str(), node_count);
        }
    }

    // XEP-0050: Ad-Hoc Commands — handle command execute/form result (type=result/error)
    xmpp_stanza_t *adhoc_command = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "command", "http://jabber.org/protocol/commands");
    if (adhoc_command && type)
    {
        const char *iq_id = xmpp_stanza_get_id(stanza);
        bool is_adhoc_query = iq_id && account.adhoc_queries.contains(iq_id);
        struct t_gui_buffer *adhoc_buf = is_adhoc_query
            ? (account.adhoc_queries[iq_id].buffer
               ? account.adhoc_queries[iq_id].buffer : account.buffer)
            : account.buffer;
        const char *cmd_node = xmpp_stanza_get_attribute(adhoc_command, "node");
        const char *cmd_status = xmpp_stanza_get_attribute(adhoc_command, "status");
        const char *session_id = xmpp_stanza_get_attribute(adhoc_command, "sessionid");
        const char *from_jid = xmpp_stanza_get_from(stanza);

        if (weechat_strcasecmp(type, "error") == 0)
        {
            weechat_printf_date_tags(adhoc_buf, 0, "xmpp_adhoc,notify_none",
                                     "%s[adhoc] Error executing command %s",
                                     weechat_prefix("error"),
                                     cmd_node ? cmd_node : "(unknown)");
        }
        else if (weechat_strcasecmp(type, "result") == 0)
        {
            // Check for a data form to display
            xmpp_stanza_t *x_form = xmpp_stanza_get_child_by_name_and_ns(
                adhoc_command, "x", "jabber:x:data");

            if (x_form)
            {
                const char *form_type = xmpp_stanza_get_attribute(x_form, "type");
                if (form_type && std::string_view(form_type) == "result")
                {
                    // Display result form (read-only)
                    render_data_form(adhoc_buf, x_form, from_jid, cmd_node, nullptr);
                }
                else
                {
                    // Input form — render and prompt for submission
                    render_data_form(adhoc_buf, x_form, from_jid, cmd_node, session_id);
                }
            }
            else if (cmd_status && std::string_view(cmd_status) == "completed")
            {
                // Command completed with no form — check for <note>
                xmpp_stanza_t *note = xmpp_stanza_get_child_by_name(adhoc_command, "note");
                const std::string note_text = stanza_element_text(note);
                weechat_printf_date_tags(adhoc_buf, 0, "xmpp_adhoc,notify_none",
                                         "%s[adhoc] Command %s completed%s%s",
                                         weechat_prefix("network"),
                                         cmd_node ? cmd_node : "",
                                         note_text.empty() ? "" : ": ",
                                         note_text.empty() ? "" : note_text.c_str());
            }
            else if (cmd_status && std::string_view(cmd_status) == "executing" && !x_form)
            {
                weechat_printf_date_tags(adhoc_buf, 0, "xmpp_adhoc,notify_none",
                                         "%s[adhoc] Command %s in progress (no form)",
                                         weechat_prefix("network"),
                                         cmd_node ? cmd_node : "");
            }
        }

        if (is_adhoc_query)
            account.adhoc_queries.erase(iq_id);
    }

    // XEP-0433: Extended Channel Search — handle <result> or <search> IQ responses
    {
        const char *cs_id = xmpp_stanza_get_id(stanza);
        bool is_cs_query = cs_id && account.channel_search_queries.contains(cs_id);

        if (is_cs_query && type)
        {
            auto &cs_info = account.channel_search_queries[cs_id];
            struct t_gui_buffer *cs_buf = cs_info.buffer ? cs_info.buffer : account.buffer;

            if (weechat_strcasecmp(type, "error") == 0)
            {
                // Try to extract a human-readable error
                xmpp_stanza_t *error_el = xmpp_stanza_get_child_by_name(stanza, "error");
                std::string err_text_str;
                const char *err_condition = nullptr;
                if (error_el)
                {
                    if (xmpp_stanza_t *text_el = xmpp_stanza_get_child_by_name(error_el, "text"))
                        err_text_str = stanza_element_text(text_el);

                    // XMPP stanza errors usually encode the condition as a child in
                    // urn:ietf:params:xml:ns:xmpp-stanzas (e.g. <bad-request/>).
                    if (err_text_str.empty())
                    {
                        xmpp_stanza_t *cond = xmpp_stanza_get_children(error_el);
                        while (cond)
                        {
                            const char *cond_ns = xmpp_stanza_get_ns(cond);
                            const char *cond_name = xmpp_stanza_get_name(cond);
                            if (cond_name
                                && cond_ns
                                && std::string_view(cond_ns) == "urn:ietf:params:xml:ns:xmpp-stanzas"
                                && std::string_view(cond_name) != "text")
                            {
                                err_condition = cond_name;
                                break;
                            }
                            cond = xmpp_stanza_get_next(cond);
                        }
                    }
                }
                weechat_printf_date_tags(cs_buf, 0, "xmpp_channel_search,notify_none",
                                         "%s[search] Error from %s: %s",
                                         weechat_prefix("error"),
                                         cs_info.service_jid.c_str(),
                                         !err_text_str.empty() ? err_text_str.c_str()
                                                  : (err_condition ? err_condition : "unknown error"));
                account.channel_search_queries.erase(cs_id);
            }
            else if (weechat_strcasecmp(type, "result") == 0)
            {
                if (cs_info.form_requested)
                {
                    // Step 1 response: service returned a search form.
                    xmpp_stanza_t *search_el = xmpp_stanza_get_child_by_name_and_ns(
                        stanza, "search", "urn:xmpp:channel-search:0:search");
                    xmpp_stanza_t *x_form = search_el
                        ? xmpp_stanza_get_child_by_name_and_ns(search_el, "x", "jabber:x:data")
                        : nullptr;

                    if (!search_el || !x_form)
                    {
                        weechat_printf_date_tags(cs_buf, 0, "xmpp_channel_search,notify_none",
                                                 "%s[search] Unexpected response from %s (missing form)",
                                                 weechat_prefix("error"),
                                                 cs_info.service_jid.c_str());
                        account.channel_search_queries.erase(cs_id);
                        return true;
                    }

                    const std::string submit_id = stanza::uuid(account.context);

                    weechat::account::channel_search_query_info next_info = cs_info;
                    next_info.form_requested = false;
                    account.channel_search_queries[submit_id] = next_info;

                    // Build search submit based on XEP-0433 fields using fluent builders.
                    xmpp_ctx_t *ctx = account.context;

                    struct search_spec : stanza::spec {
                        search_spec() : spec("search") {
                            xmlns<urn::xmpp::channel_search::_0>();
                        }
                    };

                    auto submit_form = stanza::xep0004::form("submit")
                        .add_hidden("FORM_TYPE", "urn:xmpp:channel-search:0:search-params");
                    if (!cs_info.keywords.empty())
                    {
                        stanza::xep0004::field q_field("q");
                        q_field.type("text-single").value(cs_info.keywords);
                        submit_form.add_field(q_field);
                        // Required by XEP-0433 if q is supported.
                        stanza::xep0004::field sn_field("sinname");
                        sn_field.type("boolean").value("true");
                        submit_form.add_field(sn_field);
                        stanza::xep0004::field sd_field("sindescription");
                        sd_field.type("boolean").value("true");
                        submit_form.add_field(sd_field);
                        stanza::xep0004::field sa_field("sinaddress");
                        sa_field.type("boolean").value("true");
                        submit_form.add_field(sa_field);
                    }
                    // Prefer stable sort by address for broad compatibility.
                    stanza::xep0004::field key_field("key");
                    key_field.type("list-single").value("{urn:xmpp:channel-search:0:order}address");
                    submit_form.add_field(key_field);
                    // Restrict to MUC channels when service supports the field.
                    stanza::xep0004::field types_field("types");
                    types_field.type("list-multi").value("xep-0045");
                    submit_form.add_field(types_field);

                    search_spec ss;
                    ss.child(submit_form);

                    auto submit_iq = stanza::iq()
                        .type("get")
                        .id(submit_id)
                        .to(cs_info.service_jid);
                    submit_iq.child(ss);

                    account.connection.send(submit_iq.build(ctx).get());

                    account.channel_search_queries.erase(cs_id);
                    return true;
                }

                // Response wraps results in <result xmlns='urn:xmpp:channel-search:0:search'>
                xmpp_stanza_t *result_el = xmpp_stanza_get_child_by_name_and_ns(
                    stanza, "result", "urn:xmpp:channel-search:0:search");
                // Some services may reply with <search> instead of <result>
                if (!result_el)
                    result_el = xmpp_stanza_get_child_by_name_and_ns(
                        stanza, "search", "urn:xmpp:channel-search:0:search");

                if (result_el)
                {
                    int count = 0;
                    xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(result_el, "item");
                    while (item)
                    {
                        if (count == 0 && !cs_info.picker)
                        {
                            weechat_printf_date_tags(cs_buf, 0, "xmpp_channel_search,notify_none",
                                                     "%sMUC Rooms (via %s):",
                                                     weechat_prefix("network"),
                                                     cs_info.service_jid.c_str());
                        }

                        const char *address = xmpp_stanza_get_attribute(item, "address");
                        if (!address)
                        {
                            item = xmpp_stanza_get_next(item);
                            continue;
                        }

                        // Child elements may include: <name>, <nusers>, <description>,
                        // <is-open>, <language>, <service-type>, <anonymity-mode>.
                        xmpp_stanza_t *name_el  = xmpp_stanza_get_child_by_name(item, "name");
                        xmpp_stanza_t *nusers_el = xmpp_stanza_get_child_by_name(item, "nusers");
                        xmpp_stanza_t *desc_el  = xmpp_stanza_get_child_by_name(item, "description");
                        xmpp_stanza_t *open_el  = xmpp_stanza_get_child_by_name(item, "is-open");
                        xmpp_stanza_t *language_el = xmpp_stanza_get_child_by_name(item, "language");
                        xmpp_stanza_t *service_type_el = xmpp_stanza_get_child_by_name(item, "service-type");
                        xmpp_stanza_t *anonymity_el = xmpp_stanza_get_child_by_name(item, "anonymity-mode");

                        const std::string name_raw    = name_el  ? stanza_element_text(name_el)   : std::string {};
                        const std::string nusers_raw  = nusers_el ? stanza_element_text(nusers_el) : std::string {};
                        const std::string desc_raw    = desc_el  ? stanza_element_text(desc_el)   : std::string {};
                        const std::string language_raw = language_el ? stanza_element_text(language_el) : std::string {};
                        const std::string service_type_raw = service_type_el ? stanza_element_text(service_type_el) : std::string {};
                        const std::string anonymity_raw = anonymity_el ? stanza_element_text(anonymity_el) : std::string {};

                        std::string display = address;
                        if (!name_raw.empty())
                            display = name_raw + " <" + address + ">";

                        std::vector<std::string> meta_parts;
                        if (!nusers_raw.empty())
                            meta_parts.emplace_back(nusers_raw + " users");

                        bool is_open = false;
                        if (open_el)
                        {
                            const std::string open_raw = stanza_element_text(open_el);
                            if (open_raw.empty()
                                || weechat_strcasecmp(open_raw.c_str(), "true") == 0
                                || open_raw == "1")
                            {
                                is_open = true;
                            }
                        }
                        if (is_open)
                            meta_parts.emplace_back("open");

                        if (!language_raw.empty())
                            meta_parts.emplace_back(std::string("lang=") + language_raw);

                        if (!service_type_raw.empty())
                        {
                            std::string st = service_type_raw;
                            if (st == "xep-0045") st = "muc";
                            else if (st == "xep-0369") st = "mix";
                            meta_parts.emplace_back(std::string("type=") + st);
                        }

                        if (!anonymity_raw.empty())
                            meta_parts.emplace_back(std::string("anon=") + anonymity_raw);

                        std::string info_str;
                        if (!meta_parts.empty())
                        {
                            info_str = "[";
                            bool first = true;
                            std::ranges::for_each(meta_parts, [&](const auto &part) {
                                if (!first) info_str += ", ";
                                first = false;
                                info_str += part;
                            });
                            info_str += "]";
                        }

                        if (cs_info.picker)
                        {
                            // Picker path: add_entry with address as data, display as label.
                            // Sublabel carries metadata. Skip async disco#info enrichment
                            // since picker entries cannot be updated in-place.
                            using picker_t = weechat::ui::picker<std::string>;
                            std::string sublabel = info_str;
                            if (!desc_raw.empty())
                            {
                                std::string desc = desc_raw;
                                if (desc.length() > 60)
                                    desc = desc.substr(0, 57) + "...";
                                if (!sublabel.empty()) sublabel += "  ";
                                sublabel += desc;
                            }
                            cs_info.picker->add_entry(
                                picker_t::entry{std::string(address), display, sublabel, true});
                        }
                        else
                        {
                            // Inline print path (legacy / non-picker).
                            std::string info_bracketed = info_str.empty() ? "" : " " + info_str;
                            weechat_printf_date_tags(cs_buf, 0, "xmpp_channel_search,notify_none",
                                                     "  %s%s%s%s",
                                                     weechat_color("chat_nick"),
                                                     display.c_str(),
                                                     weechat_color("reset"),
                                                     info_bracketed.c_str());

                            // Truncate long descriptions
                            if (!desc_raw.empty())
                            {
                                std::string desc = desc_raw;
                                if (desc.length() > 120)
                                    desc = desc.substr(0, 117) + "...";
                                weechat_printf_date_tags(cs_buf, 0, "xmpp_channel_search,notify_none",
                                                         "    %s", desc.c_str());
                            }

                            // If the directory result is sparse, query room disco#info for
                            // additional metadata (name/description/occupants/language).
                            if (name_raw.empty()
                                || desc_raw.empty()
                                || nusers_raw.empty()
                                || language_raw.empty())
                            {
                                std::string disco_id = stanza::uuid(account.context);

                                weechat::account::channel_search_disco_query_info dq;
                                dq.buffer = cs_buf;
                                dq.room_jid = address;
                                account.channel_search_disco_queries[disco_id] = dq;

                                account.connection.send(stanza::iq()
                                    .to(address)
                                    .type("get")
                                    .id(disco_id)
                                    .xep0030()
                                    .query()
                                    .build(account.context)
                                    .get());
                            }
                        }

                        count++;
                        item = xmpp_stanza_get_next(item);
                    }

                    if (!cs_info.picker)
                    {
                        if (count == 0)
                        {
                            weechat_printf_date_tags(cs_buf, 0, "xmpp_channel_search,notify_none",
                                                     "%sNo rooms found matching your query",
                                                     weechat_prefix("network"));
                        }
                        else
                        {
                            weechat_printf_date_tags(cs_buf, 0, "xmpp_channel_search,notify_none",
                                                     "%sUse /enter <address> to join a room",
                                                     weechat_prefix("network"));
                        }
                    }
                    else if (count == 0)
                    {
                        // No results — add a non-selectable placeholder.
                        using picker_t = weechat::ui::picker<std::string>;
                        cs_info.picker->add_entry(
                            picker_t::entry{"", "(no rooms found)", "", false});
                    }
                }
                else
                {
                    weechat_printf_date_tags(cs_buf, 0, "xmpp_channel_search,notify_none",
                                             "%s[search] Unexpected response from %s (missing <result>)",
                                             weechat_prefix("error"),
                                             cs_info.service_jid.c_str());
                }

                account.channel_search_queries.erase(cs_id);
            }
        }
    }

    query = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "query", "http://jabber.org/protocol/disco#info");
    if (query && type)
    {
        const char *stanza_id = xmpp_stanza_get_id(stanza);

        // /list enrichment path: compact room metadata from follow-up disco#info.
        if (stanza_id && account.channel_search_disco_queries.contains(stanza_id))
        {
            auto dq = account.channel_search_disco_queries[stanza_id];
            struct t_gui_buffer *out = dq.buffer ? dq.buffer : account.buffer;

            if (weechat_strcasecmp(type, "result") == 0)
            {
                std::string name_s;
                std::string desc_s;
                std::string occ_s;
                std::string lang_s;

                xmpp_stanza_t *identity = xmpp_stanza_get_child_by_name(query, "identity");
                while (identity)
                {
                    const char *cat = xmpp_stanza_get_attribute(identity, "category");
                    const char *typ = xmpp_stanza_get_attribute(identity, "type");
                    const char *nam = xmpp_stanza_get_attribute(identity, "name");
                    if (cat && typ
                        && weechat_strcasecmp(cat, "conference") == 0
                        && weechat_strcasecmp(typ, "text") == 0
                        && nam && nam[0])
                    {
                        name_s = nam;
                        break;
                    }
                    identity = xmpp_stanza_get_next(identity);
                }

                xmpp_stanza_t *x = xmpp_stanza_get_child_by_name_and_ns(query, "x", "jabber:x:data");
                if (x)
                {
                    xmpp_stanza_t *field = xmpp_stanza_get_child_by_name(x, "field");
                    while (field)
                    {
                        const char *var = xmpp_stanza_get_attribute(field, "var");
                        if (!var)
                        {
                            field = xmpp_stanza_get_next(field);
                            continue;
                        }
                        std::string txt;
                        for (xmpp_stanza_t *vnode = xmpp_stanza_get_children(field);
                             vnode; vnode = xmpp_stanza_get_next(vnode))
                        {
                            const char *vname = xmpp_stanza_get_name(vnode);
                            if (!vname || weechat_strcasecmp(vname, "value") != 0)
                                continue;
                            txt = stanza_element_text(vnode);
                            break;
                        }
                        if (!txt.empty())
                        {
                            if (std::string_view(var) == "muc#roominfo_description")
                                desc_s = txt;
                            else if (std::string_view(var) == "muc#roominfo_occupants")
                                occ_s = txt;
                            else if (std::string_view(var) == "muc#roominfo_lang")
                                lang_s = txt;
                        }
                        field = xmpp_stanza_get_next(field);
                    }
                }

                if (!name_s.empty() || !desc_s.empty() || !occ_s.empty() || !lang_s.empty())
                {
                    std::vector<std::string> meta;
                    if (!occ_s.empty()) meta.emplace_back(occ_s + " users");
                    if (!lang_s.empty()) meta.emplace_back("lang=" + lang_s);

                    std::string header = dq.room_jid;
                    if (!name_s.empty())
                        header = name_s + " <" + dq.room_jid + ">";

                    std::string meta_s;
                    if (!meta.empty())
                    {
                        meta_s = " [";
                        bool first = true;
                        std::ranges::for_each(meta, [&](const auto &m) {
                            if (!first) meta_s += ", ";
                            first = false;
                            meta_s += m;
                        });
                        meta_s += "]";
                    }

                    weechat_printf_date_tags(out, 0, "xmpp_channel_search,notify_none",
                                             "    %s%s%s",
                                             weechat_color("chat_delimiters"),
                                             header.c_str(),
                                             meta_s.c_str());

                    if (!desc_s.empty())
                    {
                        if (desc_s.size() > 140)
                            desc_s = desc_s.substr(0, 137) + "...";
                        weechat_printf_date_tags(out, 0, "xmpp_channel_search,notify_none",
                                                 "      %s", desc_s.c_str());
                    }
                }
            }

            account.channel_search_disco_queries.erase(stanza_id);
            return true;
        }

        if (weechat_strcasecmp(type, "get") == 0)
        {
            const char *requested_node = xmpp_stanza_get_attribute(query, "node");

            // XEP-0030 §3.3: if a node= is present, it MUST match a recognized
            // node (our caps node "http://weechat.org#<hash>") otherwise return
            // <item-not-found/>.  An absent or empty node= always gets a normal reply.
            bool node_ok = true;
            if (requested_node && *requested_node)
            {
                // Recompute our caps hash to derive the canonical node URI.
                    // Use a throwaway pre-existing stanza as reply placeholder.
                    struct caps_placeholder : stanza::spec {
                        caps_placeholder() : spec("caps") {}
                    } cph;
                    auto dummy_sp = cph.build(account.context);
                    std::optional<std::string> computed_hash;
                    get_caps(dummy_sp.get(), &computed_hash);
                    // dummy_sp owns the ref — do NOT xmpp_stanza_release here.

                // Accept "http://weechat.org" (bare) or "http://weechat.org#<hash>"
                std::string_view req(requested_node);
                node_ok = (req == "http://weechat.org") ||
                          (computed_hash &&
                           req == std::string("http://weechat.org#") + *computed_hash);
            }

            if (node_ok)
            {
                reply = get_caps(xmpp_stanza_reply(stanza), nullptr, requested_node);
                account.connection.send(reply);
                xmpp_stanza_release(reply);
            }
            else
            {
                // Return <iq type='error'><error type='cancel'><item-not-found/></error></iq>
                xmpp_stanza_t *err_iq = xmpp_stanza_reply(stanza);
                xmpp_stanza_set_attribute(err_iq, "type", "error");
                // Build <error type='cancel'><item-not-found xmlns='...'/></error> via spec builder.
                struct inf_spec : stanza::spec {
                    inf_spec() : spec("item-not-found") {
                        attr("xmlns", "urn:ietf:params:xml:ns:xmpp-stanzas");
                    }
                } infs;
                struct err_spec : stanza::spec {
                    err_spec(stanza::spec &child) : spec("error") {
                        attr("type", "cancel");
                        this->child(child);
                    }
                } errs(infs);
                auto err_sp = errs.build(account.context);
                xmpp_stanza_add_child(err_iq, err_sp.get());
                account.connection.send(err_iq);
                xmpp_stanza_release(err_iq);
            }
        }

        if (weechat_strcasecmp(type, "result") == 0)
        {
            bool user_initiated = stanza_id && account.user_disco_queries.contains(stanza_id);
            bool caps_query = stanza_id && account.caps_disco_queries.contains(stanza_id);

            // Extract features for capability caching
            std::vector<std::string> features;
            xmpp_stanza_t *feature = xmpp_stanza_get_child_by_name(query, "feature");
            while (feature)
            {
                const char *var = xmpp_stanza_get_attribute(feature, "var");
                if (var)
                    features.push_back(var);
                feature = xmpp_stanza_get_next(feature);
            }

            if (from && !features.empty())
            {
                account.peer_features_update(from, features);
            }

            // XEP-0045 §6.4 / §6.5: if this result is a modes disco#info for
            // a MUC channel, extract muc_* features + muc#roominfo x-data form
            // and apply them to the channel.
            if (from && stanza_id && account.muc_modes_queries.contains(stanza_id))
            {
                std::string room_jid = account.muc_modes_queries[stanza_id];
                account.muc_modes_queries.erase(stanza_id);

                if (auto ch_it = account.channels.find(room_jid);
                    ch_it != account.channels.end() &&
                    ch_it->second.type == weechat::channel::chat_type::MUC)
                {
                    weechat::channel::muc_info info;

                    // XEP-0045 §6.4: rooms advertise positive or negative
                    // muc_* feature vars; the refresh is authoritative.
                    auto has_feat = [&](std::string_view f) {
                        return std::ranges::find(features, f) != features.end();
                    };
                    info.moderated    = has_feat("muc_moderated");
                    info.members_only = has_feat("muc_membersonly");
                    info.persistent   = has_feat("muc_persistent");
                    info.password     = has_feat("muc_passwordprotected");
                    info.hidden       = has_feat("muc_hidden");
                    if (has_feat("muc_unmoderated"))    info.moderated    = false;
                    if (has_feat("muc_open"))           info.members_only = false;
                    if (has_feat("muc_temporary"))      info.persistent   = false;
                    if (has_feat("muc_unsecured"))      info.password     = false;
                    if (has_feat("muc_public"))         info.hidden       = false;
                    if (has_feat("muc_nonanonymous"))   info.anon =
                        weechat::channel::muc_info::anonymity::nonanonymous;
                    else if (has_feat("muc_semianonymous")) info.anon =
                        weechat::channel::muc_info::anonymity::semianonymous;

                    // XEP-0045 §6.5: muc#roominfo FORM_TYPE (jabber:x:data)
                    xmpp_stanza_t *xdata = xmpp_stanza_get_child_by_name_and_ns(
                        query, "x", "jabber:x:data");
                    if (xdata)
                    {
                        for (xmpp_stanza_t *field = xmpp_stanza_get_child_by_name(xdata, "field");
                             field;
                             field = xmpp_stanza_get_next(field))
                        {
                            const char *var = xmpp_stanza_get_attribute(field, "var");
                            if (!var)
                                continue;
                            std::string txt;
                            for (xmpp_stanza_t *vnode = xmpp_stanza_get_children(field);
                                 vnode; vnode = xmpp_stanza_get_next(vnode))
                            {
                                const char *vname = xmpp_stanza_get_name(vnode);
                                if (!vname || weechat_strcasecmp(vname, "value") != 0)
                                    continue;
                                txt = stanza_element_text(vnode);
                                break;
                            }
                            if (txt.empty())
                                continue;
                            std::string_view v(var);
                            if      (v == "muc#roominfo_description") info.description = txt;
                            else if (v == "muc#roominfo_lang")        info.language    = txt;
                            else if (v == "muc#roominfo_subject")     info.subject     = txt;
                            else if (v == "muc#roominfo_logs")        info.logs_url    = txt;
                            else if (v == "muc#roominfo_occupants")
                            {
                                if (auto n = parse_int64(txt); n)
                                    info.occupants = static_cast<int>(*n);
                            }
                            else if (v == "muc#roomconfig_maxusers")
                            {
                                // XEP-0045 §16.5.3: value is "none" or a number.
                                if (txt != "none")
                                {
                                    if (auto n = parse_int64(txt); n)
                                        info.max_users = static_cast<int>(*n);
                                }
                            }
                            else if (v == "muc#roominfo_subjectmod")
                            {
                                info.subject_modifiable =
                                    !(txt == "0" || txt == "false");
                            }
                        }
                    }

                    ch_it->second.apply_muc_info(info);
                }
            }

            if (caps_query)
            {
                // XEP-0115 §5.4: verify the hash before caching to prevent caps poisoning.
                // Reconstruct the hash from identities + features + x-data forms.
                std::string ver_hash = account.caps_disco_queries[stanza_id];
                account.caps_disco_queries.erase(stanza_id);

                // --- build serial string S per §5.1 ---
                std::string S;

                // xmpp_stanza_get_next() returns the next sibling regardless of name,
                // so we must filter by element name to avoid picking up <feature>/<x> etc.
                auto next_named = [](xmpp_stanza_t *st, const char *name) -> xmpp_stanza_t * {
                    for (st = xmpp_stanza_get_next(st); st; st = xmpp_stanza_get_next(st)) {
                        const char *n = xmpp_stanza_get_name(st);
                        if (n && std::string_view(n) == name) return st;
                    }
                    return nullptr;
                };

                // Step 2-3: collect identities, sort by "category/type/lang/name"
                std::vector<std::string> identities;
                for (xmpp_stanza_t *id_elem = xmpp_stanza_get_child_by_name(query, "identity");
                     id_elem;
                     id_elem = next_named(id_elem, "identity"))
                {
                    const char *cat  = xmpp_stanza_get_attribute(id_elem, "category");
                    const char *typ  = xmpp_stanza_get_attribute(id_elem, "type");
                    // libstrophe strips the "xml:" namespace prefix when storing
                    // attributes, so xml:lang is stored under the key "lang".
                    const char *lang = xmpp_stanza_get_attribute(id_elem, "lang");
                    const char *name = xmpp_stanza_get_attribute(id_elem, "name");
                    identities.push_back(
                        std::string(cat  ? cat  : "") + "/" +
                        std::string(typ  ? typ  : "") + "/" +
                        std::string(lang ? lang : "") + "/" +
                        std::string(name ? name : ""));
                }
                std::ranges::sort(identities);
                std::ranges::for_each(identities, [&](const auto& ident){ S += ident + "<"; });

                // Step 4-5: features already collected above; sort and append
                std::vector<std::string> sorted_features = features;
                std::ranges::sort(sorted_features);
                std::ranges::for_each(sorted_features, [&](const auto& feat){ S += feat + "<"; });

                // Step 6-7: x-data forms (XEP-0128)
                struct FormData {
                    std::string form_type;
                    // fields: var → sorted values  (FORM_TYPE excluded)
                    std::vector<std::pair<std::string, std::vector<std::string>>> fields;
                };
                std::vector<FormData> forms;
                for (xmpp_stanza_t *x_elem = xmpp_stanza_get_child_by_name(query, "x");
                     x_elem; x_elem = next_named(x_elem, "x"))
                {
                    const char *xmlns_x = xmpp_stanza_get_attribute(x_elem, "xmlns");
                    if (xmlns_x && std::string_view(xmlns_x) == "jabber:x:data")
                    {
                        FormData fd;
                        bool has_form_type = false;
                        // find FORM_TYPE value (XEP-0115 §5: skip form if missing or not hidden)
                        // NOTE: xmpp_stanza_get_text_ptr only works on raw XMPP_STANZA_TEXT
                        // nodes; for element nodes like <value> use stanza_element_text().
                        for (xmpp_stanza_t *f = xmpp_stanza_get_child_by_name(x_elem, "field");
                             f; f = next_named(f, "field"))
                        {
                            const char *fvar = xmpp_stanza_get_attribute(f, "var");
                            if (fvar && std::string_view(fvar) == "FORM_TYPE")
                            {
                                xmpp_stanza_t *vnode = xmpp_stanza_get_child_by_name(f, "value");
                                if (vnode)
                                    fd.form_type = stanza_element_text(vnode);
                                has_form_type = true;
                                break;
                            }
                        }
                        // XEP-0115 §5: if no FORM_TYPE field, ignore this form entirely
                        if (!has_form_type)
                            continue;
                        // collect non-FORM_TYPE fields
                        for (xmpp_stanza_t *f = xmpp_stanza_get_child_by_name(x_elem, "field");
                             f; f = next_named(f, "field"))
                        {
                            const char *fvar = xmpp_stanza_get_attribute(f, "var");
                            if (fvar && std::string_view(fvar) != "FORM_TYPE")
                            {
                                std::vector<std::string> vals;
                                for (xmpp_stanza_t *vnode = xmpp_stanza_get_child_by_name(f, "value");
                                     vnode; vnode = next_named(vnode, "value"))
                                {
                                    // Always include the value, even if empty: an empty
                                    // <value></value> must contribute "" to the S string
                                    // (e.g. Psi sends os_version with an empty value).
                                    vals.push_back(stanza_element_text(vnode));
                                }
                                std::ranges::sort(vals);
                                fd.fields.emplace_back(fvar, std::move(vals));
                            }
                        }
                        std::ranges::sort(fd.fields,
                                  [](const auto &a, const auto &b){ return a.first < b.first; });
                        forms.push_back(std::move(fd));
                    }
                }
                // sort forms by FORM_TYPE
                std::ranges::sort(forms,
                          [](const FormData &a, const FormData &b){ return a.form_type < b.form_type; });
                for (const auto &form : forms)
                {
                    S += form.form_type + "<";
                    for (const auto &[fvar, fvals] : form.fields)
                    {
                        S += fvar + "<";
                        for (const auto &val : fvals)
                            S += val + "<";
                    }
                }

                // --- hash S with SHA-1 and base64-encode ---
                XDEBUG("caps: S string for {} (len={}): '{}'",
                       from ? from : "?", S.size(), S);
                unsigned char digest[20];
                unsigned int  digest_len = sizeof(digest);
                std::span<const char> S_span = S;
                EVP_Digest(S_span.data(), S_span.size(), digest, &digest_len, EVP_sha1(), nullptr);

                const int enc_size = 4 * static_cast<int>((digest_len + 2) / 3) + 1;
                std::string computed(static_cast<std::size_t>(enc_size), '\0');
                const int written = weechat_string_base_encode(
                    "64", reinterpret_cast<const char *>(digest),
                    static_cast<int>(digest_len), computed.data());
                if (written > 0)
                    computed.resize(static_cast<std::size_t>(written));
                else
                    computed.clear();

                if (computed == ver_hash)
                {
                    account.caps_cache_save(ver_hash, features);
                }
                else
                {
                    XDEBUG("caps: hash mismatch for {}: got '{}' expected '{}'; discarding",
                           from ? from : "?", computed, ver_hash);
                }
            }
            
            // Check if this is a response to upload service discovery
            bool upload_disco = stanza_id && account.upload_disco_queries.contains(stanza_id);
            std::string upload_service_jid; // kept alive for the identity loop below
            if (upload_disco)
            {
                upload_service_jid = account.upload_disco_queries[stanza_id];
                std::string service_jid = upload_service_jid;
                account.upload_disco_queries.erase(stanza_id);
                
                // Check if this service supports HTTP File Upload
                bool supports_upload = false;
                size_t max_size = 0;
                
                xmpp_stanza_t *feature = xmpp_stanza_get_child_by_name(query, "feature");
                while (feature)
                {
                    const char *var = xmpp_stanza_get_attribute(feature, "var");
                    if (var && std::string_view(var) == "urn:xmpp:http:upload:0")
                    {
                        supports_upload = true;
                    }
                    feature = xmpp_stanza_get_next(feature);
                }
                
                // Check for max file size in x data form
                if (supports_upload)
                {
                    xmpp_stanza_t *x = xmpp_stanza_get_child_by_name_and_ns(query, "x", "jabber:x:data");
                    if (x)
                    {
                        xmpp_stanza_t *field = xmpp_stanza_get_child_by_name(x, "field");
                        while (field)
                        {
                            const char *var = xmpp_stanza_get_attribute(field, "var");
                            if (var && std::string_view(var) == "max-file-size")
                            {
                                xmpp_stanza_t *value = xmpp_stanza_get_child_by_name(field, "value");
                                if (value)
                                {
                                    if (auto n = parse_int64(stanza_element_text(value)); n && *n > 0)
                                        max_size = static_cast<size_t>(*n);
                                }
                            }
                            field = xmpp_stanza_get_next(field);
                        }
                    }
                    
                    account.upload_service = service_jid;
                    account.upload_max_size = max_size;
                    
                    if (max_size > 0)
                    {
                        XDEBUG("Discovered upload service: {} (max: {} MB)",
                               service_jid, max_size / (1024 * 1024));
                    }
                    else
                    {
                        XDEBUG("Discovered upload service: {}", service_jid);
                    }
                }
            }

            // XEP-0442: handle MAM-support discovery response for a pubsub service.
            bool pubsub_mam_disco = stanza_id
                && account.pubsub_mam_disco_queries.contains(stanza_id);
            if (pubsub_mam_disco)
            {
                std::string svc_jid = account.pubsub_mam_disco_queries[stanza_id];
                account.pubsub_mam_disco_queries.erase(stanza_id);

                // Check whether this service advertises urn:xmpp:mam:2
                bool has_mam = std::ranges::any_of(features, [](const auto &feat) {
                    return feat == "urn:xmpp:mam:2";
                });

                if (has_mam)
                    account.pubsub_mam_services.insert(svc_jid);

                // Flush deferred feed restores for this service.
                if (auto def_it = account.pubsub_mam_deferred_feeds.find(svc_jid); def_it != account.pubsub_mam_deferred_feeds.end())
                {
                    auto& [_, deferred] = *def_it;
                    const int max_items = 20;
                    for (const auto &feed_key : deferred)
                    {
                        auto slash = feed_key.find('/');
                        if (slash == std::string::npos) continue;
                        std::string node_name = feed_key.substr(slash + 1);

                        if (has_mam)
                        {
                            // XEP-0442 + XEP-0413: MAM query with Order-By
                            std::string uid = stanza::uuid(account.context);

                            struct order_spec : stanza::spec {
                                order_spec() : spec("order") {
                                    xmlns<urn::xmpp::order_by::_1>();
                                    attr("by", "creation");
                                }
                            };
                            stanza::xep0059::set rsm_set;
                            rsm_set.max(static_cast<unsigned>(max_items));
                            struct pubsub_mam_q : stanza::xep0313::query {
                                pubsub_mam_q(std::string_view node_name_,
                                             order_spec &ord,
                                             stanza::xep0059::set &rsm)
                                    : spec("query") {
                                    xmlns<urn::xmpp::mam::_2>();
                                    attr("node", node_name_);
                                    child(ord);
                                    child(rsm);
                                }
                            };
                            order_spec ord;
                            pubsub_mam_q mam_q(node_name, ord, rsm_set);
                            account.pubsub_mam_queries[uid] = {svc_jid, node_name, {}, max_items};
                            account.connection.send(stanza::iq()
                                .from(account.jid())
                                .to(svc_jid)
                                .type("set")
                                .id(uid)
                                .xep0313()
                                .query(mam_q)
                                .build(account.context)
                                .get());
                        }
                        else
                        {
                            // Fallback: plain XEP-0060 pubsub items IQ
                            std::string fuid = stanza::uuid(account.context);
                            stanza::xep0060::items fb_its(node_name);
                            fb_its.max_items(max_items);
                            stanza::xep0060::pubsub fb_ps;
                            fb_ps.items(fb_its);
                            account.pubsub_fetch_ids[fuid] = {svc_jid, node_name, {}, max_items};
                            account.connection.send(stanza::iq()
                                .from(account.jid())
                                .to(svc_jid)
                                .type("get")
                                .id(fuid)
                                .xep0060()
                                .pubsub(fb_ps)
                                .build(account.context)
                                .get());
                        }
                    }
                    account.pubsub_mam_deferred_feeds.erase(def_it);
                }
            }

            if (user_initiated)
            {
                account.user_disco_queries.erase(stanza_id);
                
                const char *from_jid = xmpp_stanza_get_from(stanza);
                struct t_gui_buffer *output_buffer = account.buffer;
                
                weechat_printf(output_buffer, "");
                weechat_printf(output_buffer, "%sService Discovery for %s%s:", 
                              weechat_color("chat_prefix_network"),
                              weechat_color("chat_server"), 
                              from_jid ? from_jid : "server");
            }
            
            xmpp_stanza_t *identity = xmpp_stanza_get_child_by_name(query, "identity");
            while (identity)
            {
                std::string category;
                std::string name;
                std::string type;

                if (const char *attr = xmpp_stanza_get_attribute(identity, "category"))
                    category = attr;
                if (const char *attr = xmpp_stanza_get_attribute(identity, "name"))
                    name = unescape(attr);
                if (const char *attr = xmpp_stanza_get_attribute(identity, "type"))
                    type = attr;

                if (user_initiated)
                {
                    weechat_printf(account.buffer, "  %sIdentity:%s %s/%s %s%s%s",
                                  weechat_color("chat_prefix_network"),
                                  weechat_color("reset"),
                                  category.c_str(), type.c_str(),
                                  weechat_color("chat_delimiters"),
                                  name.empty() ? "" : name.c_str(),
                                  weechat_color("reset"));
                }

                if (category == "conference")
                {
                    if (auto ptr_channel = account.channels.find(from); ptr_channel != account.channels.end())
                    {
                        auto& [_, ch] = *ptr_channel;
                        ch.update_name(name.data());
                    }
                }

                // XEP-0060: record pubsub service components discovered at
                // connect time so /feed discover can list them without the
                // user having to know the service JID in advance.
                if (upload_disco && category == "pubsub")
                {
                    const std::string &svc_jid = upload_service_jid.empty()
                        ? (from ? std::string(from) : std::string{})
                        : upload_service_jid;
                    if (!svc_jid.empty())
                    {
                        auto &kps = account.known_pubsub_services;
                        if (!std::ranges::contains(kps, svc_jid))
                            kps.push_back(svc_jid);
                    }
                }
                // Legacy OMEMO devicelist fetch via hard-coded IQ id ("fetch2")
                // was removed. It generated request storms from disco identity
                // traffic (notably MUC contexts) and is superseded by targeted
                // roster/PM-driven request_devicelist() paths.
                
                identity = xmpp_stanza_get_next(identity);
            }
            
            if (user_initiated)
            {
                xmpp_stanza_t *feature = xmpp_stanza_get_child_by_name(query, "feature");
                if (feature)
                {
                    weechat_printf(account.buffer, "  %sFeatures:",
                                  weechat_color("chat_prefix_network"));
                    while (feature)
                    {
                        const char *var = xmpp_stanza_get_attribute(feature, "var");
                        if (var)
                            weechat_printf(account.buffer, "    %s", var);
                        feature = xmpp_stanza_get_next(feature);
                    }
                }
            }
        }
    }
    
    // Handle roster (RFC 6121)
    query = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "query", "jabber:iq:roster");

    // Parse <group> children of a roster <item> into account.roster[jid].groups
    auto parse_roster_groups = [&](xmpp_stanza_t *item, const char *jid) {
        account.roster[jid].groups.clear();
        for (xmpp_stanza_t *g = xmpp_stanza_get_children(item);
             g; g = xmpp_stanza_get_next(g))
        {
            if (weechat_strcasecmp(xmpp_stanza_get_name(g), "group") != 0)
                continue;
            const std::string group_name = stanza_element_text(g);
            if (!group_name.empty())
                account.roster[jid].groups.push_back(group_name);
        }
    };

    if (query && type && weechat_strcasecmp(type, "result") == 0)
    {
        xmpp_stanza_t *item;
        for (item = xmpp_stanza_get_children(query);
             item; item = xmpp_stanza_get_next(item))
        {
            const char *name = xmpp_stanza_get_name(item);
            if (weechat_strcasecmp(name, "item") != 0)
                continue;

            const char *jid = xmpp_stanza_get_attribute(item, "jid");
            const char *roster_name = xmpp_stanza_get_attribute(item, "name");
            const char *subscription = xmpp_stanza_get_attribute(item, "subscription");

            if (!jid)
                continue;

            account.roster[jid].jid = jid;
            account.roster[jid].name = roster_name ? roster_name : "";
            account.roster[jid].subscription = subscription ? subscription : "none";
            parse_roster_groups(item, jid);
        }
    }

    // RFC 6121 §2.1.6 — roster push: server sends IQ type="set" with a single item
    if (query && type && weechat_strcasecmp(type, "set") == 0)
    {
        xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(query, "item");
        if (item)
        {
            const char *jid          = xmpp_stanza_get_attribute(item, "jid");
            const char *roster_name  = xmpp_stanza_get_attribute(item, "name");
            const char *subscription = xmpp_stanza_get_attribute(item, "subscription");

            if (jid)
            {
                if (subscription && weechat_strcasecmp(subscription, "remove") == 0)
                {
                    account.roster.erase(jid);
                    weechat_printf(account.buffer, "%sRoster: %s removed",
                                   weechat_prefix("network"), jid);
                }
                else
                {
                    bool is_new = !account.roster.contains(jid);
                    account.roster[jid].jid = jid;
                    account.roster[jid].name = roster_name ? roster_name : "";
                    account.roster[jid].subscription = subscription ? subscription : "none";
                    parse_roster_groups(item, jid);

                    if (is_new)
                        weechat_printf(account.buffer, "%sRoster: %s added (%s)",
                                       weechat_prefix("network"), jid,
                                       subscription ? subscription : "none");
                    else
                        weechat_printf(account.buffer, "%sRoster: %s updated (subscription: %s)",
                                       weechat_prefix("network"), jid,
                                       subscription ? subscription : "none");

                }
            }
        }
        // Acknowledge the roster push
        account.connection.send(stanza::iq()
            .type("result")
            .id(id ? id : "")
            .to(from ? from : "")
            .from(to ? to : "")
            .build(account.context)
            .get());
        return true;
    }
    
    query = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "query", "jabber:iq:private");
    // BUG 1 fix: only process jabber:iq:private results, not gets/sets/errors.
    // An <iq type='error'> from a server that doesn't support XEP-0049 would
    // otherwise clear our bookmarks and crash on the autojoin nullptr deref below.
    if (query && type && weechat_strcasecmp(type, "result") == 0)
    {
        storage = xmpp_stanza_get_child_by_name_and_ns(
                query, "storage", "storage:bookmarks");
        if (storage)
        {
            // BUG 5 fix: only clear XEP-0048 bookmarks; preserve any entries
            // already populated by a XEP-0402 PEP push that arrived first.
            // We remove only entries whose source is XEP-0048 (i.e. those not
            // already in the map, which is empty on first connect and may have
            // been populated by the PEP push).  Simplest safe approach: clear
            // only if the map is empty (PEP push hasn't run yet); otherwise
            // we merge — the XEP-0049 data wins per-JID via operator[].
            if (account.bookmarks.empty())
                account.bookmarks.clear();

            for (conference = xmpp_stanza_get_children(storage);
                 conference; conference = xmpp_stanza_get_next(conference))
            {
                const char *name = xmpp_stanza_get_name(conference);
                if (weechat_strcasecmp(name, "conference") != 0)
                    continue;

                const char *jid = xmpp_stanza_get_attribute(conference, "jid");
                const char *autojoin = xmpp_stanza_get_attribute(conference, "autojoin");
                name = xmpp_stanza_get_attribute(conference, "name");
                nick = xmpp_stanza_get_child_by_name(conference, "nick");
                const std::string bookmark_nick = nick ? stanza_element_text(nick) : std::string {};

                if (!jid)
                    continue;

                // Store bookmark
                account.bookmarks[jid].jid = jid;
                account.bookmarks[jid].name = name ? name : "";
                account.bookmarks[jid].nick = bookmark_nick;
                account.bookmarks[jid].autojoin = autojoin
                    && (weechat_strcasecmp(autojoin, "true") == 0
                        || weechat_strcasecmp(autojoin, "1") == 0);

                account.connection.send(stanza::iq()
                            .from(to)
                            .to(jid)
                            .type("get")
                            .id(stanza::uuid(account.context))
                            .xep0030()
                            .query()
                            .build(account.context)
                            .get());
                // BUG 2 fix: autojoin attr may be absent (nullptr); use the already
                // computed bookmarks[jid].autojoin flag which is null-safe.
                if (account.bookmarks[jid].autojoin)
                {
                    // Skip autojoin for biboumi (IRC gateway) rooms
                    // Biboumi JIDs typically contain % (e.g., #channel%irc.server.org@gateway)
                    // or have 'biboumi' in the server component
                    std::string_view jid_sv(jid);
                    bool is_biboumi = jid_sv.contains('%') ||
                                      jid_sv.contains("biboumi") ||
                                      jid_sv.contains("@irc.");

                    if (is_biboumi)
                    {
                        weechat_printf(account.buffer,
                                      "%sSkipping autojoin for IRC gateway room: %s",
                                      weechat_prefix("network"), jid);
                    }
                    else
                    {
                        std::string cmd = fmt::format("/enter {}{}{}",
                                                      jid,
                                                      bookmark_nick.empty() ? "" : "/",
                                                      bookmark_nick);
                        weechat_command(account.buffer, cmd.c_str());
                        struct t_gui_buffer *ptr_buffer = nullptr;
                        if (auto ptr_channel = account.channels.find(jid); ptr_channel != account.channels.end())
                        {
                            auto& [_, ch] = *ptr_channel;
                            ptr_buffer = ch.buffer;
                            if (ptr_buffer)
                                ch.update_name(name);
                        }
                    }
                }
            }
        }
    }

    if (handle_omemo_pubsub_iq_event(stanza, own_jid))
        return true;

    // XEP-0442: PubSub MAM <fin> — marks end of a pubsub node MAM query.
    // The <fin> arrives as a child of the IQ result with id= matching our query IQ.
    {
        xmpp_stanza_t *pmam_fin = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "fin", "urn:xmpp:mam:2");
        if (pmam_fin && id && type && weechat_strcasecmp(type, "result") == 0)
        {
            if (auto pq_it = account.pubsub_mam_queries.find(id); pq_it != account.pubsub_mam_queries.end())
            {
                auto& [_, pq] = *pq_it;
                std::string svc_jid   = pq.service;
                std::string node_name = pq.node;
                account.pubsub_mam_queries.erase(pq_it);

                std::string feed_key = fmt::format("{}/{}", svc_jid, node_name);

                // Persist RSM <last> cursor so next reconnect can resume.
                xmpp_stanza_t *prsm = xmpp_stanza_get_child_by_name_and_ns(
                    pmam_fin, "set", "http://jabber.org/protocol/rsm");
                if (prsm)
                {
                    xmpp_stanza_t *plast = xmpp_stanza_get_child_by_name(prsm, "last");
                    if (plast)
                    {
                        const std::string plast_text = stanza_element_text(plast);
                        if (!plast_text.empty())
                            account.mam_cursor_set(
                                fmt::format("pubsub:{}", feed_key),
                                plast_text);
                    }
                }
                return true;
            }
        }
    }

    fin = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "fin", "urn:xmpp:mam:2");
    if (fin)
    {
        xmpp_stanza_t *set, *set__last;
        weechat::account::mam_query mam_query;

        const char *fin_complete = xmpp_stanza_get_attribute(fin, "complete");
        const char *fin_stable = xmpp_stanza_get_attribute(fin, "stable");
        const char *fin_abort = xmpp_stanza_get_attribute(fin, "abort");
        const bool fin_is_complete = fin_complete
            && (weechat_strcasecmp(fin_complete, "true") == 0
                || weechat_strcasecmp(fin_complete, "1") == 0);
        const bool fin_has_abort = fin_abort
            && (weechat_strcasecmp(fin_abort, "true") == 0
                || weechat_strcasecmp(fin_abort, "1") == 0);

        set = xmpp_stanza_get_child_by_name_and_ns(
            fin, "set", "http://jabber.org/protocol/rsm");
        if (id && account.mam_query_search(&mam_query, id))
        {
            // Check if this is a global MAM query (empty 'with')
            bool is_global_query = mam_query.with.empty();

            if (fin_has_abort)
            {
                weechat_printf(account.buffer,
                               "%sMAM query %s aborted by server (complete=%s stable=%s)",
                               weechat_prefix("error"),
                               mam_query.id.c_str(),
                               fin_complete ? fin_complete : "(unset)",
                               fin_stable ? fin_stable : "(unset)");
                account.mam_query_remove(mam_query.id);
                account.release_mam_slot();
                if (is_global_query)
                {
                    account.omemo.global_mam_catchup = false;
                    account.omemo.process_postponed_key_transports(account);
                    account.omemo.process_postponed_bundle_republish(account);
                }
                return true;
            }

            set__last = set ? xmpp_stanza_get_child_by_name(set, "last") : nullptr;
            const std::string set_last_text = stanza_element_text(set__last);
            
            if (auto channel_it = account.channels.find(mam_query.with.data()); channel_it != account.channels.end())
            {
                auto& [_, ch] = *channel_it;

                // Only page when the result set is not complete.
                // Some servers include <last/> even on the final page.
                if (!set_last_text.empty() && !fin_is_complete)
                {
                    account.mam_deferred_pages.push_back({
                        ch.id,
                        mam_query.id,
                        mam_query.start,
                        mam_query.end,
                        set_last_text
                    });
                    account.schedule_next_mam_page();
                }
                else
                {
                    // MAM fetch complete, update last fetch timestamp
                    ch.last_mam_fetch = time(nullptr);
                    account.mam_cache_set_last_timestamp(ch.id, ch.last_mam_fetch);
                    // Persist this PM JID so it can be restored on the next full restart
                    if (ch.type == weechat::channel::chat_type::PM)
                        account.pm_open_register(ch.id);
                    account.mam_query_remove(mam_query.id);
                    account.release_mam_slot();

                    // Print "History loaded" completion banner matching the fetch banner
                    // printed at the start of fetch_mam().
                    if (ch.buffer)
                    {
                        const std::string start_str = mam_query.start
                            ? format_local_timestamp(*mam_query.start)
                            : "the beginning";
                        const std::string end_str = mam_query.end
                            ? format_local_timestamp(*mam_query.end)
                            : "now";
                        weechat_printf_date_tags(ch.buffer, 0,
                                                 "xmpp_mam_fin,notify_none,no_log",
                                                 "%sHistory loaded: %s → %s",
                                                 weechat_prefix("network"),
                                                 start_str.c_str(), end_str.c_str());
                    }
                }
            }
            else if (is_global_query)
            {
                // Only page when the result set is not complete.
                if (!set_last_text.empty() && !fin_is_complete)
                {
                    // Persist the RSM cursor so the next reconnect resumes from here
                    account.mam_cursor_set("global", set_last_text);

                    // Defer the next page to the next event-loop tick so the GUI
                    // gets a chance to render between batches.
                    account.mam_query_remove(mam_query.id);
                    account.release_mam_slot();

                    account.mam_deferred_pages.push_back({
                        std::string{},                      // empty = global query
                        std::string{},
                        mam_query.start,
                        mam_query.end,
                        set_last_text
                    });
                    account.schedule_next_mam_page();
                }
                else
                {
                    // Global MAM query complete — persist the final RSM cursor so
                    // the next reconnect resumes from the very end of the archive
                    // rather than replaying from a stale intermediate cursor.
                    if (!set_last_text.empty())
                        account.mam_cursor_set("global", set_last_text);

                    account.mam_query_remove(mam_query.id);
                    account.release_mam_slot();
                        // MAM catchup done — fire deferred key transports now
                        account.omemo.global_mam_catchup = false;
                        account.omemo.process_postponed_key_transports(account);
                        account.omemo.process_postponed_bundle_republish(account);
                }
            }
            else
            {
                if (set_last_text.empty()) {
                    account.mam_query_remove(mam_query.id);
                    account.release_mam_slot();
                }
            }
        }
    }

    return true;
}
