bool weechat::connection::handle_pubsub_feed_iq_event(xmpp_stanza_t *stanza)
{
    const char *id = xmpp_stanza_get_id(stanza);
    const char *type = xmpp_stanza_get_attribute(stanza, "type");
    bool handled = false;

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
            its.item(stanza::xep0060::item().id(pub_item_id));
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
        // XEP-0060: PubSub feed item-fetch result
        // Arrives when we sent an IQ get for items after a <retract> event.
        {
            xmpp_stanza_t *pubsub_feed = xmpp_stanza_get_child_by_name_and_ns(
                stanza, "pubsub", "http://jabber.org/protocol/pubsub");
            if (pubsub_feed && id && type && weechat_strcasecmp(type, "result") == 0)
            {
                // Clean up successful publish tracking and trigger a re-fetch so the
                // comments/blog buffer updates immediately (server echoed <publish> back).
                trigger_publish_refetch(id);
    
                if (auto fetch_it = account.pubsub_fetch_ids.find(id); fetch_it != account.pubsub_fetch_ids.end())
                {
                    auto& [_, fi] = *fetch_it;
                    std::string feed_service    = fi.service;
                    std::string node_name       = fi.node;
                    std::string before_cursor   = fi.before_cursor;
                    int         max_items_req   = fi.max_items;
                    std::string feed_key = fmt::format("{}/{}", feed_service, node_name);
                    account.pubsub_fetch_ids.erase(fetch_it);
                    handled = true;

                    if (!account.feed_is_open(feed_key))
                        return handled;

                    auto [ch_it, inserted] = account.channels.try_emplace(
                        feed_key,
                        account,
                        weechat::channel::chat_type::FEED,
                        feed_key,
                        feed_key);
                    if (inserted)
                        account.feed_open_register(feed_key);
                    {
                        auto& [_, feed_ch] = *ch_it;
    
                        // XEP-0059 RSM: read <set> metadata BEFORE rendering items so we
                        // can detect a stale oldest-page result and skip rendering it.
                        // The <set> is a child of <pubsub>, not <items>.
                        bool stale_page = false;
                        int  rsm_total_count = -1;
                        {
                            xmpp_stanza_t *rsm_set = xmpp_stanza_get_child_by_name_and_ns(
                                pubsub_feed, "set", "http://jabber.org/protocol/rsm");
                            if (rsm_set && max_items_req > 0)
                            {
                                xmpp_stanza_t *count_el  = xmpp_stanza_get_child_by_name(rsm_set, "count");
                                const std::string count_text = stanza_element_text(count_el);
                                if (auto n = parse_int64(count_text); n)
                                    rsm_total_count = static_cast<int>(*n);
                                else
                                    rsm_total_count = -1;
    
                                xmpp_stanza_t *first_el   = xmpp_stanza_get_child_by_name(rsm_set, "first");
                                const std::string first_text = stanza_element_text(first_el);
    
                                // Detect stale page: the server returned the oldest-first page
                                // (index=0) even though there are more items than max_items.
                                // Heuristic: if the first item's ID parses as a timestamp older
                                // than 30 days AND the node has more items than we requested,
                                // this is a stale result (e.g. gadgeteerza-tech-blog on
                                // news.movim.eu which sorts oldest-first).
                                if (!first_text.empty() && rsm_total_count > max_items_req)
                                {
                                    // Try to parse the item-id as an ISO 8601 timestamp.
                                    // news.movim.eu uses timestamps as item IDs.
                                    struct tm tm_first = {};
                                    bool parsed = false;
                                    // Try full timestamp with fractional seconds: 2023-11-09T10:52:18.590034Z
                                    // strptime doesn't handle sub-seconds; truncate at '.'.
                                    std::string id_str = first_text;
                                    auto dot = id_str.find('.');
                                    if (dot != std::string::npos)
                                        id_str.resize(dot);
                                    // Also remove trailing Z if present after truncation
                                    if (!id_str.empty() && id_str.back() == 'Z')
                                        id_str.pop_back();
                                    if (strptime(id_str.c_str(), "%Y-%m-%dT%H:%M:%S", &tm_first) != nullptr)
                                    {
                                        tm_first.tm_isdst = -1;
                                        time_t t_first = mktime(&tm_first);
                                        time_t now = time(nullptr);
                                        // Treat as stale if oldest-in-page is more than 30 days old
                                        if (t_first > 0 && (now - t_first) > 30 * 24 * 3600)
                                            stale_page = true;
                                        parsed = true;
                                    }
                                    // If the item-id doesn't look like a timestamp, fall back:
                                    // treat as stale if index == 0 and count >> max_items
                                    // (conservative: only trigger if count is at least 10x max_items)
                                    if (!parsed)
                                    {
                                        const char *index_attr = first_el
                                            ? xmpp_stanza_get_attribute(first_el, "index")
                                            : nullptr;
                                        const int first_index = index_attr
                                            ? static_cast<int>(parse_int64(index_attr).value_or(-1))
                                            : -1;
                                        if (first_index == 0 && rsm_total_count > max_items_req * 10)
                                            stale_page = true;
                                    }
                                }
    
                                if (stale_page)
                                {
                                    // Discard this response silently and re-fetch from the end
                                    // of the node using RSM <index>. Structure:
                                    //   <pubsub>
                                    //     <items node="..." max_items="N"/>
                                    //     <set xmlns="rsm"><max>N</max><index>count-N</index></set>
                                    //   </pubsub>
                                    int start_index = std::max(0, rsm_total_count - max_items_req);
    
                                    // Build <pubsub> with <items max_items=N> + RSM <set>
                                    std::string uid2 = stanza::uuid(account.context);
                                    stanza::xep0060::items its2(node_name);
                                    its2.max_items(max_items_req);
                                    stanza::xep0059::set rset2;
                                    rset2.max(max_items_req).index(start_index);
                                    stanza::xep0060::pubsub ps2;
                                    ps2.items(its2).rsm(rset2);
                                    account.pubsub_fetch_ids[uid2] = {feed_service, node_name, {}, max_items_req};
                                    account.connection.send(stanza::iq()
                                        .from(account.jid())
                                        .to(feed_service)
                                        .type("get")
                                        .id(uid2)
                                        .xep0060()
                                        .pubsub(ps2)
                                        .build(account.context)
                                        .get());
                                }
    
                            }
                        }
    
                        xmpp_stanza_t *items = xmpp_stanza_get_child_by_name(pubsub_feed, "items");
                        if (items && !stale_page)
                        {
                            // Collect items and sort oldest-first by Atom <published>
                            // (falling back to <updated>). ISO 8601 strings are
                            // zero-padded and sort correctly as strings, so no
                            // strptime needed. This is server-order-independent:
                            // it doesn't matter whether the server returns
                            // oldest-first or newest-first.
                            auto item_pubdate = [](xmpp_stanza_t *item) -> std::string {
                                xmpp_stanza_t *entry =
                                    xmpp_stanza_get_child_by_name_and_ns(
                                        item, "entry", "http://www.w3.org/2005/Atom");
                                if (!entry)
                                    entry = xmpp_stanza_get_child_by_name(item, "entry");
                                if (!entry) return {};
                                for (const char *tag : {"published", "updated"}) {
                                    xmpp_stanza_t *el =
                                        xmpp_stanza_get_child_by_name(entry, tag);
                                    if (!el) continue;
                                    const std::string s = stanza_element_text(el);
                                    if (!s.empty()) return s;
                                }
                                return {};
                            };
                            std::vector<xmpp_stanza_t *> item_vec;
                            for (xmpp_stanza_t *item = xmpp_stanza_get_children(items);
                                 item; item = xmpp_stanza_get_next(item))
                                item_vec.push_back(item);
                            std::ranges::stable_sort(item_vec,
                                [&item_pubdate](xmpp_stanza_t *a, xmpp_stanza_t *b) {
                                    return item_pubdate(a) < item_pubdate(b);
                                });
                            for (xmpp_stanza_t *item : item_vec)
                            {
                                if (!item || weechat_strcasecmp(xmpp_stanza_get_name(item), "item") != 0)
                                    continue;
    
                                const char *item_id_raw = xmpp_stanza_get_id(item);
    
                                // Skip non-Atom items stored in the node (e.g. avatar metadata
                                // published by Prosody into the blog node).  These are not
                                // microblog posts and have no <entry xmlns="…Atom"> child.
                                // Check by looking for a well-known non-post item id namespace.
                                if (item_id_raw
                                    && ::xmpp::is_skipped_non_atom_feed_item_id(item_id_raw))
                                    continue;
    
                                xmpp_stanza_t *entry = xmpp_stanza_get_child_by_name_and_ns(
                                    item, "entry", "http://www.w3.org/2005/Atom");
                                if (!entry)
                                    entry = xmpp_stanza_get_child_by_name(item, "entry");
    
                                xmpp_stanza_t *feed = xmpp_stanza_get_child_by_name_and_ns(
                                    item, "feed", "http://www.w3.org/2005/Atom");
                                if (!feed)
                                    feed = xmpp_stanza_get_child_by_name(item, "feed");
    
                                if (!entry && feed)
                                {
                                    atom_feed af = parse_atom_feed(account.context, feed);
                                    if (!af.empty())
                                    {
                                        if (!af.title.empty())
                                        {
                                            feed_ch.update_name(af.title.c_str());
                                            weechat::UiPort::for_buffer(feed_ch.buffer)->printf_date_tags(
                                                0, "xmpp_feed,notify_none",
                                                fmt::format("{}Feed title:{} {}",
                                                    weechat_prefix("network"),
                                                    weechat_color("reset"),
                                                    af.title));
                                        }
                                        if (!af.author.empty())
                                            weechat::UiPort::for_buffer(feed_ch.buffer)->printf_date_tags(
                                                0, "xmpp_feed,notify_none",
                                                fmt::format("  {}Author:{} {}",
                                                    weechat_color("darkgray"),
                                                    weechat_color("reset"),
                                                    af.author));
                                        if (!af.subtitle.empty())
                                            weechat::UiPort::for_buffer(feed_ch.buffer)->printf_date_tags(
                                                0, "xmpp_feed,notify_none",
                                                fmt::format("  {}", af.subtitle));
                                    }
                                    continue;
                                }
    
                                const char *publisher = xmpp_stanza_get_attribute(item, "publisher");
                                atom_entry ae = parse_atom_entry(account.context, entry, publisher);
                                if (item_id_raw && !ae.item_id.empty())
                                    account.feed_atom_id_set(feed_key, item_id_raw, ae.item_id);
                                if (item_id_raw && !ae.replies_link.empty())
                                    account.feed_replies_link_set(feed_key, item_id_raw, ae.replies_link);
    
                                // Assign a short #N alias for this item so the user can write
                                // "/feed reply #3 …" instead of the full service/node/item-id.
                                int item_alias = -1;
                                if (item_id_raw && *item_id_raw)
                                    item_alias = account.feed_alias_assign(feed_key, item_id_raw);
    
                                if (ae.empty())
                                    continue;

                                ::xmpp::render_atom_entry_to_feed(
                                    feed_ch,
                                    account,
                                    feed_key,
                                    feed_service,
                                    node_name,
                                    item_id_raw ? std::string_view(item_id_raw) : std::string_view{},
                                    item_alias,
                                    ae);

                                // Mark rendered so push duplicates and re-fetches are suppressed.
                                if (item_id_raw)
                                    account.feed_item_mark_seen(feed_key, item_id_raw);
                            }
                        }
    
                        // XEP-0059 RSM paging hint (only shown for non-stale pages).
                        // rsm_total_count was already populated in the pre-check above.
                        if (!stale_page && max_items_req > 0)
                        {
                            xmpp_stanza_t *rsm_set2 = xmpp_stanza_get_child_by_name_and_ns(
                                pubsub_feed, "set", "http://jabber.org/protocol/rsm");
                            if (rsm_set2)
                            {
                                xmpp_stanza_t *first_el2 = xmpp_stanza_get_child_by_name(rsm_set2, "first");
                                const std::string first_text2 = stanza_element_text(first_el2);
                                if (!first_text2.empty())
                                {
                                    std::string hint = fmt::format(
                                        "/feed {} {} --before {}",
                                        feed_service, node_name, first_text2);
                                    if (rsm_total_count > 0)
                                        weechat::UiPort::for_buffer(feed_ch.buffer)->printf_date_tags(
                                            0, "xmpp_feed,notify_none",
                                            fmt::format("{}{} item(s) total — for older entries: {}",
                                                weechat_prefix("network"),
                                                rsm_total_count, hint));
                                    else
                                        weechat::UiPort::for_buffer(feed_ch.buffer)->printf_date_tags(
                                            0, "xmpp_feed,notify_none",
                                            fmt::format("{}For older entries: {}",
                                                weechat_prefix("network"), hint));
                                }
                            }
                        }
                        (void)before_cursor; // used at send time; not needed in result handler
                    }
                }
            }
        }
    
        // XEP-0060: PubSub subscriptions result — /feed <service> (default, no --all)
        {
            xmpp_stanza_t *pubsub_subs = xmpp_stanza_get_child_by_name_and_ns(
                stanza, "pubsub", "http://jabber.org/protocol/pubsub");
            if (pubsub_subs && id && type && weechat_strcasecmp(type, "result") == 0)
            {
                if (auto subs_it = account.pubsub_subscriptions_queries.find(id); subs_it != account.pubsub_subscriptions_queries.end())
                {
                    const std::string feed_service = subs_it->second;
                    account.pubsub_subscriptions_queries.erase(subs_it);
                    handled = true;

                    xmpp_stanza_t *subscriptions = xmpp_stanza_get_child_by_name(pubsub_subs, "subscriptions");
                    int node_count = 0;
    
                    if (subscriptions)
                    {
                        for (xmpp_stanza_t *sub = xmpp_stanza_get_children(subscriptions);
                             sub; sub = xmpp_stanza_get_next(sub))
                        {
                            const char *sub_name = xmpp_stanza_get_name(sub);
                            if (!sub_name || weechat_strcasecmp(sub_name, "subscription") != 0)
                                continue;
    
                            const char *sub_state = xmpp_stanza_get_attribute(sub, "subscription");
                            const char *node_attr = xmpp_stanza_get_attribute(sub, "node");
    
                            if (!node_attr || !sub_state
                                || weechat_strcasecmp(sub_state, "subscribed") != 0)
                                continue;
    
                            std::string node_name(node_attr);
    
                            if (::xmpp::is_microblog_comments_node(node_name))
                                continue;
    
                            std::string feed_key = fmt::format("{}/{}", feed_service, node_name);
    
                            // Ensure FEED buffer exists
                            auto [sub_ch_it, sub_inserted] = account.channels.try_emplace(
                                feed_key,
                                account,
                                weechat::channel::chat_type::FEED,
                                feed_key,
                                feed_key);
                            (void)sub_inserted;
                            account.feed_open_register(feed_key);
    
                            // Fetch items for this subscribed node using only max_items (XEP-0060 §6.5.7).
                            // RSM <before/> is omitted: news.movim.eu ignores empty <before/> last-page
                            // semantics and returns the oldest page. Plain max_items returns the most
                            // recently published items on compliant servers.
                            std::string uid = stanza::uuid(account.context);
                            stanza::xep0060::items sub_its(node_name);
                            sub_its.max_items(20);
                            stanza::xep0060::pubsub sub_ps;
                            sub_ps.items(sub_its);
                            account.pubsub_fetch_ids[uid] = {feed_service, node_name, "", 20};
                            account.connection.send(stanza::iq()
                                .from(account.jid())
                                .to(feed_service)
                                .type("get")
                                .id(uid)
                                .xep0060()
                                .pubsub(sub_ps)
                                .build(account.context)
                                .get());
                            node_count++;
                        }
                    }
    
                    if (node_count == 0)
                        weechat::UiPort::for_buffer(account.buffer)->printf_network(fmt::format(
                            "No subscribed feeds found on {}. "
                            "Try: /feed {} --all",
                            feed_service, feed_service));
                    else
                        weechat::UiPort::for_buffer(account.buffer)->printf_network(fmt::format(
                            "Feed subscriptions on {}: fetching {} subscribed node(s)",
                            feed_service, node_count));
                }
            }
        }
    
        // XEP-0060: PubSub subscribe/unsubscribe result
        {
            xmpp_stanza_t *pubsub_res = xmpp_stanza_get_child_by_name_and_ns(
                stanza, "pubsub", "http://jabber.org/protocol/pubsub");
            if (pubsub_res && id && type && weechat_strcasecmp(type, "result") == 0)
            {
                if (auto sub_ok_it = account.pubsub_subscribe_queries.find(id); sub_ok_it != account.pubsub_subscribe_queries.end())
                {
                    auto& [_, sub] = *sub_ok_it;
                    struct t_gui_buffer *fb = sub.buffer ? sub.buffer : account.buffer;
                    weechat::UiPort::for_buffer(fb)->printf_network(fmt::format(
                        "Subscribed to {}", sub.feed_key));
                    account.pubsub_subscribe_queries.erase(sub_ok_it);
                    handled = true;
                }

                if (auto unsub_ok_it = account.pubsub_unsubscribe_queries.find(id); unsub_ok_it != account.pubsub_unsubscribe_queries.end())
                {
                    auto& [_, unsub] = *unsub_ok_it;
                    struct t_gui_buffer *fb = unsub.buffer ? unsub.buffer : account.buffer;
                    weechat::UiPort::for_buffer(fb)->printf_network(fmt::format(
                        "Unsubscribed from {}", unsub.feed_key));
                    account.pubsub_unsubscribe_queries.erase(unsub_ok_it);
                    handled = true;
                }
            }
        }
    return handled;
}
