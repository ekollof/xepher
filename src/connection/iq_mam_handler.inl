void weechat::connection::handle_mam_query_iq_error(xmpp_stanza_t *stanza)
{
    const char *id = xmpp_stanza_get_id(stanza);
    const char *type = xmpp_stanza_get_attribute(stanza, "type");
    if (!id || !type || weechat_strcasecmp(type, "error") != 0)
        return;

    weechat::account::mam_query failed_mam_query;
    if (!account.mam_query_search(&failed_mam_query, id))
        return;

    const bool is_global_query = failed_mam_query.with.empty();
    bool recovered = false;

    if (is_global_query && ::xmpp::iq_has_item_not_found_error(::xmpp::StanzaView(stanza)))
    {
        weechat_printf(account.buffer,
                       "%sGlobal MAM cursor stale (item-not-found) — "
                       "clearing cursor and retrying with time-based query",
                       weechat_prefix("network"));
        account.mam_cursor_clear("global");
        account.mam_query_remove(failed_mam_query.id);
        account.release_mam_slot();

        const time_t now = time(nullptr);
        const time_t fetch_days = weechat::config::instance
            ? static_cast<time_t>(weechat::config::instance->look.mam_fetch_days.integer())
            : 3;
        const time_t start = now - (fetch_days * 86400);
        const std::string retry_id = stanza::uuid(account.context);
        account.add_mam_query(retry_id.c_str(), "",
                              std::optional<time_t>(start),
                              std::optional<time_t>(now));

        stanza::xep0059::set rsm_set;
        rsm_set.max(50);

        stanza::xep0313::query retry_q;
        stanza::xep0313::x_filter xf;
        xf.start(fmt::format("{:%Y-%m-%dT%H:%M:%SZ}", fmt::gmtime(start)));
        retry_q.filter(xf).rsm(rsm_set);

        send(stanza::iq()
            .type("set")
            .id(retry_id)
            .xep0313()
            .query(retry_q)
            .build(account.context)
            .get());

        recovered = true;
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

bool weechat::connection::handle_mam_fin_iq_event(xmpp_stanza_t *stanza)
{
    const char *id = xmpp_stanza_get_id(stanza);
    const char *type = xmpp_stanza_get_attribute(stanza, "type");

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
                const std::string svc_jid   = pq.service;
                const std::string node_name = pq.node;
                account.pubsub_mam_queries.erase(pq_it);

                const std::string feed_key = fmt::format("{}/{}", svc_jid, node_name);
                const std::string plast_text = ::xmpp::mam_fin_rsm_last(::xmpp::StanzaView(pmam_fin));
                if (!plast_text.empty())
                {
                    account.mam_cursor_set(
                        fmt::format("pubsub:{}", feed_key),
                        plast_text);
                }
                return true;
            }
        }
    }

    xmpp_stanza_t *fin = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "fin", "urn:xmpp:mam:2");
    if (!fin)
        return false;

    weechat::account::mam_query mam_query;

    const ::xmpp::StanzaView fin_view(fin);
    const auto fin_complete = fin_view.attr("complete");
    const auto fin_stable = fin_view.attr("stable");
    const auto fin_abort = fin_view.attr("abort");
    const bool fin_is_complete = fin_complete
        && ::xmpp::is_mam_fin_bool_attr_true(*fin_complete);
    const bool fin_has_abort = fin_abort
        && ::xmpp::is_mam_fin_bool_attr_true(*fin_abort);

    if (!id || !account.mam_query_search(&mam_query, id))
        return false;

    const bool is_global_query = mam_query.with.empty();

    if (fin_has_abort)
    {
        weechat_printf(account.buffer,
                       "%sMAM query %s aborted by server (complete=%s stable=%s)",
                       weechat_prefix("error"),
                       mam_query.id.c_str(),
                       fin_complete ? std::string(*fin_complete).c_str() : "(unset)",
                       fin_stable ? std::string(*fin_stable).c_str() : "(unset)");
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

    const std::string set_last_text = ::xmpp::mam_fin_rsm_last(fin_view);

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
        if (set_last_text.empty())
        {
            account.mam_query_remove(mam_query.id);
            account.release_mam_slot();
        }
    }

    return false;
}