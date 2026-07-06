void weechat::connection::request_server_disco_probes(bool resumed_session)
{
    if (resumed_session || account.optional_server_probes_done)
        return;

    ::jid parsed(nullptr, std::string(account.option_jid.string()));
    if (parsed.domain.empty())
    {
        run_optional_server_probes(resumed_session, {});
        return;
    }

    const std::string disco_id = stanza::uuid(account.context);
    account.pending_server_disco_id = disco_id;

    this->send(stanza::iq()
                   .from(account.jid())
                   .to(parsed.domain)
                   .type("get")
                   .id(disco_id)
                   .xep0030()
                   .query()
                   .build(account.context)
                   .get());
}

void weechat::connection::run_optional_server_probes(
    bool resumed_session,
    std::span<const std::string> server_features)
{
    if (resumed_session || account.optional_server_probes_done)
        return;

    account.optional_server_probes_done = true;
    account.pending_server_disco_id.reset();

    if (server_features.empty())
    {
        XDEBUG("Skipping optional server probes — server disco returned no features");
        return;
    }

    account.peer_features_update(
        ::jid(nullptr, std::string(account.option_jid.string())).domain,
        std::vector<std::string>(server_features.begin(), server_features.end()));

    const auto has = [&](std::string_view feature) {
        return disco_features_contain(server_features, feature);
    };

    if (has("urn:xmpp:carbons:2"))
    {
        const std::string carbons_enable_id = stanza::uuid(account.context);
        account.pending_carbons_enable_iq_ = carbons_enable_id;
        this->send(stanza::iq()
                       .from(account.jid())
                       .type("set")
                       .id(carbons_enable_id)
                       .xep0280()
                       .enable()
                       .build(account.context)
                       .get());
    }
    else
    {
        XDEBUG("Message carbons not advertised by server — skipping enable");
    }

    if (has("urn:xmpp:bookmarks:1"))
    {
        stanza::xep0060::items bm_items("urn:xmpp:bookmarks:1");
        stanza::xep0060::pubsub bm_ps;
        bm_ps.items(bm_items);
        this->send(stanza::iq()
                       .from(account.jid())
                       .to(account.jid())
                       .type("get")
                       .id(stanza::uuid(account.context))
                       .xep0060()
                       .pubsub(bm_ps)
                       .build(account.context)
                       .get());
    }
    else if (has("storage:bookmarks"))
    {
        this->send(stanza::iq()
                       .from(account.jid())
                       .to(account.jid())
                       .type("get")
                       .id(stanza::uuid(account.context))
                       .xep0049()
                       .query(stanza::xep0049::query().bookmarks())
                       .build(account.context)
                       .get());
    }
    else
    {
        XDEBUG("Bookmarks not advertised by server — skipping fetch");
    }

    if (has("urn:xmpp:mds:displayed:0"))
    {
        const std::string mds_fetch_id = stanza::uuid(account.context);
        account.pending_mds_fetch_iq_ = mds_fetch_id;
        stanza::xep0060::items mds_items("urn:xmpp:mds:displayed:0");
        stanza::xep0060::pubsub mds_ps;
        mds_ps.items(mds_items);
        this->send(stanza::iq()
                       .from(account.jid())
                       .to(account.jid())
                       .type("get")
                       .id(mds_fetch_id)
                       .xep0060()
                       .pubsub(mds_ps)
                       .build(account.context)
                       .get());
    }
    else
    {
        XDEBUG("Message Display Status not advertised by server — skipping fetch");
    }

    if (has("urn:xmpp:mam:2"))
    {
        time_t now = time(nullptr);
        time_t fetch_days = weechat::config::instance
            ? static_cast<time_t>(weechat::config::instance->look.mam_fetch_days.integer())
            : 3;
        time_t start = now - (fetch_days * 86400);
        std::string global_mam_cursor = account.mam_cursor_get("global");
        const bool has_cursor = !global_mam_cursor.empty();

        std::string global_mam_id = stanza::uuid(account.context);
        account.add_mam_query(global_mam_id.c_str(), "",
                            has_cursor ? std::optional<time_t>{} : std::optional<time_t>(start),
                            std::optional<time_t>(now));
        account.omemo.global_mam_catchup = true;

        stanza::xep0059::set rsm_set;
        rsm_set.max(50);

        stanza::xep0313::query mam_query;
        if (!has_cursor)
        {
            stanza::xep0313::x_filter xf;
            xf.start(format_utc_timestamp(start));
            mam_query.filter(xf).rsm(rsm_set);
        }
        else
        {
            rsm_set.after(global_mam_cursor);
            mam_query.rsm(rsm_set);
        }

        this->send(stanza::iq()
                       .type("set")
                       .id(global_mam_id)
                       .xep0313()
                       .query(mam_query)
                       .build(account.context)
                       .get());
    }
    else
    {
        XDEBUG("MAM not advertised by server — skipping global catchup");
    }
}