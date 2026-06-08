bool weechat::connection::iq_handler(xmpp_stanza_t *stanza, bool top_level)
{
    // SM counter incremented in libstrophe wrapper, not here
    // top_level parameter kept for nested/recursive calls

    (void) top_level;
    append_raw_xml_trace(account, "RECV", stanza);

    xmpp_stanza_t *query;
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
