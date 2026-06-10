bool weechat::connection::iq_handler(xmpp_stanza_t *stanza, bool top_level)
{
    // SM counter incremented in libstrophe wrapper, not here
    // top_level parameter kept for nested/recursive calls

    (void) top_level;
    append_raw_xml_trace(account, "RECV", stanza);

    xmpp_stanza_t *query;

    auto binding = std::make_unique<xml::iq>(account.context, stanza);
    const char *id = xmpp_stanza_get_id(stanza);
    const char *from = xmpp_stanza_get_from(stanza);
    const char *to = xmpp_stanza_get_to(stanza);
    const char *type = xmpp_stanza_get_attribute(stanza, "type");
    // Keep own JID alive for the duration of this handler.
    // account.jid() returns a temporary std::string; storing .data() on it
    // immediately produces a dangling pointer.  Cache it once here.
    const std::string own_jid = account.jid();

    // XEP-0060: on publish success, re-fetch so the feed buffer updates
    // immediately without a manual /feed refresh.
    // - publish/reply: re-fetch only the single new item by ID.
    // - retract:       re-fetch the full node (the item is gone; fetching it
    //                  by ID returns nothing, so we need the current page).
    // Called at every publish-result erase site.
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
        {
            // Publish path: request only the single newly-published item.
            its.item(stanza::xep0060::item().id(pub_item_id));
        }
        // Retract path: no <item id> filter → server returns current page.
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
    
    if (handle_ping_iq_event(stanza, own_jid))
        return true;

    // XEP-0280: carbons enable IQ result/error
    if (id && account.pending_carbons_enable_iq_
        && *account.pending_carbons_enable_iq_ == id)
    {
        account.pending_carbons_enable_iq_.reset();
        if (type && weechat_strcasecmp(type, "error") == 0)
        {
            const auto err_detail = ::xmpp::iq_error_text(
                ::xmpp::StanzaView(stanza).child("error"));
            XDEBUG("Message carbons enable rejected: {}", err_detail);
            auto ui = weechat::UiPort::for_buffer(account.buffer);
            ui->printf_error(fmt::format(
                "Message carbons: server rejected enable ({})", err_detail));
        }
        else
        {
            XDEBUG("Message carbons enabled");
            auto ui = weechat::UiPort::for_buffer(account.buffer);
            ui->printf_network("Message carbons enabled");
        }
        return true;
    }

    // XEP-0441: MAM Preferences — handle <prefs xmlns='urn:xmpp:mam:2'> result/error
    if (id && account.mam_prefs_queries.contains(id))
    {
        struct t_gui_buffer *prefs_buf = account.mam_prefs_queries[id];
        if (!prefs_buf) prefs_buf = account.buffer;
        account.mam_prefs_queries.erase(id);

        xmpp_stanza_t *prefs_el = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "prefs", "urn:xmpp:mam:2");

        auto prefs_ui = weechat::UiPort::for_buffer(prefs_buf);
        if (type && weechat_strcasecmp(type, "error") == 0)
        {
            xmpp_stanza_t *err = xmpp_stanza_get_child_by_name(stanza, "error");
            const char *err_type = err ? xmpp_stanza_get_attribute(err, "type") : "unknown";
            prefs_ui->printf_error(fmt::format(
                "MAM preferences: server returned error ({}) — feature may not be supported",
                err_type));
        }
        else if (prefs_el)
        {
            const char *def = xmpp_stanza_get_attribute(prefs_el, "default");
            prefs_ui->printf_network(fmt::format(
                "MAM preferences: default={}{}{}",
                weechat_color("bold"), def ? def : "(unset)", weechat_color("-bold")));

            // Always list
            xmpp_stanza_t *always_el = xmpp_stanza_get_child_by_name(prefs_el, "always");
            if (always_el)
            {
                std::string jids_str;
                for (xmpp_stanza_t *jid_el = xmpp_stanza_get_children(always_el);
                     jid_el; jid_el = xmpp_stanza_get_next(jid_el))
                {
                    const char *jn = xmpp_stanza_get_name(jid_el);
                    if (!jn || std::string_view(jn) != "jid") continue;
                    const std::string jid_txt = stanza_element_text(jid_el);
                    if (!jid_txt.empty())
                    {
                        if (!jids_str.empty()) jids_str += ", ";
                        jids_str += jid_txt;
                    }
                }
                prefs_ui->printf_network(fmt::format(
                    "  always: {}",
                    jids_str.empty() ? "(empty)" : jids_str));
            }

            // Never list
            xmpp_stanza_t *never_el = xmpp_stanza_get_child_by_name(prefs_el, "never");
            if (never_el)
            {
                std::string jids_str;
                for (xmpp_stanza_t *jid_el = xmpp_stanza_get_children(never_el);
                     jid_el; jid_el = xmpp_stanza_get_next(jid_el))
                {
                    const char *jn = xmpp_stanza_get_name(jid_el);
                    if (!jn || std::string_view(jn) != "jid") continue;
                    const std::string jid_txt = stanza_element_text(jid_el);
                    if (!jid_txt.empty())
                    {
                        if (!jids_str.empty()) jids_str += ", ";
                        jids_str += jid_txt;
                    }
                }
                prefs_ui->printf_network(fmt::format(
                    "  never:  {}",
                    jids_str.empty() ? "(empty)" : jids_str));
            }

            prefs_ui->printf_network("MAM preferences updated successfully");
        }
        return true;
    }

    if (handle_vcard_iq_event(stanza, own_jid))
        return true;

    if (handle_vcard4_pubsub_iq_event(stanza, own_jid))
        return true;

    if (handle_avatar_pubsub_iq_event(stanza, own_jid))
        return true;

    if (handle_bob_iq_event(stanza, own_jid))
        return true;

    if (handle_pubsub_feed_iq_event(stanza))
        return true;

    if (handle_upload_slot_iq_event(stanza))
        return true;

    // XEP-0060: clean up publish tracking on bare <iq type='result'/> (no pubsub child)
    // and trigger a single-item re-fetch so the buffer updates immediately.
    if (id && type && weechat_strcasecmp(type, "result") == 0)
        trigger_publish_refetch(id);

    if (id && type && weechat_strcasecmp(type, "error") == 0)
    {
        handle_mam_query_iq_error(stanza);

        if (handle_upload_slot_iq_error(stanza))
            return true;

        handle_pubsub_mam_disco_iq_error(stanza);


        // XEP-0060: pubsub publish error — report to the originating buffer.
        {
            if (auto pub_it = account.pubsub_publish_ids.find(id); pub_it != account.pubsub_publish_ids.end())
            {
                auto& [_, ctx] = *pub_it;
                xmpp_stanza_t *error_elem = xmpp_stanza_get_child_by_name(stanza, "error");
                const std::string error_cond = error_elem
                    ? ::xmpp::iq_error_text(::xmpp::StanzaView(error_elem)) : "unknown error";
                struct t_gui_buffer *err_buf = ctx.buffer ? ctx.buffer : account.buffer;
                weechat::UiPort::for_buffer(err_buf)->printf_error(fmt::format(
                    "{}: publish failed for {}/{} (item {}): {}",
                    WEECHAT_XMPP_PLUGIN_NAME,
                    ctx.service, ctx.node, ctx.item_id, error_cond));
                account.pubsub_publish_ids.erase(pub_it);
            }
        }

        // XEP-0060: pubsub subscribe/unsubscribe error
        {
            if (auto sub_it = account.pubsub_subscribe_queries.find(id); sub_it != account.pubsub_subscribe_queries.end())
            {
                auto& [_, sub] = *sub_it;
                xmpp_stanza_t *error_elem = xmpp_stanza_get_child_by_name(stanza, "error");
                const std::string error_cond = error_elem
                    ? ::xmpp::iq_error_text(::xmpp::StanzaView(error_elem)) : "unknown error";
                struct t_gui_buffer *fb = sub.buffer ? sub.buffer : account.buffer;
                weechat::UiPort::for_buffer(fb)->printf_error(fmt::format(
                    "{}: subscribe to {} failed: {}",
                    WEECHAT_XMPP_PLUGIN_NAME, sub.feed_key, error_cond));
                account.pubsub_subscribe_queries.erase(sub_it);
            }
        }
        {
            if (auto unsub_it = account.pubsub_unsubscribe_queries.find(id); unsub_it != account.pubsub_unsubscribe_queries.end())
            {
                auto& [_, unsub] = *unsub_it;
                xmpp_stanza_t *error_elem = xmpp_stanza_get_child_by_name(stanza, "error");
                const std::string error_cond = error_elem
                    ? ::xmpp::iq_error_text(::xmpp::StanzaView(error_elem)) : "unknown error";
                struct t_gui_buffer *fb = unsub.buffer ? unsub.buffer : account.buffer;
                weechat::UiPort::for_buffer(fb)->printf_error(fmt::format(
                    "{}: unsubscribe from {} failed: {}",
                    WEECHAT_XMPP_PLUGIN_NAME, unsub.feed_key, error_cond));
                account.pubsub_unsubscribe_queries.erase(unsub_it);
            }
        }

        // XEP-0060: pubsub item-fetch error (e.g. forbidden, item-not-found)
        {
            if (auto fetch_it = account.pubsub_fetch_ids.find(id); fetch_it != account.pubsub_fetch_ids.end())
            {
                auto& [_, fi] = *fetch_it;
                const std::string &err_service = fi.service;
                const std::string &err_node    = fi.node;

                xmpp_stanza_t *error_elem = xmpp_stanza_get_child_by_name(stanza, "error");
                const std::string error_cond = error_elem
                    ? ::xmpp::iq_error_text(::xmpp::StanzaView(error_elem)) : "unknown error";

                // Report the error in the feed buffer if it already exists,
                // otherwise fall back to the account buffer.
                std::string feed_key = fmt::format("{}/{}", err_service, err_node);
                struct t_gui_buffer *err_buf = account.buffer;
                if (auto ch_it = account.channels.find(feed_key); ch_it != account.channels.end())
                {
                    auto& [_, ch] = *ch_it;
                    err_buf = ch.buffer;
                }

                weechat::UiPort::for_buffer(err_buf)->printf_error(fmt::format(
                    "{}: cannot fetch feed {}/{}: {}",
                    WEECHAT_XMPP_PLUGIN_NAME, err_service, err_node, error_cond));

                account.pubsub_fetch_ids.erase(fetch_it);
            }
        }
    }
    
    // XEP-0191: Blocking Command
    xmpp_stanza_t *blocklist = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "blocklist", "urn:xmpp:blocking");
    xmpp_stanza_t *block = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "block", "urn:xmpp:blocking");
    xmpp_stanza_t *unblock = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "unblock", "urn:xmpp:blocking");
    
    if (blocklist && type && weechat_strcasecmp(type, "result") == 0)
    {
        // Handle blocklist response — populate the picker if open, else print inline.
        xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(blocklist, "item");

        if (account.blocklist_picker)
        {
            // Picker is open: feed entries into it.
            using picker_t = weechat::ui::picker<std::string>;
            if (!item)
            {
                // No blocked JIDs — add a non-selectable placeholder row.
                account.blocklist_picker->add_entry(
                    picker_t::entry{"", "(no blocked JIDs)", "", false});
            }
            else
            {
                while (item)
                {
                    const char *jid = xmpp_stanza_get_attribute(item, "jid");
                    if (jid)
                        account.blocklist_picker->add_entry(
                            picker_t::entry{std::string(jid), std::string(jid), "", true});
                    item = xmpp_stanza_get_next(item);
                }
            }
        }
        else
        {
            // Picker not open (e.g. called without /blocklist picker path) — print inline.
            auto ui = weechat::UiPort::for_buffer(account.buffer);
            if (item)
            {
                ui->printf_network("Blocked JIDs:");
                while (item)
                {
                    const char *jid = xmpp_stanza_get_attribute(item, "jid");
                    if (jid)
                        ui->printf(fmt::format("  {}", jid));
                    item = xmpp_stanza_get_next(item);
                }
            }
            else
            {
                ui->printf_network("No JIDs blocked");
            }
        }

        return true;
    }
    
    if (block && type && weechat_strcasecmp(type, "result") == 0)
    {
        weechat::UiPort::for_buffer(account.buffer)->printf_network(
            "Block request successful");
        return true;
    }
    
    if (unblock && type && weechat_strcasecmp(type, "result") == 0)
    {
        weechat::UiPort::for_buffer(account.buffer)->printf_network(
            "Unblock request successful");
        return true;
    }

    // XEP-0191: server-pushed block/unblock IQ sets (§8.4, §8.5)
    if (block && type && weechat_strcasecmp(type, "set") == 0)
    {
        auto ui = weechat::UiPort::for_buffer(account.buffer);
        xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(block, "item");
        while (item)
        {
            const char *jid = xmpp_stanza_get_attribute(item, "jid");
            if (jid)
                ui->printf_network(fmt::format("{} was blocked", jid));
            item = xmpp_stanza_get_next(item);
        }
        // Acknowledge the server push
        account.connection.send(stanza::iq()
            .type("result")
            .id(id ? id : "")
            .to(from ? from : "")
            .from(to ? to : "")
            .build(account.context)
            .get());
        return true;
    }

    if (unblock && type && weechat_strcasecmp(type, "set") == 0)
    {
        auto ui = weechat::UiPort::for_buffer(account.buffer);
        xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(unblock, "item");
        if (item)
        {
            while (item)
            {
                const char *jid = xmpp_stanza_get_attribute(item, "jid");
                if (jid)
                    ui->printf_network(fmt::format("{} was unblocked", jid));
                item = xmpp_stanza_get_next(item);
            }
        }
        else
        {
            ui->printf_network("All JIDs unblocked");
        }
        // Acknowledge the server push
        account.connection.send(stanza::iq()
            .type("result")
            .id(id ? id : "")
            .to(from ? from : "")
            .from(to ? to : "")
            .build(account.context)
            .get());
        return true;
    }

    // XEP-0045 §10.2/§10.5/§10.7: muc#owner / muc#admin owner-tracked IQ results.
    // Match by IQ id only — muc#owner responses carry muc#owner, not disco#items.
    const char *owner_stanza_id = xmpp_stanza_get_id(stanza);
    if (owner_stanza_id && account.muc_owner_queries.contains(owner_stanza_id))
    {
        auto info = account.muc_owner_queries[owner_stanza_id];
        account.muc_owner_queries.erase(owner_stanza_id);
        struct t_gui_buffer *out = info.buffer ? info.buffer : account.buffer;

        xmpp_stanza_t *owner_q = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "query", "http://jabber.org/protocol/muc#owner");
        xmpp_stanza_t *admin_q = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "query", "http://jabber.org/protocol/muc#admin");
        xmpp_stanza_t *register_q = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "query", "http://jabber.org/protocol/muc#register");

        // Error path: print a friendly message and return.
        if (type && weechat_strcasecmp(type, "error") == 0)
        {
            std::string err = "server error";
            if (auto err_el = xmpp_stanza_get_child_by_name(stanza, "error"))
            {
                if (xmpp_stanza_get_child_by_name(err_el, "forbidden"))
                    err = "permission denied (you must be a room owner)";
                else if (xmpp_stanza_get_child_by_name(err_el, "not-allowed"))
                    err = "not allowed (room configuration is locked)";
                else if (xmpp_stanza_get_child_by_name(err_el, "item-not-found"))
                    err = "room not found";
                else if (xmpp_stanza_get_child_by_name(err_el, "conflict"))
                    err = "conflict";
                else if (xmpp_stanza_get_child_by_name(err_el, "not-acceptable"))
                    err = "not acceptable";
            }
            const char *what = "operation";
            switch (info.kind)
            {
                case weechat::account::muc_owner_kind::config_get:   what = "fetch room config form"; break;
                case weechat::account::muc_owner_kind::config_set:   what = "submit room config";     break;
                case weechat::account::muc_owner_kind::destroy:      what = "destroy room";           break;
                case weechat::account::muc_owner_kind::aff_set:      what = "set affiliation";        break;
                case weechat::account::muc_owner_kind::aff_list:     what = "list affiliations";      break;
                case weechat::account::muc_owner_kind::register_get: what = "fetch registration";     break;
                case weechat::account::muc_owner_kind::register_set: what = "submit registration";   break;
            }
            auto ui = weechat::UiPort::for_buffer(out);
            ui->printf_error(fmt::format(
                "{}: {} failed: {}", WEECHAT_XMPP_PLUGIN_NAME, what, err));
            return true;
        }

        if (type && weechat_strcasecmp(type, "result") == 0)
        {
            auto ui = weechat::UiPort::for_buffer(out);
            switch (info.kind)
            {
                case weechat::account::muc_owner_kind::config_get:
                {
                    if (auto ch_it = account.channels.find(info.room_jid);
                        ch_it != account.channels.end())
                    {
                        auto& [_, ch] = *ch_it;
                        weechat::channel::room_config_form form;

                        xmpp_stanza_t *xdata = owner_q
                            ? xmpp_stanza_get_child_by_name_and_ns(
                                  owner_q, "x", "jabber:x:data")
                            : nullptr;
                        if (xdata)
                        {
                            if (const char *sid = xmpp_stanza_get_attribute(xdata, "sessionid"))
                                form.sessionid = sid;
                            xmpp_stanza_t *field = xmpp_stanza_get_child_by_name(xdata, "field");
                            while (field)
                            {
                                weechat::channel::room_config_field f;
                                if (const char *var = xmpp_stanza_get_attribute(field, "var"))
                                    f.var = var;
                                if (const char *typ = xmpp_stanza_get_attribute(field, "type"))
                                    f.type = typ;
                                if (const char *lbl = xmpp_stanza_get_attribute(field, "label"))
                                    f.label = lbl;
                                // xmpp_stanza_get_text_ptr only works on raw text
                                // nodes; <value> elements need get_text (see caps
                                // hash builder ~L3486).
                                for (xmpp_stanza_t *v = xmpp_stanza_get_children(field);
                                     v; v = xmpp_stanza_get_next(v))
                                {
                                    const char *vname = xmpp_stanza_get_name(v);
                                    if (!vname || weechat_strcasecmp(vname, "value") != 0)
                                        continue;
                                    f.values.push_back(stanza_element_text(v));
                                }
                                if (!f.var.empty())
                                    form.fields.push_back(std::move(f));
                                field = xmpp_stanza_get_next(field);
                            }
                        }

                        // If /setmodes --confirm is waiting on this form
                        // (no cached form was available at submit time),
                        // take the pending diff, apply it to the form,
                        // and send the submit. Single-shot apply.
                        auto pending = ch.take_pending_setmodes();
                        if (pending.has_value())
                        {
                            // Apply the diff in place.
                            for (int i = 0; i < 7; ++i)
                            {
                                if (pending->want_set[i] || pending->want_clear[i])
                                {
                                    // Map index → field var
                                    static constexpr const char *const vars[7] = {
                                        "muc#roomconfig_moderatedroom",
                                        "muc#roomconfig_membersonly",
                                        "muc#roomconfig_passwordprotectedroom",
                                        "muc#roomconfig_publicroom",
                                        "muc#roomconfig_persistentroom",
                                        "muc#roomconfig_whois",
                                        "muc#roomconfig_whois",
                                    };
                                    std::string val;
                                    if (i == 5 && pending->want_set[5])       val = "anyone";
                                    else if (i == 6 && pending->want_set[6])  val = "moderators";
                                    else if (i == 5 || i == 6)                val = "moderators"; // want_clear
                                    else if (i == 3)                          val = pending->want_set[i] ? "0" : "1"; // p=hidden
                                    else                                     val = pending->want_set[i] ? "1" : "0";
                                    for (auto &fld : form.fields)
                                    {
                                        if (fld.var == vars[i])
                                        {
                                            fld.values = { val };
                                            break;
                                        }
                                    }
                                }
                            }
                            // +k sets roomsecret; -k clears it (Prosody and
                            // others use roomsecret, not passwordprotectedroom).
                            if (pending->want_set[2] || pending->want_clear[2])
                            {
                                for (auto &fld : form.fields)
                                {
                                    if (fld.var == "muc#roomconfig_roomsecret")
                                    {
                                        fld.values = { pending->want_set[2]
                                                           ? pending->password
                                                           : std::string{} };
                                        break;
                                    }
                                }
                            }

                            ch.store_config_form(form);  // cache the mutated form

                            weechat::channel::prepare_room_config_submit(form);

                            // Build the submit.
                            stanza::xep0004::form submit("submit");
                            submit.add_hidden("FORM_TYPE",
                                "http://jabber.org/protocol/muc#roomconfig");
                            for (const auto &fld : form.fields)
                            {
                                if (!weechat::channel::include_room_config_field_in_submit(fld))
                                    continue;
                                stanza::xep0004::field fd(fld.var);
                                if (!fld.type.empty()) fd.type(fld.type);
                                for (const auto &v : fld.values)
                                    fd.value(v);
                                submit.add_field(fd);
                            }

                            std::string set_id = stanza::uuid(account.context);
                            weechat::account::muc_owner_query_info set_info{
                                info.room_jid,
                                out,
                                weechat::account::muc_owner_kind::config_set
                            };
                            account.muc_owner_queries[set_id] = set_info;

                            stanza::xep0045::xep0045owner::query q;
                            q.form(submit);
                            auto set_iq = stanza::iq().type("set")
                                              .to(info.room_jid).id(set_id);
                            set_iq.muc_owner(q);
                            account.connection.send(
                                set_iq.build(account.context).get());

                            ui->printf_network(fmt::format(
                                "{}: room config form fetched and "
                                "/setmodes diff applied — submit sent",
                                WEECHAT_XMPP_PLUGIN_NAME));
                            return true;
                        }

                        // No pending diff — just cache the form and
                        // tell the user.
                        ch.store_config_form(std::move(form));
                        ui->printf_network(fmt::format(
                            "{}: room config form cached for {} — "
                            "use /setmodes to apply changes",
                            WEECHAT_XMPP_PLUGIN_NAME, info.room_jid));
                    }
                    else
                    {
                        ui->printf_error(fmt::format(
                            "{}: room config received for {} but channel no longer exists",
                            WEECHAT_XMPP_PLUGIN_NAME, info.room_jid));
                    }
                    return true;
                }
                case weechat::account::muc_owner_kind::config_set:
                {
                    ui->printf_network(fmt::format(
                        "{}: room config submitted for {}",
                        WEECHAT_XMPP_PLUGIN_NAME, info.room_jid));
                    if (auto ch_it = account.channels.find(info.room_jid);
                        ch_it != account.channels.end())
                    {
                        auto& ch = ch_it->second;
                        ch.clear_config_form();
                        // Refresh disco#info so /modes and the status bar
                        // show the new state. Drop the idempotency marker
                        // so the query actually goes out, then send it.
                        account.muc_modes_fetched.erase(info.room_jid);
                        std::string disco_id = stanza::uuid(account.context);
                        account.muc_modes_queries[disco_id] = info.room_jid;
                        account.connection.send(
                            stanza::iq().type("get")
                                .to(info.room_jid).id(disco_id)
                                .xep0030().query()
                                .build(account.context).get());
                    }
                    return true;
                }
                case weechat::account::muc_owner_kind::destroy:
                {
                    ui->printf_network(fmt::format(
                        "{}: room {} destroyed",
                        WEECHAT_XMPP_PLUGIN_NAME, info.room_jid));
                    return true;
                }
                case weechat::account::muc_owner_kind::aff_set:
                {
                    ui->printf_network(fmt::format(
                        "{}: affiliation change applied for {}",
                        WEECHAT_XMPP_PLUGIN_NAME, info.room_jid));
                    return true;
                }
                case weechat::account::muc_owner_kind::aff_list:
                {
                    ui->printf_network(fmt::format(
                        "{} list for {}:", info.list_affiliation, info.room_jid));
                    for (const auto& item :
                         ::xmpp::parse_muc_admin_list_items(
                             ::xmpp::StanzaView{admin_q}))
                    {
                        const std::string_view jid = item.jid.empty()
                            ? std::string_view{"(no jid)"}
                            : std::string_view{item.jid};
                        const auto aff_suffix = (!item.affiliation.empty()
                            && item.affiliation != info.list_affiliation)
                            ? fmt::format(" ({})", item.affiliation) : std::string{};
                        ui->printf(fmt::format(
                            "  {}{}{}  {}{}{}{}",
                            weechat_color("chat_nick"), jid, weechat_color("reset"),
                            weechat_color("chat_value"), item.nick,
                            weechat_color("reset"), aff_suffix));
                    }
                    return true;
                }
                case weechat::account::muc_owner_kind::register_get:
                {
                    const ::xmpp::StanzaView xdata = register_q
                        ? ::xmpp::StanzaView{register_q}.child("x", "jabber:x:data")
                        : ::xmpp::StanzaView{};
                    const auto fields = ::xmpp::parse_muc_register_form_fields(xdata);

                    if (!info.register_nick.empty() && xdata.valid())
                    {
                        stanza::xep0004::form submit("submit");
                        submit.add_hidden("FORM_TYPE",
                            "http://jabber.org/protocol/muc#register");
                        for (const auto& field : fields)
                        {
                            stanza::xep0004::field fd(field.var);
                            if (!field.type.empty())
                                fd.type(field.type);
                            fd.value(field.var == "muc#register_roomnick"
                                ? info.register_nick : field.value);
                            submit.add_field(fd);
                        }

                        const std::string set_id = stanza::uuid(account.context);
                        account.muc_owner_queries[set_id] =
                            weechat::account::muc_owner_query_info{
                                info.room_jid,
                                out,
                                weechat::account::muc_owner_kind::register_set
                            };

                        stanza::xep0045register::query rq;
                        rq.form(submit);
                        auto set_iq = stanza::iq().type("set")
                                          .to(info.room_jid).id(set_id);
                        set_iq.muc_register(rq);
                        account.connection.send(set_iq.build(account.context).get());

                        ui->printf_network(fmt::format(
                            "{}: registration form submitted for {}",
                            WEECHAT_XMPP_PLUGIN_NAME, info.room_jid));
                        return true;
                    }

                    ui->printf_network(fmt::format(
                        "Registration info for {}:", info.room_jid));
                    for (const auto& field : fields)
                    {
                        ui->printf(fmt::format(
                            "  {}{}{}: {}{}{}",
                            weechat_color("chat_nick"),
                            field.label.empty() ? field.var : field.label,
                            weechat_color("reset"),
                            weechat_color("chat_value"),
                            field.value.empty() ? "(empty)" : field.value,
                            weechat_color("reset")));
                    }
                    return true;
                }
                case weechat::account::muc_owner_kind::register_set:
                {
                    ui->printf_network(fmt::format(
                        "{}: room registration submitted for {}",
                        WEECHAT_XMPP_PLUGIN_NAME, info.room_jid));
                    return true;
                }
            }
        }
        return true;
    }

    if (handle_disco_items_iq_event(stanza))
        return true;

    handle_adhoc_command_iq_event(stanza);

    if (handle_channel_search_iq_event(stanza))
        return true;

    if (handle_disco_info_iq_event(stanza))
        return true;

    
    // Handle roster (RFC 6121)
    query = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "query", "jabber:iq:roster");

    // Parse <group> children of a roster <item> into account.roster[jid].groups
    auto parse_roster_groups = [&](xmpp_stanza_t *item, const char *jid) {
        account.roster[jid].groups.clear();
        for (xmpp_stanza_t *g = xmpp_stanza_get_children(item);
             g; g = xmpp_stanza_get_next(g))
        {
            if (weechat_strcasecmp(xmpp_stanza_get_name(g), "group") != 0)
                continue;
            const std::string group_name = stanza_element_text(g);
            if (!group_name.empty())
                account.roster[jid].groups.push_back(group_name);
        }
    };

    if (query && type && weechat_strcasecmp(type, "result") == 0)
    {
        xmpp_stanza_t *item;
        for (item = xmpp_stanza_get_children(query);
             item; item = xmpp_stanza_get_next(item))
        {
            const char *name = xmpp_stanza_get_name(item);
            if (weechat_strcasecmp(name, "item") != 0)
                continue;

            const char *jid = xmpp_stanza_get_attribute(item, "jid");
            const char *roster_name = xmpp_stanza_get_attribute(item, "name");
            const char *subscription = xmpp_stanza_get_attribute(item, "subscription");

            if (!jid)
                continue;

            account.roster[jid].jid = jid;
            account.roster[jid].name = roster_name ? roster_name : "";
            account.roster[jid].subscription = subscription ? subscription : "none";
            parse_roster_groups(item, jid);
        }
        account.sync_roster_nicklist();
    }

    // RFC 6121 §2.1.6 — roster push: server sends IQ type="set" with a single item
    if (query && type && weechat_strcasecmp(type, "set") == 0)
    {
        xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(query, "item");
        if (item)
        {
            const char *jid          = xmpp_stanza_get_attribute(item, "jid");
            const char *roster_name  = xmpp_stanza_get_attribute(item, "name");
            const char *subscription = xmpp_stanza_get_attribute(item, "subscription");

            if (jid)
            {
                if (subscription && weechat_strcasecmp(subscription, "remove") == 0)
                {
                    account.roster.erase(jid);
                    if (weechat::user *removed = weechat::user::search(&account, jid))
                        removed->nicklist_remove(&account, nullptr);
                    weechat::UiPort::for_buffer(account.buffer)->printf_network(
                        fmt::format("Roster: {} removed", jid));
                }
                else
                {
                    bool is_new = !account.roster.contains(jid);
                    account.roster[jid].jid = jid;
                    account.roster[jid].name = roster_name ? roster_name : "";
                    account.roster[jid].subscription = subscription ? subscription : "none";
                    parse_roster_groups(item, jid);

                    auto roster_ui = weechat::UiPort::for_buffer(account.buffer);
                    if (is_new)
                        roster_ui->printf_network(fmt::format(
                            "Roster: {} added ({})",
                            jid, subscription ? subscription : "none"));
                    else
                        roster_ui->printf_network(fmt::format(
                            "Roster: {} updated (subscription: {})",
                            jid, subscription ? subscription : "none"));

                    account.update_roster_nicklist_entry(jid);
                }
            }
        }
        // Acknowledge the roster push
        account.connection.send(stanza::iq()
            .type("result")
            .id(id ? id : "")
            .to(from ? from : "")
            .from(to ? to : "")
            .build(account.context)
            .get());
        return true;
    }
    
    handle_bookmarks_iq_event(stanza);

    if (handle_omemo_pubsub_iq_event(stanza, own_jid))
        return true;

    if (handle_mam_fin_iq_event(stanza))
        return true;

    return true;
}
