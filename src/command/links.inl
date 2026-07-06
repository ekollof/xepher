// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// /links [fetch]
//
// Both forms do the same thing:
//   1. Walk all buffer lines, collect unique http(s):// URLs oldest-first.
//   2. Print a formatted list: timestamp, URL, OG title (from LMDB if cached).
//   3. Queue silent background fetches for any URL not yet in LMDB cache.
//   4. Print a one-line footer: "N fetches queued — re-run /links to see titles"
//
// /links fetch is kept as an alias for discoverability.

// ── helpers ───────────────────────────────────────────────────────────────────

// Scan a message string for http:// or https:// URLs and append them to `out`.
// Uses the same algorithm as message_handler.inl.
static void scan_urls(std::string_view msg, std::vector<std::string> &out)
{
    size_t pos = 0;
    while (pos < msg.size())
    {
        size_t found = msg.find("http", pos);
        if (found == std::string_view::npos)
            break;
        std::string_view rest = msg.substr(found);
        if (!rest.starts_with("http://") && !rest.starts_with("https://"))
        {
            pos = found + 4;
            continue;
        }
        size_t end = found;
        while (end < msg.size() && !std::isspace((unsigned char)msg[end]))
            ++end;
        std::string url(msg.substr(found, end - found));
        strip_url_trailing_punct(url);
        if (!url.empty())
            out.emplace_back(std::move(url));
        pos = end;
    }
}

// ── /links ────────────────────────────────────────────────────────────────────

int command__links(COMMAND_ARGS)
{
    (void) pointer;
    (void) data;
    (void) argv;
    (void) argv_eol;
    (void) argc;

    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;
    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account || !ptr_channel)
        return WEECHAT_RC_ERROR;

    auto ui = weechat::UiPort::for_buffer(buffer);

    // ── Walk all buffer lines oldest-first, collect unique URLs ───────────────

    struct url_entry {
        std::string url;
        time_t      date = 0;
    };

    std::vector<url_entry>          urls;
    std::unordered_set<std::string> seen;

    struct t_hdata *hd_line      = weechat_hdata_get("line");
    struct t_hdata *hd_line_data = weechat_hdata_get("line_data");
    struct t_hdata *hd_lines     = weechat_hdata_get("lines");
    struct t_hdata *hd_buffer    = weechat_hdata_get("buffer");

    void *lines = weechat_hdata_pointer(hd_buffer, buffer, "lines");
    if (!lines)
        return WEECHAT_RC_OK;

    void *cur = weechat_hdata_pointer(hd_lines, lines, "first_line");

    while (cur)
    {
        void *ld = weechat_hdata_pointer(hd_line, cur, "data");
        if (ld)
        {
            // Skip xmpp_og_preview lines — they are decorative, not messages.
            bool is_og = false;
            int tc = weechat_hdata_integer(hd_line_data, ld, "tags_count");
            for (int n = 0; n < tc && !is_og; ++n)
            {
                std::string key = fmt::format("{}|tags_array", n);
                const char *tag = weechat_hdata_string(hd_line_data, ld, key.c_str());
                if (tag && std::string_view(tag) == "xmpp_og_preview")
                    is_og = true;
            }

            if (!is_og)
            {
                const char *msg = weechat_hdata_string(hd_line_data, ld, "message");
                time_t date    = weechat_hdata_time(hd_line_data, ld, "date");

                if (msg)
                {
                    // Strip WeeChat color/attribute codes before scanning for URLs.
                    // The rendered message string contains embedded escape sequences
                    // that would corrupt extracted URLs if not removed first.
                    std::unique_ptr<char, decltype(&free)>
                        plain(weechat_string_remove_color(msg, nullptr), &free);
                    std::string_view sv = plain ? plain.get() : msg;
                    std::vector<std::string> found_urls;
                    scan_urls(sv, found_urls);
                    for (auto &u : found_urls)
                    {
                        if (seen.insert(u).second)
                            urls.push_back({u, date});
                    }
                }
            }
        }
        cur = weechat_hdata_pointer(hd_line, cur, "next_line");
    }

    // ── Nothing found ─────────────────────────────────────────────────────────

    if (urls.empty())
    {
        ui->printf_network("xmpp/links: no URLs found in this buffer");
        return WEECHAT_RC_OK;
    }

    // Sort oldest-first so newest links appear last (chat-scroll convention).
    std::ranges::stable_sort(urls,
                     [](const url_entry &a, const url_entry &b) {
                         return a.date < b.date;
                     });

    // ── Print the list ────────────────────────────────────────────────────────

    const char *bold = weechat::RuntimePort::default_runtime().color("bold");
    const char *rst  = weechat::RuntimePort::default_runtime().color("reset");
    const char *dim  = weechat::RuntimePort::default_runtime().color("darkgray");
    const char *cyan = weechat::RuntimePort::default_runtime().color("cyan");

        ui->printf_network(fmt::format("xmpp/links: {} URL{} in this buffer:", urls.size(), urls.size() == 1 ? "" : "s"));

    for (auto &e : urls)
    {
        auto cached = ptr_account->og_cache_lookup(e.url);

        std::string time_str;
        if (e.date > 0)
        {
            struct tm tm_local = {};
            localtime_r(&e.date, &tm_local);
            time_str = fmt::format("{:02d}-{:02d} {:02d}:{:02d}",
                                   tm_local.tm_mon + 1, tm_local.tm_mday,
                                   tm_local.tm_hour, tm_local.tm_min);
        }

        if (cached && !cached->title.empty())
        {
        ui->printf_network(fmt::format("  {}{}{}  {}{}{}  {}{}{}", dim, time_str.c_str(), rst, cyan, e.url.c_str(), rst, bold, cached->title.c_str(), rst));
        }
        else
        {
        ui->printf_network(fmt::format("  {}{}{}  {}{}{}", dim, time_str.c_str(), rst, cyan, e.url.c_str(), rst));
        }
    }

    // ── Queue silent background fetches for uncached URLs ────────────────────

    int queued   = 0;
    int inflight = 0;

    for (auto &e : urls)
    {
        auto cached = ptr_account->og_cache_lookup(e.url);
        if (cached)
            continue; // already cached

        bool already_fetching =
            std::ranges::any_of(g_og_fetches,
                        [&](const og_fetch_ctx &ctx) { return ctx.url == e.url; }) ||
            std::ranges::any_of(g_og_pending,
                        [&](const og_pending_entry &p) { return p.url == e.url; });
        if (already_fetching)
        {
            ++inflight;
            continue;
        }

        // silent=true: cache-only, no preview line will be displayed
        og_start_fetch(e.url, buffer, ptr_account, "", 0, true);
        ++queued;
    }

    // Footer
    if (queued > 0 || inflight > 0)
    {
        std::string footer = fmt::format(
            "xmpp/links: {} fetch{} queued in background",
            queued, queued == 1 ? "" : "es");
        if (inflight > 0)
            footer += fmt::format(", {} already in-flight", inflight);
        footer += " \xe2\x80\x94 re-run /links to see updated titles";
        ui->printf_network(footer);
    }

    return WEECHAT_RC_OK;
}
