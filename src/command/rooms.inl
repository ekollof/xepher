static void xep0433_send_search(weechat::account *account,
                                struct t_gui_buffer *buffer,
                                const char *service_jid,
                                const char *keywords,
                                weechat::ui::picker<std::string> *picker_ptr = nullptr)
{
    xmpp_string_guard search_id_g(account->context, xmpp_uuid_gen(account->context));
    const char *search_id = search_id_g.ptr;

    weechat::account::channel_search_query_info info;
    info.service_jid = service_jid;
    info.keywords    = keywords ? keywords : "";
    info.buffer      = buffer;
    info.form_requested = true;
    info.picker      = picker_ptr;
    account->channel_search_queries[search_id] = info;

    // Build: <iq type='get' to='service' id='...'><search xmlns='urn:xmpp:channel-search:0:search'/></iq>
    // and let iq_handler submit the actual form query based on this response.
    xmpp_stanza_t *iq = xmpp_iq_new(account->context, "get", search_id);
    xmpp_stanza_set_to(iq, service_jid);

    xmpp_stanza_t *search_el = xmpp_stanza_new(account->context);
    xmpp_stanza_set_name(search_el, "search");
    xmpp_stanza_set_ns(search_el, "urn:xmpp:channel-search:0:search");
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
        bool first_arg_is_jid = (strchr(argv[1], '.') != nullptr)
                                 && (strchr(argv[1], ' ') == nullptr);
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
    auto *p = new picker_t(
        "xmpp.picker.list",
        title_str,
        {},   // populated async as IQ results arrive
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
    *p_holder = p;
    (void) p;

    xep0433_send_search(ptr_account, buffer, service_jid,
                        keywords[0] ? keywords : nullptr, p);

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
    
    auto size_str = fmt::format("{}", file_size);
    xmpp_stanza_set_attribute(request, "size", size_str.c_str());
    
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

