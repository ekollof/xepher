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

    weechat_printf(buffer, "");
    if (keywords[0])
        weechat_printf(buffer, "%sSearching for MUC rooms matching \"%s\" via %s (XEP-0433)…",
                      weechat_prefix("network"), keywords, service_jid);
    else
        weechat_printf(buffer, "%sSearching for popular MUC rooms via %s (XEP-0433)…",
                      weechat_prefix("network"), service_jid);

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
        [acct](const std::string &jid) {
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
            if (fgets(path.data(), path.size(), fp))
            {
                pclose(fp);
                // Remove trailing newline
                size_t len = std::string_view(path.data()).size();
                if (len > 0 && path[len-1] == '\n')
                    path[len-1] = '\0';
                if (path[0])
                    return std::string(path.data());
            }
            pclose(fp);
        }
        
        // Try kdialog (KDE)
        fp = popen("kdialog --getopenfilename ~ 2>/dev/null", "r");
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
    std::string upload_id = stanza::uuid(ptr_account->context);
    const char *id = upload_id.c_str();
    
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

    // XEP-0448: when the channel has OMEMO active, the actual upload will be
    // AES-256-GCM ciphertext = plaintext + 16-byte auth tag.  Request the slot
    // for the larger size so the server does not reject the PUT with HTTP 413.
    bool channel_omemo = ptr_account->omemo && ptr_channel->omemo.enabled;
    size_t slot_size = channel_omemo ? file_size + 16 : file_size;

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
            } r(filename_, size_, content_type_);
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

