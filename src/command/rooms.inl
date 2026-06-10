static void xep0433_send_search(weechat::account *account,
                                struct t_gui_buffer *buffer,
                                const char *service_jid,
                                const char *keywords,
                                weechat::ui::picker<std::string> *picker_ptr = nullptr)
{
    std::string search_id = stanza::uuid(account->context);

    weechat::account::channel_search_query_info info;
    info.service_jid = service_jid;
    info.keywords    = keywords ? keywords : "";
    info.buffer      = buffer;
    info.form_requested = true;
    info.picker      = picker_ptr;
    account->channel_search_queries[search_id] = info;

    // Build: <iq type='get' to='service' id='...'><search xmlns='urn:xmpp:channel-search:0:search'/></iq>
    // and let iq_handler submit the actual form query based on this response.
    struct channel_search_iq : stanza::spec {
        channel_search_iq(std::string_view to_, std::string_view id_) : spec("iq") {
            attr("type", "get");
            attr("to", to_);
            attr("id", id_);
            struct search_child : stanza::spec {
                search_child() : spec("search") {
                    xmlns<urn::xmpp::channel_search::_0>();
                }
            } s;
            child(s);
        }
    };

    channel_search_iq iq_s(service_jid, search_id);
    account->connection.send(iq_s.build(account->context).get());
}

