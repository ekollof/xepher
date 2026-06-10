void weechat::connection::handle_pubsub_mam_disco_iq_error(xmpp_stanza_t *stanza)
{
    const char *id = xmpp_stanza_get_id(stanza);
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
                    if (!account.feed_is_open(feed_key))
                        continue;

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
}

bool weechat::connection::handle_disco_items_iq_event(xmpp_stanza_t *stanza)
{
    const char *type = xmpp_stanza_get_attribute(stanza, "type");
    const char *from = xmpp_stanza_get_from(stanza);

    xmpp_stanza_t *items_query = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "query", "http://jabber.org/protocol/disco#items");

    if (!(items_query && type && weechat_strcasecmp(type, "result") == 0))
        return false;

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
                        if (real_jid && *real_jid)
                        {
                            ch.register_omemo_recipient(real_jid);
                            if (nick && *nick)
                            {
                                ch.add_member(nick, nullptr,
                                    std::optional<std::string_view>(real_jid));
                            }
                            else if (account.omemo && ch.muc_supports_omemo())
                            {
                                account.omemo.request_axolotl_devicelist(account, real_jid);
                            }
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
    const bool is_commands_node = items_node
        && ::xmpp::is_adhoc_commands_disco_node(items_node);
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
                if (::xmpp::is_microblog_comments_node(node_name))
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
                send(stanza::iq()
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

    return false;
}

void weechat::connection::handle_adhoc_command_iq_event(xmpp_stanza_t *stanza)
{
    const char *type = xmpp_stanza_get_attribute(stanza, "type");

    xmpp_stanza_t *adhoc_command = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "command", "http://jabber.org/protocol/commands");
    if (!adhoc_command || !type)
        return;

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

bool weechat::connection::handle_channel_search_iq_event(xmpp_stanza_t *stanza)
{
    const char *type = xmpp_stanza_get_attribute(stanza, "type");

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

    return false;
}

bool weechat::connection::handle_disco_info_iq_event(xmpp_stanza_t *stanza)
{
    const char *type = xmpp_stanza_get_attribute(stanza, "type");
    const char *from = xmpp_stanza_get_from(stanza);
    xmpp_stanza_t *reply = nullptr;

    xmpp_stanza_t *query = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "query", "http://jabber.org/protocol/disco#info");
    if (!query || !type)
        return false;

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

                node_ok = ::xmpp::caps_requested_node_ok(
                    requested_node,
                    computed_hash ? *computed_hash : std::string_view{});
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

            const std::vector<std::string> features =
                ::xmpp::disco_feature_vars(::xmpp::StanzaView(query));

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
                const std::string ver_hash = account.caps_disco_queries[stanza_id];
                account.caps_disco_queries.erase(stanza_id);

                const std::string s = ::xmpp::build_caps_verification_string(
                    ::xmpp::StanzaView(query), features);
                XDEBUG("caps: S string for {} (len={}): '{}'",
                       from ? from : "?", s.size(), s);

                const std::string computed = ::xmpp::caps_sha1_base64(s);
                if (computed == ver_hash)
                    account.caps_cache_save(ver_hash, features);
                else
                    XDEBUG("caps: hash mismatch for {}: got '{}' expected '{}'; discarding",
                           from ? from : "?", computed, ver_hash);
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
                    if (!account.feed_is_open(feed_key))
                        continue;

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

                // XEP-0060: record pubsub service components so /feed discover
                // can list them without the user having to know the JID in advance.
                if (category == "pubsub")
                {
                    const std::string &svc_jid = upload_disco && !upload_service_jid.empty()
                        ? upload_service_jid
                        : (from ? std::string(from) : std::string{});
                    const std::string svc_bare = ::jid(nullptr, svc_jid).bare;
                    const std::string own_bare = ::jid(nullptr, account.jid()).bare;
                    if (!svc_bare.empty() && svc_bare != own_bare)
                    {
                        auto &kps = account.known_pubsub_services;
                        if (!std::ranges::contains(kps, svc_bare))
                            kps.push_back(svc_bare);
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

    return false;
}
