bool weechat::connection::handle_pubsub_feed_iq_event(xmpp_stanza_t *stanza)
{
    const ::xmpp::StanzaView view(stanza);
    const std::string iq_id_str = view.attr_string("id");
    const std::string iq_from_str = view.attr_string("from");
    const std::string iq_to_str = view.attr_string("to");
    const std::string iq_type_str = view.attr_string("type");
    const char *id = iq_id_str.empty() ? nullptr : iq_id_str.c_str();
    const char *from = iq_from_str.empty() ? nullptr : iq_from_str.c_str();
    const char *to = iq_to_str.empty() ? nullptr : iq_to_str.c_str();
    const char *type = iq_type_str.empty() ? nullptr : iq_type_str.c_str();
    (void)from; (void)to;
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
            const ::xmpp::StanzaView pubsub_feed = view.child("pubsub", "http://jabber.org/protocol/pubsub");
            if (pubsub_feed.valid() && id && type && weechat_strcasecmp(type, "result") == 0)
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

                    if (!weechat::xmpp_feeds_enabled() || !account.feed_is_open(feed_key))
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
                            const ::xmpp::StanzaView rsm_set = pubsub_feed.child("set", "http://jabber.org/protocol/rsm");
                            if (rsm_set.valid() && max_items_req > 0)
                            {
                                const ::xmpp::StanzaView count_el = rsm_set.child("count");
                                const std::string count_text = count_el.text();
                                if (auto n = parse_int64(count_text); n)
                                    rsm_total_count = static_cast<int>(*n);
                                else
                                    rsm_total_count = -1;
    
                                const ::xmpp::StanzaView first_el = rsm_set.child("first");
                                const std::string first_text = first_el.text();
    
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
                                        const std::string index_storage = first_el.valid() ? first_el.attr_string("index") : std::string{};
                                        const char *index_attr = index_storage.empty() ? nullptr : index_storage.c_str();
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
    
                        const ::xmpp::StanzaView items = pubsub_feed.child("items");
                        if (items.valid() && !stale_page)
                        {
                            // Collect items and sort oldest-first by Atom <published>
                            // (falling back to <updated>). ISO 8601 strings are
                            // zero-padded and sort correctly as strings, so no
                            // strptime needed. This is server-order-independent:
                            // it doesn't matter whether the server returns
                            // oldest-first or newest-first.
                            auto item_pubdate = [](const ::xmpp::StanzaView &item) -> std::string {
                                ::xmpp::StanzaView entry = item.child("entry", "http://www.w3.org/2005/Atom");
                                if (!entry.valid()) entry = item.child("entry");
                                if (!entry.valid()) return {};
                                for (const char *tag : {"published", "updated"}) {
                                    const ::xmpp::StanzaView el = entry.child(tag);
                                    if (!el.valid()) continue;
                                    const std::string s = el.text();
                                    if (!s.empty()) return s;
                                }
                                return {};
                            };
                            std::vector<xmpp_stanza_t *> item_vec;
                            for (const ::xmpp::StanzaView item : ::xmpp::StanzaView(items))
                                item_vec.push_back(item.raw());
                            std::ranges::stable_sort(item_vec,
                                [&item_pubdate](xmpp_stanza_t *a, xmpp_stanza_t *b) {
                                    return item_pubdate(::xmpp::StanzaView{a}) < item_pubdate(::xmpp::StanzaView{b});
                                });
                            for (xmpp_stanza_t *item_raw : item_vec)
                            {
                                const ::xmpp::StanzaView item{item_raw};
                                if (!item.valid() || weechat_strcasecmp(item.name().data(), "item") != 0)
                                    continue;
                                const std::string item_id_storage = item.attr_string("id");
                                const char *item_id_raw = item_id_storage.empty() ? nullptr : item_id_storage.c_str();
    
                                // Skip non-Atom items stored in the node (e.g. avatar metadata
                                // published by Prosody into the blog node).  These are not
                                // microblog posts and have no <entry xmlns="…Atom"> child.
                                // Check by looking for a well-known non-post item id namespace.
                                if (item_id_raw
                                    && ::xmpp::is_skipped_non_atom_feed_item_id(item_id_raw))
                                    continue;
    
                                ::xmpp::StanzaView entry = item.child("entry", "http://www.w3.org/2005/Atom");
                                if (!entry.valid()) entry = item.child("entry");
                                ::xmpp::StanzaView feed = item.child("feed", "http://www.w3.org/2005/Atom");
                                if (!feed.valid()) feed = item.child("feed");
                                if (!entry.valid() && feed.valid())
                                {
                                    atom_feed af = parse_atom_feed(feed);
                                    if (!af.empty())
                                    {
                                        if (!af.title.empty())
                                        {
                                            feed_ch.update_name(af.title.c_str());
                                            weechat::UiPort::for_buffer(feed_ch.buffer)->printf_date_tags_network(
                                                0, "xmpp_feed,notify_none",
                                                fmt::format("Feed title:{} {}",
                                                    weechat::RuntimePort::default_runtime().color("reset"),
                                                    af.title));
                                        }
                                        if (!af.author.empty())
                                            weechat::UiPort::for_buffer(feed_ch.buffer)->printf_date_tags(
                                                0, "xmpp_feed,notify_none",
                                                fmt::format("  {}Author:{} {}",
                                                    weechat::RuntimePort::default_runtime().color("darkgray"),
                                                    weechat::RuntimePort::default_runtime().color("reset"),
                                                    af.author));
                                        if (!af.subtitle.empty())
                                            weechat::UiPort::for_buffer(feed_ch.buffer)->printf_date_tags(
                                                0, "xmpp_feed,notify_none",
                                                fmt::format("  {}", af.subtitle));
                                    }
                                    continue;
                                }
    
                                const std::string publisher = item.attr_string("publisher");
                                atom_entry ae = parse_atom_entry(entry, publisher);
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
                            const ::xmpp::StanzaView rsm_set2 = pubsub_feed.child("set", "http://jabber.org/protocol/rsm");
                            if (rsm_set2.valid())
                            {
                                const ::xmpp::StanzaView first_el2 = rsm_set2.child("first");
                                const std::string first_text2 = first_el2.text();
                                if (!first_text2.empty())
                                {
                                    std::string hint = fmt::format(
                                        "/feed {} {} --before {}",
                                        feed_service, node_name, first_text2);
                                    if (rsm_total_count > 0)
                                        weechat::UiPort::for_buffer(feed_ch.buffer)->printf_date_tags_network(
                                            0, "xmpp_feed,notify_none",
                                            fmt::format("{} item(s) total — for older entries: {}",
                                                rsm_total_count, hint));
                                    else
                                        weechat::UiPort::for_buffer(feed_ch.buffer)->printf_date_tags_network(
                                            0, "xmpp_feed,notify_none",
                                            fmt::format("For older entries: {}", hint));
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
            const ::xmpp::StanzaView pubsub_subs = view.child("pubsub", "http://jabber.org/protocol/pubsub");
            if (pubsub_subs.valid() && id && type && weechat_strcasecmp(type, "result") == 0)
            {
                if (auto subs_it = account.pubsub_subscriptions_queries.find(id); subs_it != account.pubsub_subscriptions_queries.end())
                {
                    const std::string feed_service = subs_it->second;
                    account.pubsub_subscriptions_queries.erase(subs_it);
                    handled = true;

                    if (!weechat::xmpp_feeds_enabled())
                        return handled;

                    const ::xmpp::StanzaView subscriptions = pubsub_subs.child("subscriptions");
                    int node_count = 0;
    
                    if (subscriptions.valid())
                    {
                        for (const ::xmpp::StanzaView sub : ::xmpp::StanzaView(subscriptions))
                        {
                            const char *sub_name = sub.name().data();
                            if (!sub_name || weechat_strcasecmp(sub_name, "subscription") != 0)
                                continue;
    
                            const std::string sub_state = sub.attr_string("subscription");
                            const std::string node_attr = sub.attr_string("node");
    
                            if (node_attr.empty() || sub_state.empty()
                                || weechat_strcasecmp(sub_state.c_str(), "subscribed") != 0)
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
            const ::xmpp::StanzaView pubsub_res = view.child("pubsub", "http://jabber.org/protocol/pubsub");
            if (pubsub_res.valid() && id && type && weechat_strcasecmp(type, "result") == 0)
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
