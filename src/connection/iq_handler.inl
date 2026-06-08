bool weechat::connection::iq_handler(xmpp_stanza_t *stanza, bool top_level)
{
    // SM counter incremented in libstrophe wrapper, not here
    // top_level parameter kept for nested/recursive calls

    (void) top_level;
    append_raw_xml_trace(account, "RECV", stanza);

    xmpp_stanza_t *reply, *query;
    xmpp_stanza_t         *storage, *conference, *nick;

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

    // XEP-0441: MAM Preferences — handle <prefs xmlns='urn:xmpp:mam:2'> result/error
    if (id && account.mam_prefs_queries.contains(id))
    {
        struct t_gui_buffer *prefs_buf = account.mam_prefs_queries[id];
        if (!prefs_buf) prefs_buf = account.buffer;
        account.mam_prefs_queries.erase(id);

        xmpp_stanza_t *prefs_el = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "prefs", "urn:xmpp:mam:2");

        if (type && weechat_strcasecmp(type, "error") == 0)
        {
            xmpp_stanza_t *err = xmpp_stanza_get_child_by_name(stanza, "error");
            const char *err_type = err ? xmpp_stanza_get_attribute(err, "type") : "unknown";
            weechat_printf(prefs_buf,
                "%sMAM preferences: server returned error (%s) — feature may not be supported",
                weechat_prefix("error"), err_type);
        }
        else if (prefs_el)
        {
            const char *def = xmpp_stanza_get_attribute(prefs_el, "default");
            weechat_printf(prefs_buf,
                "%sMAM preferences: default=%s%s%s",
                weechat_prefix("network"),
                weechat_color("bold"), def ? def : "(unset)", weechat_color("-bold"));

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
                weechat_printf(prefs_buf, "%s  always: %s",
                    weechat_prefix("network"),
                    jids_str.empty() ? "(empty)" : jids_str.c_str());
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
                weechat_printf(prefs_buf, "%s  never:  %s",
                    weechat_prefix("network"),
                    jids_str.empty() ? "(empty)" : jids_str.c_str());
            }

            weechat_printf(prefs_buf, "%sMAM preferences updated successfully",
                weechat_prefix("network"));
        }
        return true;
    }

    // Handle vCard responses (XEP-0054)
    xmpp_stanza_t *vcard = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "vCard", "vcard-temp");
    if (vcard && type && weechat_strcasecmp(type, "result") == 0)
    {
        const char *from_jid = from ? from : own_jid.c_str();

        // Check if this is a /setvcard read-merge response (self-fetch before update).
        if (id)
        {
            if (auto sv_it = account.setvcard_queries.find(id); sv_it != account.setvcard_queries.end())
            {
                auto& [_, sv] = *sv_it;
                struct t_gui_buffer *sv_buf = sv.buffer;

                // Build a vcard_fields struct pre-populated from the server's vCard.
                auto ctext = [&](xmpp_stanza_t *parent, const char *name) -> std::string {
                    xmpp_stanza_t *ch = xmpp_stanza_get_child_by_name(parent, name);
                    return ch ? stanza_element_text(ch) : std::string {};
                };
                ::xmpp::xep0054::vcard_fields f;
                f.fn       = ctext(vcard, "FN");
                f.nickname = ctext(vcard, "NICKNAME");
                f.url      = ctext(vcard, "URL");
                f.desc     = ctext(vcard, "DESC");
                f.bday     = ctext(vcard, "BDAY");
                f.note     = ctext(vcard, "NOTE");
                f.title    = ctext(vcard, "TITLE");
                {
                    xmpp_stanza_t *org_el = xmpp_stanza_get_child_by_name(vcard, "ORG");
                    if (org_el) f.org = ctext(org_el, "ORGNAME");
                }
                {
                    xmpp_stanza_t *email_el = xmpp_stanza_get_child_by_name(vcard, "EMAIL");
                    if (email_el) f.email = ctext(email_el, "USERID");
                }
                {
                    xmpp_stanza_t *tel_el = xmpp_stanza_get_child_by_name(vcard, "TEL");
                    if (tel_el) f.tel = ctext(tel_el, "NUMBER");
                }

                // Apply the requested override.
                const std::string &fld = sv.field;
                const std::string &val = sv.value;
                if      (fld == "fn")       f.fn       = val;
                else if (fld == "nickname") f.nickname = val;
                else if (fld == "email")    f.email    = val;
                else if (fld == "url")      f.url      = val;
                else if (fld == "desc")     f.desc     = val;
                else if (fld == "org")      f.org      = val;
                else if (fld == "title")    f.title    = val;
                else if (fld == "tel")      f.tel      = val;
                else if (fld == "bday")     f.bday     = val;
                else if (fld == "note")     f.note     = val;

                // Publish the merged vCard.
                xmpp_stanza_t *set_iq = ::xmpp::xep0054::vcard_set(account.context, f);
                account.connection.send(set_iq);
                xmpp_stanza_release(set_iq);
                weechat_printf(sv_buf, "%svCard field %s updated",
                               weechat_prefix("network"), fld.c_str());

                account.setvcard_queries.erase(sv_it);
                return true;
            }
        }

        // Determine which buffer to print into: the one that issued /whois, or
        // the account buffer for auto-fetched vCards (XEP-0153 trigger).
        struct t_gui_buffer *target_buf = account.buffer;
        bool is_whois = false;
        if (id)
        {
            if (auto it = account.whois_queries.find(id); it != account.whois_queries.end())
            {
                auto& [_, w] = *it;
                target_buf = w.buffer;
                account.whois_queries.erase(it);
                is_whois = true;
            }
        }

        // Helper: get direct text content of a child element
        auto child_text = [&](xmpp_stanza_t *parent, const char *name) -> std::string {
            xmpp_stanza_t *child = xmpp_stanza_get_child_by_name(parent, name);
            return child ? stanza_element_text(child) : std::string {};
        };

        // Helper: print a labelled line only if value is non-empty
        auto print_field = [&](const char *label, const std::string &val) {
            if (!val.empty())
                weechat_printf(target_buf, "  %s%s%s %s",
                               weechat_color("bold"), label,
                               weechat_color("reset"), val.c_str());
        };

        if (is_whois)
        {
            weechat_printf(target_buf, "%svCard for %s:",
                           weechat_prefix("network"), from_jid);
        }
        else
        {
            XDEBUG("vCard auto-fetched for {}", from_jid);
        }

        std::string fn       = child_text(vcard, "FN");
        std::string nickname = child_text(vcard, "NICKNAME");
        std::string url      = child_text(vcard, "URL");
        std::string desc     = child_text(vcard, "DESC");
        std::string bday     = child_text(vcard, "BDAY");
        std::string note     = child_text(vcard, "NOTE");
        std::string jabbid   = child_text(vcard, "JABBERID");
        std::string title    = child_text(vcard, "TITLE");
        std::string role_vc  = child_text(vcard, "ROLE");

        // ORG: <ORG><ORGNAME>…</ORGNAME></ORG>
        std::string org;
        xmpp_stanza_t *org_el = xmpp_stanza_get_child_by_name(vcard, "ORG");
        if (org_el) org = child_text(org_el, "ORGNAME");

        // EMAIL: <EMAIL><USERID>…</USERID></EMAIL>  (first occurrence)
        std::string email_val;
        xmpp_stanza_t *email_el = xmpp_stanza_get_child_by_name(vcard, "EMAIL");
        if (email_el) email_val = child_text(email_el, "USERID");

        // TEL: <TEL><NUMBER>…</NUMBER></TEL>  (first occurrence)
        std::string tel;
        xmpp_stanza_t *tel_el = xmpp_stanza_get_child_by_name(vcard, "TEL");
        if (tel_el) tel = child_text(tel_el, "NUMBER");

        // ADR: <ADR><STREET>…</STREET><LOCALITY>…</LOCALITY><CTRY>…</CTRY></ADR>
        std::string adr;
        xmpp_stanza_t *adr_el = xmpp_stanza_get_child_by_name(vcard, "ADR");
        if (adr_el)
        {
            for (const char *part : {"STREET", "LOCALITY", "REGION", "PCODE", "CTRY"})
            {
                std::string p = child_text(adr_el, part);
                if (!p.empty())
                {
                    if (!adr.empty()) adr += ", ";
                    adr += p;
                }
            }
        }

        if (is_whois)
        {
            print_field("Full name:",    fn);
            print_field("Nickname:",     nickname);
            print_field("Birthday:",     bday);
            print_field("Organisation:", org);
            print_field("Title:",        title);
            print_field("Role:",         role_vc);
            print_field("Email:",        email_val);
            print_field("Phone:",        tel);
            print_field("Address:",      adr);
            print_field("URL:",          url);
            print_field("JID:",          jabbid);
            print_field("Note:",         note);
            print_field("Description:",  desc);
        }

        // Store into user profile for future reference
        weechat::user *u = weechat::user::search(&account, from_jid);
        if (u)
        {
            if (!fn.empty())       u->profile.fn        = fn;
            if (!nickname.empty()) u->profile.nickname  = nickname;
            if (!email_val.empty()) u->profile.email    = email_val;
            if (!url.empty())      u->profile.url       = url;
            if (!desc.empty())     u->profile.description = desc;
            if (!org.empty())      u->profile.org       = org;
            if (!title.empty())    u->profile.title     = title;
            if (!tel.empty())      u->profile.tel       = tel;
            if (!bday.empty())     u->profile.bday      = bday;
            if (!note.empty())     u->profile.note      = note;
            if (!jabbid.empty())   u->profile.jabberid  = jabbid;
            u->profile.vcard_fetched = true;
        }

        return true;
    }

    // Handle vCard4 PubSub responses (XEP-0292)
    // Arrives as: <iq type='result'><pubsub xmlns='..pubsub'><items node='urn:xmpp:vcard4'>
    //               <item id='current'><vcard xmlns='urn:ietf:params:xml:ns:vcard-4.0'>…</vcard>
    {
        xmpp_stanza_t *pubsub_vc4 = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "pubsub", "http://jabber.org/protocol/pubsub");
        if (pubsub_vc4 && type && weechat_strcasecmp(type, "result") == 0)
        {
            xmpp_stanza_t *items = xmpp_stanza_get_child_by_name(pubsub_vc4, "items");
            if (items)
            {
                const char *node = xmpp_stanza_get_attribute(items, "node");
                if (node && std::string_view(node) == NS_VCARD4_PUBSUB)
                {
                    const char *from_jid = from ? from : own_jid.c_str();

                    if (id)
                        account.whois_queries.erase(id);

                    xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(items, "item");
                    if (item)
                    {
                        xmpp_stanza_t *vcard4 = xmpp_stanza_get_child_by_name_and_ns(
                            item, "vcard", NS_VCARD4);
                        if (vcard4)
                        {
                            XDEBUG("vCard4 auto-fetched for {}", from_jid);
                            return true;
                        }
                    }
                }
            }
        }
    }

    if (handle_avatar_pubsub_iq_event(stanza, own_jid))
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

        // XEP-0060: pubsub publish error — report to the originating buffer.
        {
            if (auto pub_it = account.pubsub_publish_ids.find(id); pub_it != account.pubsub_publish_ids.end())
            {
                auto& [_, ctx] = *pub_it;
                xmpp_stanza_t *error_elem = xmpp_stanza_get_child_by_name(stanza, "error");
                const std::string error_cond = error_elem
                    ? ::xmpp::iq_error_text(::xmpp::StanzaView(error_elem)) : "unknown error";
                struct t_gui_buffer *err_buf = ctx.buffer ? ctx.buffer : account.buffer;
                weechat_printf(err_buf,
                    "%s%s: publish failed for %s/%s (item %s): %s",
                    weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                    ctx.service.c_str(), ctx.node.c_str(), ctx.item_id.c_str(),
                    error_cond.c_str());
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
                weechat_printf(fb,
                    "%s%s: subscribe to %s failed: %s",
                    weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                    sub.feed_key.c_str(), error_cond.c_str());
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
                weechat_printf(fb,
                    "%s%s: unsubscribe from %s failed: %s",
                    weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                    unsub.feed_key.c_str(), error_cond.c_str());
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

                weechat_printf(err_buf,
                    "%s%s: cannot fetch feed %s/%s: %s",
                    weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                    err_service.c_str(), err_node.c_str(),
                    error_cond.c_str());

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
            if (item)
            {
                weechat_printf(account.buffer, "%sBlocked JIDs:",
                              weechat_prefix("network"));
                while (item)
                {
                    const char *jid = xmpp_stanza_get_attribute(item, "jid");
                    if (jid)
                        weechat_printf(account.buffer, "  %s", jid);
                    item = xmpp_stanza_get_next(item);
                }
            }
            else
            {
                weechat_printf(account.buffer, "%sNo JIDs blocked",
                              weechat_prefix("network"));
            }
        }

        return true;
    }
    
    if (block && type && weechat_strcasecmp(type, "result") == 0)
    {
        weechat_printf(account.buffer, "%sBlock request successful",
                      weechat_prefix("network"));
        return true;
    }
    
    if (unblock && type && weechat_strcasecmp(type, "result") == 0)
    {
        weechat_printf(account.buffer, "%sUnblock request successful",
                      weechat_prefix("network"));
        return true;
    }

    // XEP-0191: server-pushed block/unblock IQ sets (§8.4, §8.5)
    if (block && type && weechat_strcasecmp(type, "set") == 0)
    {
        xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(block, "item");
        while (item)
        {
            const char *jid = xmpp_stanza_get_attribute(item, "jid");
            if (jid)
                weechat_printf(account.buffer, "%s%s was blocked",
                               weechat_prefix("network"), jid);
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
        xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(unblock, "item");
        if (item)
        {
            while (item)
            {
                const char *jid = xmpp_stanza_get_attribute(item, "jid");
                if (jid)
                    weechat_printf(account.buffer, "%s%s was unblocked",
                                   weechat_prefix("network"), jid);
                item = xmpp_stanza_get_next(item);
            }
        }
        else
        {
            weechat_printf(account.buffer, "%sAll JIDs unblocked",
                           weechat_prefix("network"));
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
                case weechat::account::muc_owner_kind::config_get:  what = "fetch room config form"; break;
                case weechat::account::muc_owner_kind::config_set:  what = "submit room config";    break;
                case weechat::account::muc_owner_kind::destroy:     what = "destroy room";          break;
                case weechat::account::muc_owner_kind::aff_set:     what = "set affiliation";       break;
            }
            weechat_printf(out, "%s%s: %s failed: %s",
                           weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                           what, err.c_str());
            return true;
        }

        if (type && weechat_strcasecmp(type, "result") == 0)
        {
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

                            weechat_printf(out, "%s%s: room config form fetched and "
                                           "/setmodes diff applied — submit sent",
                                           weechat_prefix("network"),
                                           WEECHAT_XMPP_PLUGIN_NAME);
                            return true;
                        }

                        // No pending diff — just cache the form and
                        // tell the user.
                        ch.store_config_form(std::move(form));
                        weechat_printf(out, "%s%s: room config form cached for %s — "
                                       "use /setmodes to apply changes",
                                       weechat_prefix("network"),
                                       WEECHAT_XMPP_PLUGIN_NAME,
                                       info.room_jid.c_str());
                    }
                    else
                    {
                        weechat_printf(out, "%s%s: room config received for %s but channel no longer exists",
                                       weechat_prefix("error"),
                                       WEECHAT_XMPP_PLUGIN_NAME,
                                       info.room_jid.c_str());
                    }
                    return true;
                }
                case weechat::account::muc_owner_kind::config_set:
                {
                    weechat_printf(out, "%s%s: room config submitted for %s",
                                   weechat_prefix("network"),
                                   WEECHAT_XMPP_PLUGIN_NAME,
                                   info.room_jid.c_str());
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
                    weechat_printf(out, "%s%s: room %s destroyed",
                                   weechat_prefix("network"),
                                   WEECHAT_XMPP_PLUGIN_NAME,
                                   info.room_jid.c_str());
                    return true;
                }
                case weechat::account::muc_owner_kind::aff_set:
                {
                    weechat_printf(out, "%s%s: affiliation change applied for %s",
                                   weechat_prefix("network"),
                                   WEECHAT_XMPP_PLUGIN_NAME,
                                   info.room_jid.c_str());
                    return true;
                }
            }
        }
        return true;
    }

    // XEP-0030: Service Discovery - disco#items response
    xmpp_stanza_t *items_query = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "query", "http://jabber.org/protocol/disco#items");
    
    if (items_query && type && weechat_strcasecmp(type, "result") == 0)
    {
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
                            if (nick && real_jid)
                            {
                                ch.add_member(nick, nullptr,
                                    std::optional<std::string_view>(real_jid));
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
        bool is_commands_node = items_node
            && std::string_view(items_node) == "http://jabber.org/protocol/commands";
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
                    static constexpr std::string_view comments_prefix =
                        "urn:xmpp:microblog:0:comments/";
                    if (node_name.starts_with(comments_prefix))
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
                    this->send(stanza::iq()
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
    }

    // XEP-0050: Ad-Hoc Commands — handle command execute/form result (type=result/error)
    xmpp_stanza_t *adhoc_command = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "command", "http://jabber.org/protocol/commands");
    if (adhoc_command && type)
    {
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

    // XEP-0433: Extended Channel Search — handle <result> or <search> IQ responses
    {
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
    }

    query = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "query", "http://jabber.org/protocol/disco#info");
    if (query && type)
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

                // Accept "http://weechat.org" (bare) or "http://weechat.org#<hash>"
                std::string_view req(requested_node);
                node_ok = (req == "http://weechat.org") ||
                          (computed_hash &&
                           req == std::string("http://weechat.org#") + *computed_hash);
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

            // Extract features for capability caching
            std::vector<std::string> features;
            xmpp_stanza_t *feature = xmpp_stanza_get_child_by_name(query, "feature");
            while (feature)
            {
                const char *var = xmpp_stanza_get_attribute(feature, "var");
                if (var)
                    features.push_back(var);
                feature = xmpp_stanza_get_next(feature);
            }

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
                // XEP-0115 §5.4: verify the hash before caching to prevent caps poisoning.
                // Reconstruct the hash from identities + features + x-data forms.
                std::string ver_hash = account.caps_disco_queries[stanza_id];
                account.caps_disco_queries.erase(stanza_id);

                // --- build serial string S per §5.1 ---
                std::string S;

                // xmpp_stanza_get_next() returns the next sibling regardless of name,
                // so we must filter by element name to avoid picking up <feature>/<x> etc.
                auto next_named = [](xmpp_stanza_t *st, const char *name) -> xmpp_stanza_t * {
                    for (st = xmpp_stanza_get_next(st); st; st = xmpp_stanza_get_next(st)) {
                        const char *n = xmpp_stanza_get_name(st);
                        if (n && std::string_view(n) == name) return st;
                    }
                    return nullptr;
                };

                // Step 2-3: collect identities, sort by "category/type/lang/name"
                std::vector<std::string> identities;
                for (xmpp_stanza_t *id_elem = xmpp_stanza_get_child_by_name(query, "identity");
                     id_elem;
                     id_elem = next_named(id_elem, "identity"))
                {
                    const char *cat  = xmpp_stanza_get_attribute(id_elem, "category");
                    const char *typ  = xmpp_stanza_get_attribute(id_elem, "type");
                    // libstrophe strips the "xml:" namespace prefix when storing
                    // attributes, so xml:lang is stored under the key "lang".
                    const char *lang = xmpp_stanza_get_attribute(id_elem, "lang");
                    const char *name = xmpp_stanza_get_attribute(id_elem, "name");
                    identities.push_back(
                        std::string(cat  ? cat  : "") + "/" +
                        std::string(typ  ? typ  : "") + "/" +
                        std::string(lang ? lang : "") + "/" +
                        std::string(name ? name : ""));
                }
                std::ranges::sort(identities);
                std::ranges::for_each(identities, [&](const auto& ident){ S += ident + "<"; });

                // Step 4-5: features already collected above; sort and append
                std::vector<std::string> sorted_features = features;
                std::ranges::sort(sorted_features);
                std::ranges::for_each(sorted_features, [&](const auto& feat){ S += feat + "<"; });

                // Step 6-7: x-data forms (XEP-0128)
                struct FormData {
                    std::string form_type;
                    // fields: var → sorted values  (FORM_TYPE excluded)
                    std::vector<std::pair<std::string, std::vector<std::string>>> fields;
                };
                std::vector<FormData> forms;
                for (xmpp_stanza_t *x_elem = xmpp_stanza_get_child_by_name(query, "x");
                     x_elem; x_elem = next_named(x_elem, "x"))
                {
                    const char *xmlns_x = xmpp_stanza_get_attribute(x_elem, "xmlns");
                    if (xmlns_x && std::string_view(xmlns_x) == "jabber:x:data")
                    {
                        FormData fd;
                        bool has_form_type = false;
                        // find FORM_TYPE value (XEP-0115 §5: skip form if missing or not hidden)
                        // NOTE: xmpp_stanza_get_text_ptr only works on raw XMPP_STANZA_TEXT
                        // nodes; for element nodes like <value> use stanza_element_text().
                        for (xmpp_stanza_t *f = xmpp_stanza_get_child_by_name(x_elem, "field");
                             f; f = next_named(f, "field"))
                        {
                            const char *fvar = xmpp_stanza_get_attribute(f, "var");
                            if (fvar && std::string_view(fvar) == "FORM_TYPE")
                            {
                                xmpp_stanza_t *vnode = xmpp_stanza_get_child_by_name(f, "value");
                                if (vnode)
                                    fd.form_type = stanza_element_text(vnode);
                                has_form_type = true;
                                break;
                            }
                        }
                        // XEP-0115 §5: if no FORM_TYPE field, ignore this form entirely
                        if (!has_form_type)
                            continue;
                        // collect non-FORM_TYPE fields
                        for (xmpp_stanza_t *f = xmpp_stanza_get_child_by_name(x_elem, "field");
                             f; f = next_named(f, "field"))
                        {
                            const char *fvar = xmpp_stanza_get_attribute(f, "var");
                            if (fvar && std::string_view(fvar) != "FORM_TYPE")
                            {
                                std::vector<std::string> vals;
                                for (xmpp_stanza_t *vnode = xmpp_stanza_get_child_by_name(f, "value");
                                     vnode; vnode = next_named(vnode, "value"))
                                {
                                    // Always include the value, even if empty: an empty
                                    // <value></value> must contribute "" to the S string
                                    // (e.g. Psi sends os_version with an empty value).
                                    vals.push_back(stanza_element_text(vnode));
                                }
                                std::ranges::sort(vals);
                                fd.fields.emplace_back(fvar, std::move(vals));
                            }
                        }
                        std::ranges::sort(fd.fields,
                                  [](const auto &a, const auto &b){ return a.first < b.first; });
                        forms.push_back(std::move(fd));
                    }
                }
                // sort forms by FORM_TYPE
                std::ranges::sort(forms,
                          [](const FormData &a, const FormData &b){ return a.form_type < b.form_type; });
                for (const auto &form : forms)
                {
                    S += form.form_type + "<";
                    for (const auto &[fvar, fvals] : form.fields)
                    {
                        S += fvar + "<";
                        for (const auto &val : fvals)
                            S += val + "<";
                    }
                }

                // --- hash S with SHA-1 and base64-encode ---
                XDEBUG("caps: S string for {} (len={}): '{}'",
                       from ? from : "?", S.size(), S);
                unsigned char digest[20];
                unsigned int  digest_len = sizeof(digest);
                std::span<const char> S_span = S;
                EVP_Digest(S_span.data(), S_span.size(), digest, &digest_len, EVP_sha1(), nullptr);

                const int enc_size = 4 * static_cast<int>((digest_len + 2) / 3) + 1;
                std::string computed(static_cast<std::size_t>(enc_size), '\0');
                const int written = weechat_string_base_encode(
                    "64", reinterpret_cast<const char *>(digest),
                    static_cast<int>(digest_len), computed.data());
                if (written > 0)
                    computed.resize(static_cast<std::size_t>(written));
                else
                    computed.clear();

                if (computed == ver_hash)
                {
                    account.caps_cache_save(ver_hash, features);
                }
                else
                {
                    XDEBUG("caps: hash mismatch for {}: got '{}' expected '{}'; discarding",
                           from ? from : "?", computed, ver_hash);
                }
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

                // XEP-0060: record pubsub service components discovered at
                // connect time so /feed discover can list them without the
                // user having to know the service JID in advance.
                if (upload_disco && category == "pubsub")
                {
                    const std::string &svc_jid = upload_service_jid.empty()
                        ? (from ? std::string(from) : std::string{})
                        : upload_service_jid;
                    if (!svc_jid.empty())
                    {
                        auto &kps = account.known_pubsub_services;
                        if (!std::ranges::contains(kps, svc_jid))
                            kps.push_back(svc_jid);
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
                    weechat_printf(account.buffer, "%sRoster: %s removed",
                                   weechat_prefix("network"), jid);
                }
                else
                {
                    bool is_new = !account.roster.contains(jid);
                    account.roster[jid].jid = jid;
                    account.roster[jid].name = roster_name ? roster_name : "";
                    account.roster[jid].subscription = subscription ? subscription : "none";
                    parse_roster_groups(item, jid);

                    if (is_new)
                        weechat_printf(account.buffer, "%sRoster: %s added (%s)",
                                       weechat_prefix("network"), jid,
                                       subscription ? subscription : "none");
                    else
                        weechat_printf(account.buffer, "%sRoster: %s updated (subscription: %s)",
                                       weechat_prefix("network"), jid,
                                       subscription ? subscription : "none");

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
    
    query = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "query", "jabber:iq:private");
    // BUG 1 fix: only process jabber:iq:private results, not gets/sets/errors.
    // An <iq type='error'> from a server that doesn't support XEP-0049 would
    // otherwise clear our bookmarks and crash on the autojoin nullptr deref below.
    if (query && type && weechat_strcasecmp(type, "result") == 0)
    {
        storage = xmpp_stanza_get_child_by_name_and_ns(
                query, "storage", "storage:bookmarks");
        if (storage)
        {
            // BUG 5 fix: only clear XEP-0048 bookmarks; preserve any entries
            // already populated by a XEP-0402 PEP push that arrived first.
            // We remove only entries whose source is XEP-0048 (i.e. those not
            // already in the map, which is empty on first connect and may have
            // been populated by the PEP push).  Simplest safe approach: clear
            // only if the map is empty (PEP push hasn't run yet); otherwise
            // we merge — the XEP-0049 data wins per-JID via operator[].
            if (account.bookmarks.empty())
                account.bookmarks.clear();

            for (conference = xmpp_stanza_get_children(storage);
                 conference; conference = xmpp_stanza_get_next(conference))
            {
                const char *name = xmpp_stanza_get_name(conference);
                if (weechat_strcasecmp(name, "conference") != 0)
                    continue;

                const char *jid = xmpp_stanza_get_attribute(conference, "jid");
                const char *autojoin = xmpp_stanza_get_attribute(conference, "autojoin");
                name = xmpp_stanza_get_attribute(conference, "name");
                nick = xmpp_stanza_get_child_by_name(conference, "nick");
                const std::string bookmark_nick = nick ? stanza_element_text(nick) : std::string {};

                if (!jid)
                    continue;

                // Store bookmark
                account.bookmarks[jid].jid = jid;
                account.bookmarks[jid].name = name ? name : "";
                account.bookmarks[jid].nick = bookmark_nick;
                account.bookmarks[jid].autojoin = autojoin
                    && (weechat_strcasecmp(autojoin, "true") == 0
                        || weechat_strcasecmp(autojoin, "1") == 0);

                account.connection.send(stanza::iq()
                            .from(to)
                            .to(jid)
                            .type("get")
                            .id(stanza::uuid(account.context))
                            .xep0030()
                            .query()
                            .build(account.context)
                            .get());
                // BUG 2 fix: autojoin attr may be absent (nullptr); use the already
                // computed bookmarks[jid].autojoin flag which is null-safe.
                if (account.bookmarks[jid].autojoin)
                {
                    // Skip autojoin for biboumi (IRC gateway) rooms
                    // Biboumi JIDs typically contain % (e.g., #channel%irc.server.org@gateway)
                    // or have 'biboumi' in the server component
                    std::string_view jid_sv(jid);
                    bool is_biboumi = jid_sv.contains('%') ||
                                      jid_sv.contains("biboumi") ||
                                      jid_sv.contains("@irc.");

                    if (is_biboumi)
                    {
                        weechat_printf(account.buffer,
                                      "%sSkipping autojoin for IRC gateway room: %s",
                                      weechat_prefix("network"), jid);
                    }
                    else
                    {
                        std::string cmd = fmt::format("/enter {}{}{}",
                                                      jid,
                                                      bookmark_nick.empty() ? "" : "/",
                                                      bookmark_nick);
                        weechat_command(account.buffer, cmd.c_str());
                        struct t_gui_buffer *ptr_buffer = nullptr;
                        if (auto ptr_channel = account.channels.find(jid); ptr_channel != account.channels.end())
                        {
                            auto& [_, ch] = *ptr_channel;
                            ptr_buffer = ch.buffer;
                            if (ptr_buffer)
                                ch.update_name(name);
                        }
                    }
                }
            }
        }
    }

    if (handle_omemo_pubsub_iq_event(stanza, own_jid))
        return true;

    if (handle_mam_fin_iq_event(stanza))
        return true;

    return true;
}