int command__list(const void *pointer, void *data,
                  struct t_gui_buffer *buffer, int argc,
                  char **argv, char **argv_eol)
{
    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;

    (void) pointer;
    (void) data;
    (void) argv_eol;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    auto ui = weechat::UiPort::for_buffer(buffer);

    if (!ptr_account->connected())
    {
        ui->printf_error(fmt::format(fmt::runtime(_("{}: you are not connected to server")), WEECHAT_XMPP_PLUGIN_NAME));
        return WEECHAT_RC_OK;
    }

    // Determine service JID and keywords.
    // If argv[1] contains a dot (looks like a domain/JID), treat it as the service.
    // Otherwise treat all args as keywords and use the default public directory.
    const char *service_jid = "api@search.jabber.network";
    const char *keywords = "";

    if (argc >= 2)
    {
        // Heuristic: if the first arg contains a dot but no spaces, it's a JID/domain
        bool first_arg_is_jid = std::string_view(argv[1]).contains('.')
                                 && !std::string_view(argv[1]).contains(' ');
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

    // search.jabber.network exposes the XMPP API on api@search.jabber.network.
    if (weechat_strcasecmp(service_jid, "search.jabber.network") == 0)
        service_jid = "api@search.jabber.network";

    ui->printf("");
    if (keywords[0])
        ui->printf_network(fmt::format("Searching for MUC rooms matching \"{}\" via {} (XEP-0433)…", keywords, service_jid));
    else
        ui->printf_network(fmt::format("Searching for popular MUC rooms via {} (XEP-0433)…", service_jid));

    // Create an interactive picker that will be populated as results stream in.
    // On select: join the chosen room.
    using picker_t = weechat::ui::picker<std::string>;
    weechat::account *acct = ptr_account;

    std::string title_str = keywords[0]
        ? fmt::format("MUC room search: \"{}\"  (XEP-0433)  — select to join", keywords)
        : fmt::format("Popular MUC rooms via {}  (XEP-0433)  — select to join", service_jid);

    // Two-step init: create picker, then patch on_close_ after we have the pointer.
    // We use a shared_ptr<picker_t*> so the lambda can see the final pointer value
    // without capturing a stack reference that would dangle after command__list returns.
    auto p_holder = std::make_shared<picker_t *>(nullptr);
    auto p = std::make_unique<picker_t>(
        "xmpp.picker.list",
        title_str,
        std::vector<picker_t::entry>{},   // populated async as IQ results arrive
        [acct](std::string_view jid) {
            // on_select: switch focus to origin buffer and join the room
            std::string cmd = fmt::format("/join {}", jid);
            weechat_command(acct->buffer, cmd.c_str());
        },
        [acct, p_holder]() {
            // on_close: null out any pending channel_search_queries entries that
            // hold a dangling pointer to this picker (closed before IQ finished).
            picker_t *raw = *p_holder;
            for (auto &[id, info] : acct->channel_search_queries)
                if (info.picker == raw) info.picker = nullptr;
        },
        buffer);
    if (!*p) return WEECHAT_RC_ERROR;
    *p_holder = p.get();
    picker_t *raw_p = p.release();

    xep0433_send_search(ptr_account, buffer, service_jid,
                        keywords[0] ? keywords : nullptr, raw_p);

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
            std::array<char, 4096> path = {};
            std::span<char> path_span = path;
            if (fgets(path_span.data(), path_span.size(), fp))
            {
                pclose(fp);
                // Remove trailing newline
                size_t len = std::string_view(path_span.data()).size();
                if (len > 0 && path_span[len-1] == '\n')
                    path_span[len-1] = '\0';
                if (path_span[0])
                    return std::string(path_span.data());
            }
            pclose(fp);
        }
        
        // Try kdialog (KDE)
        fp = popen("kdialog --getopenfilename ~ 2>/dev/null", "r");
        if (fp)
        {
            std::array<char, 4096> path = {};
            std::span<char> path_span = path;
            if (fgets(path_span.data(), path_span.size(), fp))
            {
                pclose(fp);
                size_t len = std::string_view(path_span.data()).size();
                if (len > 0 && path_span[len-1] == '\n')
                    path_span[len-1] = '\0';
                if (path_span[0])
                    return std::string(path_span.data());
            }
            pclose(fp);
        }
    }
    
    // Try fzf in terminal (works everywhere)
    FILE *fp = popen("fzf --prompt='Select file to upload: ' --preview='file {}' 2>/dev/null", "r");
    if (fp)
    {
        std::array<char, 4096> path = {};
        if (fgets(path.data(), path.size(), fp))
        {
            pclose(fp);
            size_t len = std::string_view(path.data()).size();
            if (len > 0 && path[len-1] == '\n')
                path[len-1] = '\0';
            if (path[0])
                return std::string(path.data());
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

    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;
    
    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    auto ui = weechat::UiPort::for_buffer(buffer);

    if (!ptr_channel)
    {
        ui->printf_error(fmt::format("{}: command \"upload\" must be executed in an xmpp buffer", WEECHAT_XMPP_PLUGIN_NAME));
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        ui->printf_error(fmt::format("{}: you are not connected to server", WEECHAT_XMPP_PLUGIN_NAME));
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
        ui->printf_network(fmt::format("Selected file: {}", filename.c_str()));
        }
        else
        {
        ui->printf_error(fmt::format("{}: no file selected. Usage: /upload <filename>", WEECHAT_XMPP_PLUGIN_NAME));
        ui->printf_error("Note: Install zenity, kdialog, or fzf for interactive file picker");
            return WEECHAT_RC_OK;
        }
    }
    else
    {
        filename = argv[1];
        // Expand leading '~' to the user's home directory.
        if (!filename.empty() && filename[0] == '~')
        {
            const char *home = getenv("HOME");
            if (home)
                filename = std::string(home) + filename.substr(1);
        }
    }
    
    // Check if file exists and is readable
    FILE *file = fopen(filename.c_str(), "rb");
    if (!file)
    {
        ui->printf_error(fmt::format("{}: cannot open file: {}", WEECHAT_XMPP_PLUGIN_NAME, filename.c_str()));
        return WEECHAT_RC_OK;
    }
    
    // Get file size
    fseek(file, 0, SEEK_END);
    long filesize = ftell(file);
    fclose(file);
    
    if (filesize <= 0)
    {
        ui->printf_error(fmt::format("{}: file is empty: {}", WEECHAT_XMPP_PLUGIN_NAME, filename.c_str()));
        return WEECHAT_RC_OK;
    }
    
    // Check against server max size if known
    if (ptr_account->upload_max_size > 0 && (size_t)filesize > ptr_account->upload_max_size)
    {
        ui->printf_error(fmt::format("{}: file too large (max: {} bytes, file: {} bytes)", WEECHAT_XMPP_PLUGIN_NAME, ptr_account->upload_max_size, filesize));
        return WEECHAT_RC_OK;
    }

    // Extract just the filename (no path) for metadata and BoB alt text.
    size_t last_slash = filename.find_last_of("/\\");
    std::string basename = (last_slash != std::string::npos)
        ? filename.substr(last_slash + 1)
        : filename;

    // Sanitize filename: allow alphanumeric, dots, dashes, and underscores.
    std::string sanitized_basename;
    std::ranges::for_each(basename, [&](char c) {
        if ((c >= 'a' && c <= 'z')
            || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9')
            || c == '.' || c == '-' || c == '_')
        {
            sanitized_basename += c;
        }
        else
        {
            sanitized_basename += '-';
        }
    });

    size_t pos = 0;
    while ((pos = sanitized_basename.find("--", pos)) != std::string::npos)
        sanitized_basename.erase(pos, 1);
    while (!sanitized_basename.empty() && sanitized_basename[0] == '-')
        sanitized_basename.erase(0, 1);
    while (!sanitized_basename.empty() && sanitized_basename.back() == '-')
        sanitized_basename.pop_back();

    std::string content_type = ::xmpp::content_type_from_upload_filename(sanitized_basename);
    const bool channel_omemo = ptr_account->omemo && ptr_channel->omemo.enabled;

    // XEP-0231: small plaintext images use BoB instead of HTTP upload (Movim interop).
    if (content_type.starts_with("image/")
        && ::xmpp::bob_payload_size_ok(static_cast<std::size_t>(filesize))
        && !channel_omemo)
    {
        std::vector<std::uint8_t> image_bytes(static_cast<std::size_t>(filesize));
        FILE *bob_file = fopen(filename.c_str(), "rb");
        if (!bob_file)
        {
        ui->printf_error(fmt::format("{}: cannot read image for BoB send: {}", WEECHAT_XMPP_PLUGIN_NAME, filename.c_str()));
            return WEECHAT_RC_OK;
        }
        const std::size_t read_n = fread(
            image_bytes.data(), 1, image_bytes.size(), bob_file);
        fclose(bob_file);
        if (read_n != image_bytes.size())
        {
        ui->printf_error(fmt::format("{}: failed to read image for BoB send", WEECHAT_XMPP_PLUGIN_NAME));
            return WEECHAT_RC_OK;
        }

        ui->printf_date_tags(0, "no_trigger,notify_none",
            fmt::format("{}Sending image via XEP-0231 BoB ({} bytes)...",
                        weechat_prefix("network"), filesize));
        ptr_channel->send_bob_image(
            ptr_channel->id, image_bytes, content_type, sanitized_basename);
        return WEECHAT_RC_OK;
    }
    
    // Check if we have discovered upload service
    if (ptr_account->upload_service.empty())
    {
        ui->printf_error(fmt::format("{}: upload service not discovered yet (try reconnecting)", WEECHAT_XMPP_PLUGIN_NAME));
        return WEECHAT_RC_OK;
    }
    
    ui->printf_date_tags(0, "no_trigger,notify_none",
                         fmt::format("Requesting upload slot for {} ({} bytes)...",
                                     filename, filesize));
    
    // Generate request ID
    std::string upload_id = stanza::uuid(ptr_account->context);
    const char *id = upload_id.c_str();

        ui->printf_network(fmt::format("Using sanitized filename: {}", sanitized_basename.c_str()));
    
    // Use the size from the early probe (printed in "Requesting..." and used for max check).
    // Snapshot happens immediately after this, so the snapshotted bytes (and thus uploaded
    // content + advertised meta) match the size the user saw, with minimal window for
    // external writers to mutate the source between probe and snapshot.
    size_t file_size = static_cast<size_t>(filesize);

    // XEP-0448: when the channel has OMEMO active, the actual upload will be
    // AES-256-GCM ciphertext = plaintext + 16-byte auth tag.  Request the slot
    // for the larger size so the server does not reject the PUT with HTTP 413.
    size_t slot_size = channel_omemo ? file_size + 16 : file_size;

    // Snapshot the exact bytes of the selected file to a private temp right now.
    // The async worker (launched after the IQ roundtrip) will open *this* temp
    // for size/hash/dims/upload. This guarantees the content uploaded (and the
    // SHA/size/dims we advertise in metadata) matches the file the user chose
    // at /upload time, even if the original path is overwritten, deleted, or
    // is itself a temp that changes between the command and the worker.
    // The temp is cleaned up in the worker after the PUT (plain or ESFS case).
    std::string src_path = filename;
    char src_tmpl[] = "/tmp/xepher-upload-XXXXXX";
    int src_fd = ::mkstemp(src_tmpl);
    if (src_fd < 0)
    {
        ui->printf_error(fmt::format("{}: failed to create temp snapshot for upload", WEECHAT_XMPP_PLUGIN_NAME));
        return WEECHAT_RC_OK;
    }
    std::string src_tmp_path = src_tmpl;
    {
        FILE *src_f = fopen(src_path.c_str(), "rb");
        if (!src_f)
        {
            ::close(src_fd);
            ::unlink(src_tmp_path.c_str());
        ui->printf_error(fmt::format("{}: cannot reopen selected file for snapshot: {}", WEECHAT_XMPP_PLUGIN_NAME, src_path.c_str()));
            return WEECHAT_RC_OK;
        }
        FILE *dst_f = fdopen(src_fd, "wb");
        if (!dst_f)
        {
            fclose(src_f);
            ::close(src_fd);
            ::unlink(src_tmp_path.c_str());
        ui->printf_error(fmt::format("{}: failed to fdopen upload temp", WEECHAT_XMPP_PLUGIN_NAME));
            return WEECHAT_RC_OK;
        }
        char cbuf[8192];
        size_t cn;
        while ((cn = fread(cbuf, 1, sizeof(cbuf), src_f)) > 0)
        {
            if (fwrite(cbuf, 1, cn, dst_f) != cn)
            {
                fclose(src_f);
                fclose(dst_f);
                ::unlink(src_tmp_path.c_str());
        ui->printf_error(fmt::format("{}: failed to write upload snapshot", WEECHAT_XMPP_PLUGIN_NAME));
                return WEECHAT_RC_OK;
            }
        }
        fclose(src_f);
        if (fflush(dst_f) != 0 || fclose(dst_f) != 0)
        {
            ::unlink(src_tmp_path.c_str());
        ui->printf_error(fmt::format("{}: failed to finalize upload snapshot", WEECHAT_XMPP_PLUGIN_NAME));
            return WEECHAT_RC_OK;
        }
    }

    // Store upload request with metadata for SIMS. Use the *snapshot* temp as
    // the filepath the worker will open (stable content). The sanitized name
    // is still used for the upload service filename attr.
    ptr_account->upload_requests[id] = {
        id, 
        src_tmp_path, 
        sanitized_basename, 
        ptr_channel->id,
        content_type,
        file_size,
        {},  // hashes will be calculated during upload
        (ptr_channel->type == weechat::channel::chat_type::MUC)
    };
    
    // Build upload slot request (XEP-0363 v1.2.0+)
    struct upload_request_iq : stanza::spec {
        upload_request_iq(std::string_view to_, std::string_view id_,
                          std::string_view filename_, std::string_view size_,
                          std::string_view content_type_) : spec("iq") {
            attr("type", "get");
            attr("to", to_);
            attr("id", id_);
            struct req : stanza::spec {
                req(std::string_view fn, std::string_view sz, std::string_view ct) : spec("request") {
                    xmlns<urn::xmpp::http::upload::_0>();
                    attr("filename", fn);
                    attr("size", sz);
                    if (!ct.empty())
                        attr("content-type", ct);
                }
                // XEP-0363 §5: purpose child (message/profile/ephemeral/permanent)
                req& purpose_message() {
                    struct p : stanza::spec {
                        p() : spec("message") { xmlns<urn::xmpp::http::upload_purpose::_0>(); }
                    };
                    child(p());
                    return *this;
                }
            } r(filename_, size_, content_type_);
            r.purpose_message();  // all current uploads are message attachments
            child(r);
        }
    };

    auto size_str = fmt::format("{}", slot_size);
    upload_request_iq upload_iq(ptr_account->upload_service, upload_id,
                                sanitized_basename, size_str, content_type);
    ptr_account->connection.send(upload_iq.build(ptr_account->context).get());
    // freed by upload_id (std::string)

    return WEECHAT_RC_OK;
}

