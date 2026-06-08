bool weechat::connection::handle_upload_slot_iq_event(xmpp_stanza_t *stanza)
{
    const char *id = xmpp_stanza_get_id(stanza);
    const char *type = xmpp_stanza_get_attribute(stanza, "type");
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
                        if (::xmpp::is_allowed_http_upload_put_header(name))
                        {
                            const std::string safe_value =
                                ::xmpp::sanitize_http_header_value(value);
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
                    return true;
                }
                XDEBUG("Upload: file verified, filename='{}'", req.filename);
                
                const std::string content_type =
                    ::xmpp::content_type_from_upload_filename(req.filename);
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
                    return true;
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
    
    return false;
}

bool weechat::connection::handle_upload_slot_iq_error(xmpp_stanza_t *stanza)
{
    const char *id = xmpp_stanza_get_id(stanza);
    if (!id)
        return false;

    auto req_it = account.upload_requests.find(id);
    if (req_it == account.upload_requests.end())
        return false;

    xmpp_stanza_t *error_elem = xmpp_stanza_get_child_by_name(stanza, "error");
    const char *error_type = error_elem
        ? xmpp_stanza_get_attribute(error_elem, "type") : nullptr;
    const std::string error_msg = ::xmpp::format_upload_slot_error_message(
        ::xmpp::StanzaView(error_elem));

    weechat_printf(account.buffer, "%s%s: %s (type: %s)",
                   weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                   error_msg.c_str(), error_type ? error_type : "unknown");

    account.upload_requests.erase(req_it);
    return true;
}
