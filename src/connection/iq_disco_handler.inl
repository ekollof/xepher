void weechat::connection::handle_pubsub_mam_disco_iq_error(xmpp_stanza_t *stanza)
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
    (void)from; (void)to; (void)type;
        // XEP-0442: if a disco#info IQ to a pubsub service returned an error,
        // flush deferred feeds via XEP-0060 plain items IQ (fallback path).
        // Without this, the deferred feeds hang forever since the success path
        // (inside the `if (query.valid() && type)` block) is only entered for type=result.
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
    const ::xmpp::StanzaView view(stanza);
    const std::string iq_id_str = view.attr_string("id");
    const std::string iq_from_str = view.attr_string("from");
    const std::string iq_to_str = view.attr_string("to");
    const std::string iq_type_str = view.attr_string("type");
    const char *id = iq_id_str.empty() ? nullptr : iq_id_str.c_str();
    const char *from = iq_from_str.empty() ? nullptr : iq_from_str.c_str();
    const char *to = iq_to_str.empty() ? nullptr : iq_to_str.c_str();
    const char *type = iq_type_str.empty() ? nullptr : iq_type_str.c_str();
    (void)to;

    const ::xmpp::StanzaView items_query = view.child("query", "http://jabber.org/protocol/disco#items");

    if (!(items_query.valid() && type && weechat_strcasecmp(type, "result") == 0))
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
                ::xmpp::StanzaView item = items_query.child("item");
                while (item.valid())
                {
                    const std::string item_jid = item.attr_string("jid");
                    if (!item_jid.empty())
                    {
                        std::string nick = ::jid(nullptr, item_jid).resource;
                        if (!nick.empty())
                        {
                            ch.add_member(nick.c_str(), nullptr, std::nullopt);
                        }
                    }
                    item = item.next_sibling();
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
    const ::xmpp::StanzaView admin_query = view.child("query", "http://jabber.org/protocol/muc#admin");
    if (admin_query.valid() && type && weechat_strcasecmp(type, "result") == 0)
    {
        if (from)
        {
            std::string from_bare = ::jid(nullptr, from).bare;
            if (auto ch_it = account.channels.find(from_bare); ch_it != account.channels.end())
            {
                auto& [_, ch] = *ch_it;
                if (ch.type == weechat::channel::chat_type::MUC)
                {
                    ::xmpp::StanzaView item = admin_query.child("item");
                    while (item.valid())
                    {
                        const std::string nick = item.attr_string("nick");
                        const std::string real_jid = item.attr_string("jid");
                        const std::string affiliation = item.attr_string("affiliation");
                        if (!real_jid.empty() )
                        {
                            ch.register_omemo_recipient(real_jid.c_str());
                            if (!nick.empty() )
                            {
                                const std::string full_id =
                                    fmt::format("{}/{}", from_bare, nick);
                                weechat::user *occupant =
                                    weechat::user::search(&account, full_id.c_str());
                                if (!occupant)
                                    occupant = weechat::user::search(&account, nick);
                                if (!occupant)
                                {
                                    try
                                    {
                                        auto [it_u, _ins] = account.users.emplace(
                                            std::piecewise_construct,
                                            std::forward_as_tuple(full_id),
                                            std::forward_as_tuple(
                                                &account, &ch, full_id, nick));
                                        occupant = &it_u->second;
                                    }
                                    catch (const std::invalid_argument&)
                                    {
                                        occupant = weechat::user::search(&account, full_id.c_str());
                                    }
                                }

                                bool online = false;
                                if (occupant)
                                    online = occupant->is_online;
                                else if (auto member = ch.member_search(full_id.c_str());
                                         member && (*member)->present)
                                    online = true;
                                else if (auto member = ch.member_search(nick.c_str());
                                         member && (*member)->present)
                                    online = true;

                                if (occupant && !affiliation.empty()                                     && weechat_strcasecmp(affiliation.c_str(), "none") != 0)
                                {
                                    occupant->profile.affiliation = affiliation;
                                }

                                ch.add_member(
                                    full_id.c_str(), nullptr,
                                    std::optional<std::string_view>(real_jid),
                                    occupant,
                                    {.announce_join = false,
                                     .online = online});
                            }
                            else if (account.omemo && ch.muc_supports_omemo())
                            {
                                account.omemo.request_axolotl_devicelist(account, real_jid);
                            }
                        }
                        item = item.next_sibling();
                    }
                }
                // Do not return here — let other admin result processing (if any)
                // or fallthrough continue; this is discovery only.
            }
        }
    }

    // Look for HTTP upload service in items
    ::xmpp::StanzaView item = items_query.child("item");
    while (item.valid())
    {
        const std::string item_jid = item.attr_string("jid");
        if (!item_jid.empty())
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
        item = item.next_sibling();
    }

    // XEP-0050: Ad-Hoc Commands — handle list and execute/form results
    const std::string items_node = items_query.attr_string("node");
    const bool is_commands_node = !items_node.empty() && ::xmpp::is_adhoc_commands_disco_node(items_node);
    const char *iq_id = id;
    bool is_adhoc_query = iq_id && account.adhoc_queries.contains(iq_id);

    if (is_commands_node && is_adhoc_query)
    {
        auto &adhoc_info = account.adhoc_queries[iq_id];
        struct t_gui_buffer *adhoc_buf = adhoc_info.buffer
            ? adhoc_info.buffer : account.buffer;

        auto adhoc_ui = weechat::UiPort::for_buffer(adhoc_buf);

        // For inline (non-picker) path, print header first.
        if (!adhoc_info.picker)
            adhoc_ui->printf_date_tags(0, "xmpp_adhoc,notify_none",
                fmt::format("{}{}Commands available on {}{}:",
                    weechat::RuntimePort::default_runtime().prefix("network"),
                    weechat::RuntimePort::default_runtime().color("bold"),
                    adhoc_info.target_jid,
                    weechat::RuntimePort::default_runtime().color("reset")));

        ::xmpp::StanzaView cmd_item = items_query.child("item");
        int count = 0;
        while (cmd_item.valid())
        {
            const std::string cmd_node = cmd_item.attr_string("node");
            const std::string cmd_name = cmd_item.attr_string("name");
            const std::string cmd_jid = cmd_item.attr_string("jid");

            if (adhoc_info.picker)
            {
                // Picker path: add entry (value = node URI, label = friendly name)
                using picker_t = weechat::ui::picker<std::string>;
                std::string label = !cmd_name.empty() ? cmd_name : (!cmd_node.empty() ? cmd_node : "(unnamed)");
                std::string sublabel = !cmd_node.empty() ? cmd_node : "";
                adhoc_info.picker->add_entry(
                    picker_t::entry{cmd_node, label, sublabel, true});
            }
            else
            {
                // Inline print path
                adhoc_ui->printf_date_tags(0, "xmpp_adhoc,notify_none",
                    fmt::format("{}  {}{:<40}{}  {}{}",
                        weechat::RuntimePort::default_runtime().prefix("network"),
                        weechat::RuntimePort::default_runtime().color("bold"),
                        !cmd_name.empty() ? cmd_name : "(unnamed)",
                        weechat::RuntimePort::default_runtime().color("reset"),
                        !cmd_node.empty() ? cmd_node : "",
                        !cmd_jid.empty() && cmd_jid != adhoc_info.target_jid
                            ? fmt::format(" [{}]", cmd_jid) : ""));
            }
            count++;
            cmd_item = cmd_item.next_sibling();
        }

        if (!adhoc_info.picker)
        {
            if (count == 0)
                adhoc_ui->printf_date_tags(0, "xmpp_adhoc,notify_none",
                    fmt::format("{}  (no commands available)",
                        weechat::RuntimePort::default_runtime().prefix("network")));
            else
                adhoc_ui->printf_date_tags(0, "xmpp_adhoc,notify_none",
                    fmt::format("{}  Use /adhoc {} <node> to execute a command",
                        weechat::RuntimePort::default_runtime().prefix("network"),
                        adhoc_info.target_jid));
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
        ::xmpp::StanzaView disco_item = items_query.child("item");
        while (disco_item.valid())
        {
            const std::string node_attr = disco_item.attr_string("node");
            if (!node_attr.empty())
            {
                std::string node_name(node_attr);

                // XEP-0472 §4.1: skip comment sub-nodes — they are per-post
                // threads, not independent feeds, and are always empty at the
                // top-level disco level.
                if (::xmpp::is_microblog_comments_node(node_name))
                {
                    disco_item = disco_item.next_sibling();
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
            disco_item = disco_item.next_sibling();
        }

        weechat::UiPort::for_buffer(account.buffer)->printf_network(fmt::format(
            "Feed discovery on {}: fetching {} node(s)",
            feed_service, node_count));
    }

    return false;
}

void weechat::connection::handle_adhoc_command_iq_event(xmpp_stanza_t *stanza)
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
    (void)to;

    const ::xmpp::StanzaView adhoc_command = view.child("command", "http://jabber.org/protocol/commands");
    if (!adhoc_command.valid() || !type)
        return;
    const char *iq_id = id;
    bool is_adhoc_query = iq_id && account.adhoc_queries.contains(iq_id);
    struct t_gui_buffer *adhoc_buf = is_adhoc_query
        ? (account.adhoc_queries[iq_id].buffer
           ? account.adhoc_queries[iq_id].buffer : account.buffer)
        : account.buffer;
    const std::string cmd_node = adhoc_command.attr_string("node");
    const std::string cmd_status = adhoc_command.attr_string("status");
    const std::string session_id = adhoc_command.attr_string("sessionid");
    const char *from_jid = from;

    auto adhoc_ui = weechat::UiPort::for_buffer(adhoc_buf);
    if (weechat_strcasecmp(type, "error") == 0)
    {
        adhoc_ui->printf_date_tags(0, "xmpp_adhoc,notify_none",
            fmt::format("{}[adhoc] Error executing command {}",
                weechat::RuntimePort::default_runtime().prefix("error"),
                !cmd_node.empty() ? cmd_node : "(unknown)"));
    }
    else if (weechat_strcasecmp(type, "result") == 0)
    {
        // Check for a data form to display
        const ::xmpp::StanzaView x_form = adhoc_command.child("x", "jabber:x:data");

        if (x_form.valid())
        {
            const std::string form_type = x_form.attr_string("type");
            if (!form_type.empty() && std::string_view(form_type) == "result")
            {
                // Display result form (read-only)
                render_data_form(adhoc_buf, x_form.raw(), from_jid, cmd_node.c_str(), nullptr);
            }
            else
            {
                // Input form — render and prompt for submission
                render_data_form(adhoc_buf, x_form.raw(), from_jid, cmd_node.c_str(), session_id.empty() ? nullptr : session_id.c_str());
            }
        }
        else if (!cmd_status.empty() && std::string_view(cmd_status) == "completed")
        {
            // Command completed with no form — check for <note>
            const ::xmpp::StanzaView note = adhoc_command.child("note");
            const std::string note_text = note.text();
            adhoc_ui->printf_date_tags(0, "xmpp_adhoc,notify_none",
                fmt::format("{}[adhoc] Command {} completed{}{}",
                    weechat::RuntimePort::default_runtime().prefix("network"),
                    !cmd_node.empty() ? cmd_node : "",
                    note_text.empty() ? "" : ": ",
                    note_text.empty() ? "" : note_text));
        }
        else if (!cmd_status.empty() && std::string_view(cmd_status) == "executing" && !x_form.valid())
        {
            adhoc_ui->printf_date_tags(0, "xmpp_adhoc,notify_none",
                fmt::format("{}[adhoc] Command {} in progress (no form)",
                    weechat::RuntimePort::default_runtime().prefix("network"),
                    !cmd_node.empty() ? cmd_node : ""));
        }
    }

    if (is_adhoc_query)
        account.adhoc_queries.erase(iq_id);
}

bool weechat::connection::handle_channel_search_iq_event(xmpp_stanza_t *stanza)
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
    const char *cs_id = id;
    bool is_cs_query = cs_id && account.channel_search_queries.contains(cs_id);

    if (is_cs_query && type)
    {
        auto &cs_info = account.channel_search_queries[cs_id];
        struct t_gui_buffer *cs_buf = cs_info.buffer ? cs_info.buffer : account.buffer;
        auto cs_ui = weechat::UiPort::for_buffer(cs_buf);

        if (weechat_strcasecmp(type, "error") == 0)
        {
            // Try to extract a human-readable error
            const ::xmpp::StanzaView error_el = view.child("error");
            std::string err_text_str;
            const char *err_condition = nullptr;
            if (error_el.valid())
            {
                if (const ::xmpp::StanzaView text_el = error_el.child("text"); text_el.valid())
                    err_text_str = text_el.text();

                // XMPP stanza errors usually encode the condition as a child in
                // urn:ietf:params:xml:ns:xmpp-stanzas (e.g. <bad-request/>).
                if (err_text_str.empty())
                {
                    for (const ::xmpp::StanzaView cond : ::xmpp::StanzaView(error_el))
                    {
                        const auto cond_ns = cond.xmlns();
                        const char *cond_name = cond.name().data();
                        if (cond_name && cond_ns
                            && *cond_ns == "urn:ietf:params:xml:ns:xmpp-stanzas"
                            && std::string_view(cond_name) != "text")
                        {
                            err_condition = cond_name;
                            break;
                        }
                    }
                }
            }
            weechat::UiPort::for_buffer(cs_buf)->printf_date_tags(
                0, "xmpp_channel_search,notify_none",
                fmt::format("{}[search] Error from {}: {}",
                    weechat::RuntimePort::default_runtime().prefix("error"),
                    cs_info.service_jid,
                    !err_text_str.empty() ? err_text_str
                        : (err_condition ? err_condition : "unknown error")));
            account.channel_search_queries.erase(cs_id);
        }
        else if (weechat_strcasecmp(type, "result") == 0)
        {
            if (cs_info.form_requested)
            {
                // Step 1 response: service returned a search form.
                const ::xmpp::StanzaView search_el = view.child("search", "urn:xmpp:channel-search:0:search");
                const ::xmpp::StanzaView x_form = search_el.valid()
                    ? search_el.child("x", "jabber:x:data")
                    : ::xmpp::StanzaView{};

                if (!search_el.valid() || !x_form.valid())
                {
                    cs_ui->printf_date_tags(
                        0, "xmpp_channel_search,notify_none",
                        fmt::format("{}[search] Unexpected response from {} (missing form)",
                            weechat::RuntimePort::default_runtime().prefix("error"),
                            cs_info.service_jid));
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
            ::xmpp::StanzaView result_el = view.child("result", "urn:xmpp:channel-search:0:search");
            if (!result_el.valid())
                result_el = view.child("search", "urn:xmpp:channel-search:0:search");

            if (result_el.valid())
            {
                int count = 0;
                ::xmpp::StanzaView item = result_el.child("item");
                while (item.valid())
                {
                    if (count == 0 && !cs_info.picker)
                    {
                        cs_ui->printf_date_tags(0, "xmpp_channel_search,notify_none",
                            fmt::format("{}MUC Rooms (via {}):",
                                weechat::RuntimePort::default_runtime().prefix("network"),
                                cs_info.service_jid));
                    }

                    const std::string address = item.attr_string("address");
                    if (address.empty())
                    {
                        item = item.next_sibling();
                        continue;
                    }

                    // Child elements may include: <name>, <nusers>, <description>,
                    // <is-open>, <language>, <service-type>, <anonymity-mode>.
                    const ::xmpp::StanzaView name_el = item.child("name");
                    const ::xmpp::StanzaView nusers_el = item.child("nusers");
                    const ::xmpp::StanzaView desc_el = item.child("description");
                    const ::xmpp::StanzaView open_el = item.child("is-open");
                    const ::xmpp::StanzaView language_el = item.child("language");
                    const ::xmpp::StanzaView service_type_el = item.child("service-type");
                    const ::xmpp::StanzaView anonymity_el = item.child("anonymity-mode");

                    const std::string name_raw    = name_el.valid() ? name_el.text()   : std::string {};
                    const std::string nusers_raw  = nusers_el.valid() ? nusers_el.text() : std::string {};
                    const std::string desc_raw    = desc_el.valid() ? desc_el.text()   : std::string {};
                    const std::string language_raw = language_el.valid() ? language_el.text() : std::string {};
                    const std::string service_type_raw = service_type_el.valid() ? service_type_el.text() : std::string {};
                    const std::string anonymity_raw = anonymity_el.valid() ? anonymity_el.text() : std::string {};

                    std::string display = address;
                    if (!name_raw.empty())
                        display = name_raw + " <" + address + ">";

                    std::vector<std::string> meta_parts;
                    if (!nusers_raw.empty())
                        meta_parts.emplace_back(nusers_raw + " users");

                    bool is_open = false;
                    if (open_el.valid())
                    {
                        const std::string open_raw = open_el.text();
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
                        cs_ui->printf_date_tags(0, "xmpp_channel_search,notify_none",
                            fmt::format("  {}{}{}{}",
                                weechat::RuntimePort::default_runtime().color("chat_nick"),
                                display,
                                weechat::RuntimePort::default_runtime().color("reset"),
                                info_bracketed));

                        // Truncate long descriptions
                        if (!desc_raw.empty())
                        {
                            std::string desc = desc_raw;
                            if (desc.length() > 120)
                                desc = desc.substr(0, 117) + "...";
                            cs_ui->printf_date_tags(0, "xmpp_channel_search,notify_none",
                                fmt::format("    {}", desc));
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
                    item = item.next_sibling();
                }

                if (!cs_info.picker)
                {
                    if (count == 0)
                    {
                        cs_ui->printf_date_tags(0, "xmpp_channel_search,notify_none",
                            fmt::format("{}No rooms found matching your query",
                                weechat::RuntimePort::default_runtime().prefix("network")));
                    }
                    else
                    {
                        cs_ui->printf_date_tags(0, "xmpp_channel_search,notify_none",
                            fmt::format("{}Use /enter <address> to join a room",
                                weechat::RuntimePort::default_runtime().prefix("network")));
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
                cs_ui->printf_date_tags(0, "xmpp_channel_search,notify_none",
                    fmt::format("{}[search] Unexpected response from {} (missing <result>)",
                        weechat::RuntimePort::default_runtime().prefix("error"),
                        cs_info.service_jid));
            }

            account.channel_search_queries.erase(cs_id);
        }
    }

    return false;
}

bool weechat::connection::handle_disco_info_iq_event(xmpp_stanza_t *stanza)
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
    (void)to;
    xmpp_stanza_t *reply = nullptr;

    const ::xmpp::StanzaView query = view.child("query", "http://jabber.org/protocol/disco#info");
    if (!query.valid() || !type)
        return false;

    {
    const char *stanza_id = id;

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

                for (const ::xmpp::StanzaView identity : ::xmpp::StanzaView(query))
                {
                    if (identity.name() != "identity")
                        continue;
                    const std::string cat = identity.attr_string("category");
                    const std::string typ = identity.attr_string("type");
                    const std::string nam = identity.attr_string("name");
                    if (!cat.empty() && !typ.empty() && !nam.empty()
                        && weechat_strcasecmp(cat.c_str(), "conference") == 0
                        && weechat_strcasecmp(typ.c_str(), "text") == 0)
                    {
                        name_s = nam;
                        break;
                    }
                }

                const ::xmpp::StanzaView x = query.child("x", "jabber:x:data");
                if (x.valid())
                {
                    ::xmpp::StanzaView field = x.child("field");
                    while (field.valid())
                    {
                        const std::string var = field.attr_string("var");
                        if (var.empty())
                        {
                            field = field.next_sibling();
                            continue;
                        }
                        std::string txt;
                        for (const ::xmpp::StanzaView vnode : ::xmpp::StanzaView(field))
                        {
                            const char *vname = vnode.name().data();
                            if (!vname || weechat_strcasecmp(vname, "value") != 0)
                                continue;
                            txt = vnode.text();
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
                        field = field.next_sibling();
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

                    weechat::UiPort::for_buffer(out)->printf_date_tags(
                        0, "xmpp_channel_search,notify_none",
                        fmt::format("    {}{}{}",
                            weechat::RuntimePort::default_runtime().color("chat_delimiters"),
                            header, meta_s));

                    if (!desc_s.empty())
                    {
                        if (desc_s.size() > 140)
                            desc_s = desc_s.substr(0, 137) + "...";
                        weechat::UiPort::for_buffer(out)->printf_date_tags(
                            0, "xmpp_channel_search,notify_none",
                            fmt::format("      {}", desc_s));
                    }
                }
            }

            account.channel_search_disco_queries.erase(stanza_id);
            return true;
        }

        if (weechat_strcasecmp(type, "get") == 0)
        {
            const std::string requested_node = query.attr_string("node");

            // XEP-0030 §3.3: if a node= is present, it MUST match a recognized
            // node (our caps node "http://weechat.org#<hash>") otherwise return
            // <item-not-found/>.  An absent or empty node= always gets a normal reply.
            bool node_ok = true;
            if (!requested_node.empty() )
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
                reply = get_caps(xmpp_stanza_reply(stanza), nullptr, requested_node.empty() ? nullptr : requested_node.c_str());
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
                    const ::xmpp::StanzaView xdata = query.child("x", "jabber:x:data");
                    if (xdata.valid())
                    {
                        for (::xmpp::StanzaView field = xdata.child("field");
                             field.valid();
                             field = field.next_sibling())
                        {
                            const std::string var = field.attr_string("var");
                            if (var.empty())
                                continue;
                            std::string txt;
                            for (const ::xmpp::StanzaView vnode : ::xmpp::StanzaView(field))
                            {
                                const char *vname = vnode.name().data();
                                if (!vname || weechat_strcasecmp(vname, "value") != 0)
                                    continue;
                                txt = vnode.text();
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
                
                ::xmpp::StanzaView feature = query.child("feature");
                while (feature.valid())
                {
                    const std::string var = feature.attr_string("var");
                    if (!var.empty() && std::string_view(var) == "urn:xmpp:http:upload:0")
                    {
                        supports_upload = true;
                    }
                    feature = feature.next_sibling();
                }
                
                // Check for max file size in x data form
                if (supports_upload)
                {
                    const ::xmpp::StanzaView x = query.child("x", "jabber:x:data");
                    if (x.valid())
                    {
                        ::xmpp::StanzaView field = x.child("field");
                        while (field.valid())
                        {
                            const std::string var = field.attr_string("var");
                            if (!var.empty() && std::string_view(var) == "max-file-size")
                            {
                                const ::xmpp::StanzaView value = field.child("value");
                                if (value.valid())
                                {
                                    if (auto n = parse_int64(value.text()); n && *n > 0)
                                        max_size = static_cast<size_t>(*n);
                                }
                            }
                            field = field.next_sibling();
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
    const char *from_jid = from;
                struct t_gui_buffer *output_buffer = account.buffer;
                
                auto disco_ui = weechat::UiPort::for_buffer(output_buffer);
                disco_ui->printf("");
                disco_ui->printf(fmt::format("{}Service Discovery for {}{}:",
                    weechat::RuntimePort::default_runtime().color("chat_prefix_network"),
                    weechat::RuntimePort::default_runtime().color("chat_server"),
                    from_jid ? from_jid : "server"));
            }
            
            for (const ::xmpp::StanzaView identity : ::xmpp::StanzaView(query))
            {
                if (identity.name() != "identity")
                    continue;

                const std::string category = identity.attr_string("category");
                std::string name;
                if (auto nam = identity.attr("name"); nam && !nam->empty())
                    name = unescape(*nam);
                const std::string type = identity.attr_string("type");

                if (category.empty() && type.empty() && name.empty())
                    continue;

                if (user_initiated)
                {
                    weechat::UiPort::for_buffer(account.buffer)->printf(fmt::format(
                        "  {}Identity:{} {}/{} {}{}{}",
                        weechat::RuntimePort::default_runtime().color("chat_prefix_network"),
                        weechat::RuntimePort::default_runtime().color("reset"),
                        category, type,
                        weechat::RuntimePort::default_runtime().color("chat_delimiters"),
                        name.empty() ? "" : name,
                        weechat::RuntimePort::default_runtime().color("reset")));
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
            }
            
            if (user_initiated && !features.empty())
            {
                auto feat_ui = weechat::UiPort::for_buffer(account.buffer);
                feat_ui->printf(fmt::format("  {}Features:",
                    weechat::RuntimePort::default_runtime().color("chat_prefix_network")));
                std::ranges::for_each(features, [&](const std::string &var) {
                    feat_ui->printf(fmt::format("    {}", var));
                });
            }
        }
    }

    return false;
}
