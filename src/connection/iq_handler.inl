bool weechat::connection::iq_handler(xmpp_stanza_t *stanza, bool top_level)
{
    // SM counter incremented in libstrophe wrapper, not here
    // top_level parameter kept for nested/recursive calls

    (void) top_level;
    append_raw_xml_trace(account, "RECV", stanza);

    xmpp_stanza_t *reply, *query, *text, *fin;
    xmpp_stanza_t         *pubsub, *items, *item;
    xmpp_stanza_t         *storage, *conference, *nick;

    auto binding = xml::iq(account.context, stanza);
    const char *id = xmpp_stanza_get_id(stanza);
    const char *from = xmpp_stanza_get_from(stanza);
    const char *to = xmpp_stanza_get_to(stanza);
    const char *type = xmpp_stanza_get_attribute(stanza, "type");

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
        std::string pub_service   = pub_it->second.service;
        std::string pub_node      = pub_it->second.node;
        std::string pub_item_id   = pub_it->second.item_id;
        bool        pub_is_retract = pub_it->second.is_retract;
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
    
    // Handle XMPP Ping (XEP-0199)
    xmpp_stanza_t *ping = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "ping", "urn:xmpp:ping");
    if (ping && type && weechat_strcasecmp(type, "get") == 0)
    {
        // Respond with iq result
        account.connection.send(stanza::iq()
            .type("result")
            .id(id ? id : "")
            .to(from ? from : "")
            .from(to ? to : "")
            .build(account.context)
            .get());
        return true;
    }
    
    // Handle ping responses (XEP-0199 and XEP-0410)
    if (type && (weechat_strcasecmp(type, "result") == 0 || weechat_strcasecmp(type, "error") == 0))
    {
        const char *stanza_id = xmpp_stanza_get_id(stanza);
        auto ping_it = stanza_id ? account.user_ping_queries.find(stanza_id)
                                 : account.user_ping_queries.end();
        if (ping_it != account.user_ping_queries.end())
        {
            time_t start_time = ping_it->second;
            time_t now = time(nullptr);
            long rtt_ms = (now - start_time) * 1000;  // Convert to milliseconds

            account.user_ping_queries.erase(ping_it);

            const char *from_jid = from ? from : account.jid().data();

            // Check if this is a MUC self-ping (XEP-0410)
            bool is_muc_selfping = false;
            std::string room_jid;
            if (from)
            {
                std::string from_str(from);
                size_t slash_pos = from_str.find('/');
                if (slash_pos != std::string::npos)
                {
                    room_jid = from_str.substr(0, slash_pos);
                    std::string resource = from_str.substr(slash_pos + 1);

                    // Check if this is our own nickname in a MUC
                    if (resource == account.nickname())
                    {
                        // Check if we have a channel for this room
                        if (account.channels.find(room_jid) != account.channels.end())
                        {
                            is_muc_selfping = true;
                        }
                    }
                }
            }

            if (weechat_strcasecmp(type, "result") == 0)
            {
                if (is_muc_selfping)
                {
                    weechat_printf(account.buffer, "%sMUC self-ping OK: still in %s",
                                  weechat_prefix("network"), room_jid.c_str());
                }
                else
                {
                    weechat_printf(account.buffer, "%sPong from %s (RTT: %ld ms)",
                                  weechat_prefix("network"), from_jid, rtt_ms);
                }
            }
            else
            {
                // Error response
                xmpp_stanza_t *err_elem = xmpp_stanza_get_child_by_name(stanza, "error");
                if (is_muc_selfping)
                {
                    // XEP-0410 §3.3: interpret error condition to decide whether we
                    // are still joined or need to rejoin.
                    //
                    // Still joined (reflected-ping errors from MUC on behalf of a
                    // client that doesn't support XMPP Ping, or nick-change race):
                    //   <service-unavailable/>, <feature-not-implemented/>, <item-not-found/>
                    //
                    // Not joined — must rejoin:
                    //   <not-acceptable/> (§3.2 "you are not in the room")
                    //   <not-allowed/>, <bad-request/> (footnote 4 — some servers)
                    //
                    // Network errors — ambiguous, treat conservatively (no rejoin):
                    //   <remote-server-not-found/>, <remote-server-timeout/>

                    bool still_joined = false;
                    bool ambiguous    = false;

                    if (err_elem)
                    {
                        static constexpr const char *IETF_NS =
                            "urn:ietf:params:xml:ns:xmpp-stanzas";

                        auto has_cond = [&](const char *name) -> bool {
                            return xmpp_stanza_get_child_by_name_and_ns(
                                       err_elem, name, IETF_NS) != nullptr;
                        };

                        if (has_cond("service-unavailable") ||
                            has_cond("feature-not-implemented") ||
                            has_cond("item-not-found"))
                        {
                            still_joined = true;
                        }
                        else if (has_cond("remote-server-not-found") ||
                                 has_cond("remote-server-timeout"))
                        {
                            ambiguous = true;
                        }
                        // not-acceptable / not-allowed / bad-request / anything else
                        // → not joined → fall through to rejoin below
                    }

                    if (still_joined)
                    {
                        // Reflected ping: the pinged client doesn't support XMPP Ping
                        // (or nick-change race) — we ARE still in the room.
                        weechat_printf(account.buffer,
                                       "%sMUC self-ping: still in %s (reflected error)",
                                       weechat_prefix("network"), room_jid.c_str());
                    }
                    else if (ambiguous)
                    {
                        // Network-level error: cannot determine join state; skip rejoin.
                        const char *etype = err_elem
                            ? xmpp_stanza_get_attribute(err_elem, "type") : "unknown";
                        weechat_printf(account.buffer,
                                       "%sMUC self-ping to %s: network error (%s), skipping rejoin",
                                       weechat_prefix("network"), room_jid.c_str(), etype);
                    }
                    else
                    {
                        // <not-acceptable/> or similar: we are NOT in the room.
                        weechat_printf(account.buffer,
                                       "%sMUC self-ping FAILED: no longer in %s — rejoining",
                                       weechat_prefix("error"), room_jid.c_str());

                        // Rejoin: send a fresh MUC join presence (room@domain/nick).
                        std::string rejoin_jid = fmt::format("{}/{}", room_jid,
                                                              account.nickname());
                        auto join_pres = stanza::presence()
                            .to(rejoin_jid.c_str())
                            .from(account.jid());
                        static_cast<stanza::xep0045::presence&>(join_pres).muc_join();
                        account.connection.send(join_pres.build(account.context).get());
                    }
                }
                else
                {
                    const char *error_type = err_elem
                        ? xmpp_stanza_get_attribute(err_elem, "type") : "unknown";
                    weechat_printf(account.buffer, "%sPing failed to %s: %s",
                                  weechat_prefix("error"), from_jid, error_type);
            }
        }

    return true;
}
    }

    // XEP-0441: MAM Preferences — handle <prefs xmlns='urn:xmpp:mam:2'> result/error
    if (id && account.mam_prefs_queries.count(id))
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
                    char *jid_txt = xmpp_stanza_get_text(jid_el);
                    if (jid_txt)
                    {
                        if (!jids_str.empty()) jids_str += ", ";
                        jids_str += jid_txt;
                        xmpp_free(account.context, jid_txt);
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
                    char *jid_txt = xmpp_stanza_get_text(jid_el);
                    if (jid_txt)
                    {
                        if (!jids_str.empty()) jids_str += ", ";
                        jids_str += jid_txt;
                        xmpp_free(account.context, jid_txt);
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
        const char *from_jid = from ? from : account.jid().data();

        // Check if this is a /setvcard read-merge response (self-fetch before update).
        if (id)
        {
            auto sv_it = account.setvcard_queries.find(id);
            if (sv_it != account.setvcard_queries.end())
            {
                auto &sv = sv_it->second;
                struct t_gui_buffer *sv_buf = sv.buffer;

                // Build a vcard_fields struct pre-populated from the server's vCard.
                auto ctext = [&](xmpp_stanza_t *parent, const char *name) -> std::string {
                    xmpp_stanza_t *ch = xmpp_stanza_get_child_by_name(parent, name);
                    if (!ch) return {};
                    char *txt = xmpp_stanza_get_text(ch);
                    if (!txt) return {};
                    std::string s(txt); xmpp_free(account.context, txt); return s;
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
            auto it = account.whois_queries.find(id);
            if (it != account.whois_queries.end())
            {
                target_buf = it->second.buffer;
                account.whois_queries.erase(it);
                is_whois = true;
            }
        }

        // Helper: get direct text content of a child element
        auto child_text = [&](xmpp_stanza_t *parent, const char *name) -> std::string {
            xmpp_stanza_t *child = xmpp_stanza_get_child_by_name(parent, name);
            if (!child) return {};
            char *txt = xmpp_stanza_get_text(child);
            if (!txt) return {};
            std::string s(txt);
            xmpp_free(account.context, txt);
            return s;
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
                    const char *from_jid = from ? from : account.jid().data();

                    struct t_gui_buffer *target_buf = account.buffer;
                    bool is_whois4 = false;
                    if (id)
                    {
                        auto it = account.whois_queries.find(id);
                        if (it != account.whois_queries.end())
                        {
                            target_buf = it->second.buffer;
                            account.whois_queries.erase(it);
                            is_whois4 = true;
                        }
                    }

                    xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(items, "item");
                    if (item)
                    {
                        xmpp_stanza_t *vcard4 = xmpp_stanza_get_child_by_name_and_ns(
                            item, "vcard", NS_VCARD4);
                        if (vcard4)
                        {
                            if (is_whois4)
                                weechat_printf(target_buf, "%svCard4 for %s:",
                                               weechat_prefix("network"), from_jid);
                            else
                                XDEBUG("vCard4 auto-fetched for {}", from_jid);

                            // Helper: get text of first child matching name inside parent
                            auto vc4_text = [&](xmpp_stanza_t *p, const char *name) -> std::string {
                                xmpp_stanza_t *el = xmpp_stanza_get_child_by_name(p, name);
                                if (!el) return {};
                                // vCard4 wraps values: <text>…</text> or <uri>…</uri>
                                xmpp_stanza_t *val = xmpp_stanza_get_child_by_name(el, "text");
                                if (!val) val = xmpp_stanza_get_child_by_name(el, "uri");
                                if (!val) return {};
                                char *t = xmpp_stanza_get_text(val);
                                if (!t) return {};
                                std::string s(t);
                                xmpp_free(account.context, t);
                                return s;
                            };

                            auto print_vc4 = [&](const char *label, const std::string &val) {
                                if (!val.empty())
                                    weechat_printf(target_buf, "  %s%s%s %s",
                                                   weechat_color("bold"), label,
                                                   weechat_color("reset"), val.c_str());
                            };

                            // vCard4 uses lowercase element names
                            std::string fn       = vc4_text(vcard4, "fn");
                            std::string nickname = vc4_text(vcard4, "nickname");
                            std::string url      = vc4_text(vcard4, "url");
                            std::string note     = vc4_text(vcard4, "note");
                            std::string bday     = vc4_text(vcard4, "bday");
                            std::string title    = vc4_text(vcard4, "title");
                            std::string role_vc4 = vc4_text(vcard4, "role");

                            // email: <email><text>…</text></email>
                            std::string email_v4 = vc4_text(vcard4, "email");

                            // tel: <tel><uri>tel:…</uri></tel>
                            std::string tel_v4 = vc4_text(vcard4, "tel");

                            // org: <org><text>…</text></org>
                            std::string org_v4 = vc4_text(vcard4, "org");

                            if (is_whois4)
                            {
                                print_vc4("Full name:",    fn);
                                print_vc4("Nickname:",     nickname);
                                print_vc4("Birthday:",     bday);
                                print_vc4("Organisation:", org_v4);
                                print_vc4("Title:",        title);
                                print_vc4("Role:",         role_vc4);
                                print_vc4("Email:",        email_v4);
                                print_vc4("Phone:",        tel_v4);
                                print_vc4("URL:",          url);
                                print_vc4("Note:",         note);
                            }

                            return true;
                        }
                    }
                }
            }
        }
    }

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

            auto fetch_it = account.pubsub_fetch_ids.find(id);
            if (fetch_it != account.pubsub_fetch_ids.end())
            {
                std::string feed_service    = fetch_it->second.service;
                std::string node_name       = fetch_it->second.node;
                std::string before_cursor   = fetch_it->second.before_cursor;
                int         max_items_req   = fetch_it->second.max_items;
                account.pubsub_fetch_ids.erase(fetch_it);

                std::string feed_key = fmt::format("{}/{}", feed_service, node_name);
                auto [ch_it, inserted] = account.channels.try_emplace(
                    feed_key,
                    account,
                    weechat::channel::chat_type::FEED,
                    feed_key,
                    feed_key);
                if (inserted)
                    account.feed_open_register(feed_key);
                {
                    weechat::channel &feed_ch = ch_it->second;

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
                            char          *count_text = count_el ? xmpp_stanza_get_text(count_el) : nullptr;
                            rsm_total_count = count_text ? std::atoi(count_text) : -1;
                            if (count_text) xmpp_free(account.context, count_text);

                            xmpp_stanza_t *first_el   = xmpp_stanza_get_child_by_name(rsm_set, "first");
                            char          *first_text  = first_el ? xmpp_stanza_get_text(first_el) : nullptr;

                            // Detect stale page: the server returned the oldest-first page
                            // (index=0) even though there are more items than max_items.
                            // Heuristic: if the first item's ID parses as a timestamp older
                            // than 30 days AND the node has more items than we requested,
                            // this is a stale result (e.g. gadgeteerza-tech-blog on
                            // news.movim.eu which sorts oldest-first).
                            if (first_text && rsm_total_count > max_items_req)
                            {
                                // Try to parse the item-id as an ISO 8601 timestamp.
                                // news.movim.eu uses timestamps as item IDs.
                                struct tm tm_first = {};
                                bool parsed = false;
                                // Try full timestamp with fractional seconds: 2023-11-09T10:52:18.590034Z
                                // strptime doesn't handle sub-seconds; truncate at '.'.
                                std::string id_str(first_text);
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
                                    int first_index = index_attr ? std::atoi(index_attr) : -1;
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

                            if (first_text) xmpp_free(account.context, first_text);
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
                        xmpp_ctx_t *ctx = account.context;
                        auto item_pubdate = [ctx](xmpp_stanza_t *item) -> std::string {
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
                                char *t = xmpp_stanza_get_text(el);
                                if (!t) continue;
                                std::string s(t);
                                xmpp_free(ctx, t);
                                if (!s.empty()) return s;
                            }
                            return {};
                        };
                        std::vector<xmpp_stanza_t *> item_vec;
                        for (xmpp_stanza_t *item = xmpp_stanza_get_children(items);
                             item; item = xmpp_stanza_get_next(item))
                            item_vec.push_back(item);
                        std::stable_sort(item_vec.begin(), item_vec.end(),
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
                            if (item_id_raw)
                            {
                                std::string_view iid(item_id_raw);
                                if (iid.starts_with("urn:xmpp:avatar:")
                                    || iid.starts_with("urn:xmpp:omemo:")
                                    || iid.starts_with("urn:xmpp:bookmarks:"))
                                    continue;
                            }

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
                                        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                            "%sFeed title:%s %s",
                                            weechat_prefix("network"),
                                            weechat_color("reset"),
                                            af.title.c_str());
                                    }
                                    if (!af.author.empty())
                                        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                            "  %sAuthor:%s %s",
                                            weechat_color("darkgray"),
                                            weechat_color("reset"),
                                            af.author.c_str());
                                    if (!af.subtitle.empty())
                                        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                            "  %s", af.subtitle.c_str());
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
                            {
                                // No Atom entry — not a microblog post, skip silently.
                                // (Non-Atom items with unknown id prefixes land here.)
                                continue;
                            }

                            // Deduplication: only suppress items that arrive via
                            // push (message_handler).  IQ results are explicit
                            // user fetches (/feed …) so always render them.
                            // mark_seen is still called so push duplicates are
                            // suppressed after the fetch.

                            {
                                const std::string &title    = ae.title;
                                const std::string &pubdate  = ae.pubdate;
                                const std::string &author   = ae.author;
                                const std::string &reply_to = ae.reply_to;
                                const std::string &via_link = ae.via_link;
                                const std::string &replies_link = ae.replies_link;
                                const std::string &geoloc = ae.geoloc;
                                const std::string &body = ae.body();
                                const char *pfx  = weechat_prefix("join");
                                const char *bold = weechat_color("bold");
                                const char *rst  = weechat_color("reset");
                                const char *dim  = weechat_color("darkgray");
                                const char *grn  = weechat_color("green");

                                // Use Atom <link rel="alternate"> when present; fall back to
                                // the canonical XEP-0060 item URI. Only shown when no alias.
                                std::string link = ae.link;
                                if (link.empty() && item_id_raw && *item_id_raw)
                                    link = fmt::format("xmpp:{}?;node={};item={}",
                                                       feed_service, node_name, item_id_raw);

                                // Short alias prefix shown before the title, e.g. "#3 ".
                                std::string alias_pfx;
                                if (item_alias > 0)
                                    alias_pfx = fmt::format("#{}", item_alias);

                                // Header line.  When the entry has an explicit title we
                                // display it prominently in bold.  When there is no title
                                // (microblog / social post) we emit only the alias + metadata
                                // so the body line carries the content without duplication.
                                if (!title.empty())
                                {
                                    if (!author.empty() && !pubdate.empty())
                                        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                            "%s%s%s%s %s%s%s  [%s%s%s] — %s",
                                            pfx,
                                            alias_pfx.empty() ? "" : grn,
                                            alias_pfx.c_str(),
                                            alias_pfx.empty() ? "" : rst,
                                            bold, title.c_str(), rst,
                                            dim, author.c_str(), rst,
                                            pubdate.c_str());
                                    else if (!author.empty())
                                        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                            "%s%s%s%s %s%s%s  [%s%s%s]",
                                            pfx,
                                            alias_pfx.empty() ? "" : grn,
                                            alias_pfx.c_str(),
                                            alias_pfx.empty() ? "" : rst,
                                            bold, title.c_str(), rst,
                                            dim, author.c_str(), rst);
                                    else if (!pubdate.empty())
                                        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                            "%s%s%s%s %s%s%s — %s",
                                            pfx,
                                            alias_pfx.empty() ? "" : grn,
                                            alias_pfx.c_str(),
                                            alias_pfx.empty() ? "" : rst,
                                            bold, title.c_str(), rst,
                                            pubdate.c_str());
                                    else
                                        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                            "%s%s%s%s %s%s%s",
                                            pfx,
                                            alias_pfx.empty() ? "" : grn,
                                            alias_pfx.c_str(),
                                            alias_pfx.empty() ? "" : rst,
                                            bold, title.c_str(), rst);
                                }
                                else
                                {
                                    // No title: metadata-only header line.
                                    if (!author.empty() && !pubdate.empty())
                                        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                            "%s%s%s%s  [%s%s%s] — %s",
                                            pfx,
                                            alias_pfx.empty() ? "" : grn,
                                            alias_pfx.c_str(),
                                            alias_pfx.empty() ? "" : rst,
                                            dim, author.c_str(), rst,
                                            pubdate.c_str());
                                    else if (!author.empty())
                                        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                            "%s%s%s%s  [%s%s%s]",
                                            pfx,
                                            alias_pfx.empty() ? "" : grn,
                                            alias_pfx.c_str(),
                                            alias_pfx.empty() ? "" : rst,
                                            dim, author.c_str(), rst);
                                    else if (!pubdate.empty())
                                        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                            "%s%s%s%s  — %s",
                                            pfx,
                                            alias_pfx.empty() ? "" : grn,
                                            alias_pfx.c_str(),
                                            alias_pfx.empty() ? "" : rst,
                                            pubdate.c_str());
                                    // else: alias-only line already shown by the alias_pfx branch below,
                                    // or nothing to print; body line still follows.
                                }

                                if (!reply_to.empty())
                                {
                                    // Extract item UUID from xmpp: URI (item=<uuid>) and
                                    // resolve to an alias if possible; fall back to UUID.
                                    std::string reply_label;
                                    auto item_eq = reply_to.rfind("item=");
                                    if (item_eq != std::string::npos)
                                    {
                                        std::string reply_uuid = reply_to.substr(item_eq + 5);
                                        int ralias = account.feed_alias_lookup(feed_key, reply_uuid);
                                        if (ralias > 0)
                                            reply_label = fmt::format("#{}", ralias);
                                        else
                                            reply_label = reply_uuid;
                                    }
                                    else
                                        reply_label = reply_to;
                                    weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                        "  %sIn reply to:%s %s",
                                        dim, rst, reply_label.c_str());
                                }

                                if (!via_link.empty())
                                    weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                        "  %sRepeated from:%s %s",
                                        dim, rst, via_link.c_str());

                                if (!link.empty())
                                    weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                        "  %s", link.c_str());

                                // Comments: suppress raw URI — user uses /feed comments #N or
                                // /feed comments <item-id> when no alias is assigned yet.
                                if (!replies_link.empty())
                                {
                                    const std::string &comments_ref =
                                        alias_pfx.empty()
                                            ? (item_id_raw ? std::string(item_id_raw) : std::string())
                                            : alias_pfx;
                                    if (!comments_ref.empty())
                                    {
                                        if (ae.comments_count >= 0)
                                            weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                                "  %sComments (%d):%s /feed comments %s",
                                                dim, ae.comments_count, rst, comments_ref.c_str());
                                        else
                                            weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                                "  %sComments:%s /feed comments %s",
                                                dim, rst, comments_ref.c_str());
                                    }
                                }

                                if (!ae.categories.empty())
                                {
                                    std::string tags;
                                    for (size_t i = 0; i < ae.categories.size(); ++i)
                                    {
                                        if (i) tags += ", ";
                                        tags += ae.categories[i];
                                    }
                                    weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                        "  %sTags:%s %s",
                                        dim, rst, tags.c_str());
                                }

                                for (const auto &enclosure : ae.enclosures)
                                    weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                        "  %sAttachment:%s %s",
                                        dim, rst, enclosure.c_str());

                                for (const auto &att : ae.attachments)
                                {
                                    bool is_image = !att.media_type.empty() &&
                                                    att.media_type.rfind("image/", 0) == 0;
                                    bool is_video = !att.media_type.empty() &&
                                                    att.media_type.rfind("video/", 0) == 0;
                                    std::string kind_str = (att.disposition == "attachment") ? "File"
                                                         : is_image ? "Image"
                                                         : is_video ? "Video"
                                                         : "Media";
                                    std::string size_str;
                                    if (att.size > 0)
                                    {
                                        if (att.size >= 1024*1024)
                                            size_str = fmt::format("{:.1f} MB", att.size / (1024.0*1024.0));
                                        else if (att.size >= 1024)
                                            size_str = fmt::format("{:.1f} KB", att.size / 1024.0);
                                        else
                                            size_str = fmt::format("{} B", att.size);
                                    }
                                    std::string meta;
                                    if (!att.media_type.empty()) meta += att.media_type;
                                    if (!size_str.empty()) meta += (meta.empty() ? "" : ", ") + size_str;
                                    weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                        "  %s[%s: %s%s] %s%s",
                                        dim, kind_str.c_str(), att.filename.c_str(),
                                        meta.empty() ? "" : (" (" + meta + ")").c_str(),
                                        att.url.c_str(), rst);
                                }

                                if (!geoloc.empty())
                                    weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                        "  %sLocation:%s %s",
                                        dim, rst, geoloc.c_str());

                                if (!body.empty())
                                {
                                    weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                        "  %s\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500%s",
                                        dim, rst);
                                    std::string_view bv(body);
                                    for (std::string_view::size_type pos = 0;;)
                                    {
                                        auto nl = bv.find('\n', pos);
                                        auto line = bv.substr(pos, nl == std::string_view::npos ? nl : nl - pos);
                                        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                            "  %.*s", static_cast<int>(line.size()), line.data());
                                        if (nl == std::string_view::npos) break;
                                        pos = nl + 1;
                                    }
                                    weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none", "");
                                }
                            }
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
                            xmpp_stanza_t *first_el2  = xmpp_stanza_get_child_by_name(rsm_set2, "first");
                            char          *first_text2 = first_el2 ? xmpp_stanza_get_text(first_el2) : nullptr;
                            if (first_text2)
                            {
                                std::string hint = fmt::format(
                                    "/feed {} {} --before {}",
                                    feed_service, node_name, first_text2);
                                if (rsm_total_count > 0)
                                    weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                        "%s%d item(s) total — for older entries: %s",
                                        weechat_prefix("network"),
                                        rsm_total_count, hint.c_str());
                                else
                                    weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                        "%sFor older entries: %s",
                                        weechat_prefix("network"), hint.c_str());
                                xmpp_free(account.context, first_text2);
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
            auto subs_it = account.pubsub_subscriptions_queries.find(id);
            if (subs_it != account.pubsub_subscriptions_queries.end())
            {
                std::string feed_service = subs_it->second;
                account.pubsub_subscriptions_queries.erase(subs_it);

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

                        // XEP-0472 §4.1: skip per-post comment sub-nodes
                        static constexpr std::string_view comments_prefix =
                            "urn:xmpp:microblog:0:comments/";
                        if (node_name.starts_with(comments_prefix))
                            continue;

                        std::string feed_key = fmt::format("{}/{}", feed_service, node_name);

                        // Ensure FEED buffer exists
                        auto [sub_ch_it, sub_inserted] = account.channels.try_emplace(
                            feed_key,
                            account,
                            weechat::channel::chat_type::FEED,
                            feed_key,
                            feed_key);
                        if (sub_inserted)
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
                        this->send(stanza::iq()
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
                    weechat_printf(account.buffer,
                                   "%sNo subscribed feeds found on %s. "
                                   "Try: /feed %s --all",
                                   weechat_prefix("network"),
                                   feed_service.c_str(),
                                   feed_service.c_str());
                else
                    weechat_printf(account.buffer,
                                   "%sFeed subscriptions on %s: fetching %d subscribed node(s)",
                                   weechat_prefix("network"),
                                   feed_service.c_str(), node_count);
            }
        }
    }

    // XEP-0060: PubSub subscribe/unsubscribe result
    {
        xmpp_stanza_t *pubsub_res = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "pubsub", "http://jabber.org/protocol/pubsub");
        if (pubsub_res && id && type && weechat_strcasecmp(type, "result") == 0)
        {
            auto sub_ok_it = account.pubsub_subscribe_queries.find(id);
            if (sub_ok_it != account.pubsub_subscribe_queries.end())
            {
                struct t_gui_buffer *fb = sub_ok_it->second.buffer
                                         ? sub_ok_it->second.buffer : account.buffer;
                weechat_printf(fb,
                    "%sSubscribed to %s",
                    weechat_prefix("network"), sub_ok_it->second.feed_key.c_str());
                account.pubsub_subscribe_queries.erase(sub_ok_it);
            }

            auto unsub_ok_it = account.pubsub_unsubscribe_queries.find(id);
            if (unsub_ok_it != account.pubsub_unsubscribe_queries.end())
            {
                struct t_gui_buffer *fb = unsub_ok_it->second.buffer
                                         ? unsub_ok_it->second.buffer : account.buffer;
                weechat_printf(fb,
                    "%sUnsubscribed from %s",
                    weechat_prefix("network"), unsub_ok_it->second.feed_key.c_str());
                account.pubsub_unsubscribe_queries.erase(unsub_ok_it);
            }
        }
    }

    // XEP-0363: HTTP File Upload - handle upload slot response
    xmpp_stanza_t *slot = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "slot", "urn:xmpp:http:upload:0");
    
    if (slot && id && type && weechat_strcasecmp(type, "result") == 0)
    {
        XDEBUG("Upload slot response received (id: {})", id);
        
        auto req_it = account.upload_requests.find(id);
        if (req_it != account.upload_requests.end())
        {
            XDEBUG("Found matching upload request");
            // Extract PUT and GET URLs
            xmpp_stanza_t *put_elem = xmpp_stanza_get_child_by_name(slot, "put");
            xmpp_stanza_t *get_elem = xmpp_stanza_get_child_by_name(slot, "get");
            
            const char *put_url = put_elem ? xmpp_stanza_get_attribute(put_elem, "url") : nullptr;
            const char *get_url = get_elem ? xmpp_stanza_get_attribute(get_elem, "url") : nullptr;
            
            XDEBUG("PUT URL: {}", put_url ? put_url : "(null)");
            XDEBUG("GET URL: {}", get_url ? get_url : "(null)");
            
            // Extract PUT headers (XEP-0363 §9.2: only Authorization, Cookie, Expires
            // are permitted — forwarding arbitrary headers is a header-injection risk).
            // Also strip any CR/LF from values to prevent HTTP header injection.
            static constexpr std::string_view allowed_headers[] = {
                "authorization", "cookie", "expires"
            };
            std::vector<std::string> put_headers;
            if (put_elem)
            {
                xmpp_stanza_t *header = xmpp_stanza_get_child_by_name(put_elem, "header");
                while (header)
                {
                    const char *name = xmpp_stanza_get_attribute(header, "name");
                    char *value = xmpp_stanza_get_text(header);
                    if (name && value)
                    {
                        // Only forward headers explicitly listed in §9.2
                        bool allowed = false;
                        for (const auto &allowed_name : allowed_headers)
                        {
                            if (weechat_strcasecmp(name, allowed_name.data()) == 0)
                            {
                                allowed = true;
                                break;
                            }
                        }
                        if (allowed)
                        {
                            // Strip CR and LF to prevent HTTP header injection
                            std::string safe_value(value);
                            safe_value.erase(std::remove_if(safe_value.begin(), safe_value.end(),
                                [](char c) { return c == '\r' || c == '\n'; }),
                                safe_value.end());
                            std::string header_str = fmt::format("{}: {}", name, safe_value);
                            put_headers.push_back(header_str);
                            XDEBUG("PUT header: {}", header_str);
                        }
                        else
                        {
                            XDEBUG("PUT header '{}' not in XEP-0363 §9.2 allowlist — dropped", name);
                        }
                    }
                    if (value) xmpp_free(account.context, value);
                    header = xmpp_stanza_get_next(header);
                }
            }
            
            if (put_url && get_url)
            {
                weechat_printf(account.buffer, "%sUpload slot received, uploading file...",
                              weechat_prefix("network"));
                
                // Verify file exists and get file size
                FILE *file = fopen(req_it->second.filepath.c_str(), "rb");
                if (!file)
                {
                    weechat_printf(account.buffer, "%s%s: failed to open file for upload: %s",
                                  weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                                  req_it->second.filepath.c_str());
                    account.upload_requests.erase(req_it);
                    return 1;
                }
                fclose(file);
                
                // Get Content-Type from filename extension
                std::string filename = req_it->second.filename;
                std::string content_type = "application/octet-stream";
                size_t dot_pos = filename.find_last_of('.');
                if (dot_pos != std::string::npos)
                {
                    std::string ext = filename.substr(dot_pos + 1);
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                    
                    if (ext == "jpg" || ext == "jpeg") content_type = "image/jpeg";
                    else if (ext == "png") content_type = "image/png";
                    else if (ext == "gif") content_type = "image/gif";
                    else if (ext == "webp") content_type = "image/webp";
                    else if (ext == "mp4") content_type = "video/mp4";
                    else if (ext == "webm") content_type = "video/webm";
                    else if (ext == "pdf") content_type = "application/pdf";
                    else if (ext == "txt") content_type = "text/plain";
                }
                
                // Async HTTP PUT upload via pipe + worker thread.
                // The worker thread does all blocking I/O (file read, SHA-256,
                // curl PUT) and writes 1 byte to the pipe write-end when done.
                // weechat_hook_fd fires on the read-end in the main thread,
                // which processes the result and sends the XMPP message.

                int pipe_fds[2];
                if (pipe(pipe_fds) != 0)
                {
                    weechat_printf(account.buffer, "%s%s: failed to create pipe for upload",
                                  weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                    account.upload_requests.erase(req_it);
                    return 1;
                }

                // Build the completion context (everything the callback needs)
                auto ctx = std::make_shared<weechat::account::upload_completion>();
                ctx->channel_id    = req_it->second.channel_id;
                ctx->filename      = req_it->second.filename;
                ctx->content_type  = content_type;
                ctx->pipe_write_fd = pipe_fds[1];

                // XEP-0448: If the destination channel has OMEMO active, encrypt the file
                // before uploading so it arrives as an Encrypted File Share stanza.
                {
                    auto ch_it = account.channels.find(ctx->channel_id);
                    if (ch_it != account.channels.end() && ch_it->second.omemo.enabled)
                        ctx->encrypted = true;
                }

                // If this upload was triggered by an embed tag in a pending feed post,
                // mark the context so upload_fd_cb routes to the feed-post path.
                if (account.pending_feed_posts.count(id))
                    ctx->feed_post_upload_id = id;

                // Copy strings that will be used by the worker thread (the
                // upload_requests entry will be erased below, so we must copy
                // before erasing).
                std::string filepath_copy  = req_it->second.filepath;
                std::string put_url_copy   = put_url;
                std::string get_url_copy   = get_url;

                // Erase the upload_requests entry now (before thread starts)
                account.upload_requests.erase(req_it);

                // Register WeeChat hook on the read-end fd
                ctx->hook = weechat_hook_fd(pipe_fds[0], 1, 0, 0,
                                            &weechat::account::upload_fd_cb,
                                            &account, nullptr);

                // Store in pending_uploads keyed by read-end fd
                account.pending_uploads[pipe_fds[0]] = ctx;

                // Capture everything needed for the thread by value
                std::shared_ptr<weechat::account::upload_completion> ctx_copy = ctx;
                std::vector<std::string> put_headers_copy = put_headers;
                std::string content_type_copy = content_type;

                ctx->worker = std::thread([ctx_copy, filepath_copy,
                                           put_url_copy, get_url_copy,
                                           put_headers_copy, content_type_copy]()
                {
                    auto &c = *ctx_copy;

                    // Open file with RAII guard
                    auto file_deleter = [](FILE *f) { if (f) fclose(f); };
                    std::unique_ptr<FILE, decltype(file_deleter)>
                        upload_file_guard(fopen(filepath_copy.c_str(), "rb"), file_deleter);
                    FILE *upload_file = upload_file_guard.get();
                    if (!upload_file)
                    {
                        c.success   = false;
                        c.curl_error = "failed to open file";
                        ::write(c.pipe_write_fd, "x", 1);
                        return;
                    }

                    // Get file size — check fseek/ftell for non-seekable streams
                    if (fseek(upload_file, 0, SEEK_END) != 0)
                    {
                        c.success    = false;
                        c.curl_error = "failed to seek to end of file";
                        ::write(c.pipe_write_fd, "x", 1);
                        return;
                    }
                    long file_size = ftell(upload_file);
                    if (file_size < 0)
                    {
                        c.success    = false;
                        c.curl_error = "failed to determine file size";
                        ::write(c.pipe_write_fd, "x", 1);
                        return;
                    }
                    c.file_size = static_cast<size_t>(file_size);

                    // Close and reopen to get a fresh FILE* at position 0 for hashing.
                    // This avoids seeks on the upload handle and keeps stream state clean.
                    upload_file_guard.reset();
                    upload_file = nullptr;

                    // Calculate SHA-256 hash for SIMS — use a dedicated read handle.
                    unsigned char hash[EVP_MAX_MD_SIZE];
                    unsigned int  hash_len = 0;
                    {
                        std::unique_ptr<FILE, decltype(file_deleter)>
                            hash_file_guard(fopen(filepath_copy.c_str(), "rb"), file_deleter);
                        FILE *hash_file = hash_file_guard.get();
                        if (!hash_file)
                        {
                            c.success    = false;
                            c.curl_error = "failed to reopen file for hashing";
                            ::write(c.pipe_write_fd, "x", 1);
                            return;
                        }
                        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>
                            sha256_ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
                        EVP_DigestInit_ex(sha256_ctx.get(), EVP_sha256(), nullptr);
                        unsigned char buf[8192];
                        size_t bytes_read;
                        do {
                            bytes_read = fread(buf, 1, sizeof(buf), hash_file);
                            if (bytes_read > 0)
                                EVP_DigestUpdate(sha256_ctx.get(), buf, bytes_read);
                        } while (!feof(hash_file) && !ferror(hash_file));
                        EVP_DigestFinal_ex(sha256_ctx.get(), hash, &hash_len);
                    } // hash_file and sha256_ctx freed here

                    // Base64-encode the hash (RAII BIO chain)
                    {
                        std::unique_ptr<BIO, decltype(&BIO_free_all)>
                            bio_chain(BIO_push(BIO_new(BIO_f_base64()),
                                               BIO_new(BIO_s_mem())),
                                      BIO_free_all);
                        BIO_set_flags(bio_chain.get(), BIO_FLAGS_BASE64_NO_NL);
                        BIO_write(bio_chain.get(), hash, static_cast<int>(hash_len));
                        BIO_flush(bio_chain.get());
                        BUF_MEM *bptr;
                        BIO_get_mem_ptr(bio_chain.get(), &bptr);
                        c.sha256_hash = std::string(bptr->data, bptr->length);
                    } // bio_chain freed here

                    // Parse image dimensions from file header (JPEG / PNG).
                    // We only need the first few hundred bytes — open a short read.
                    {
                        std::unique_ptr<FILE, decltype(file_deleter)>
                            dim_file_guard(fopen(filepath_copy.c_str(), "rb"), file_deleter);
                        FILE *dim_file = dim_file_guard.get();
                        if (dim_file)
                        {
                            unsigned char hdr[24] = {};
                            size_t hdr_read = fread(hdr, 1, sizeof(hdr), dim_file);
                            if (hdr_read >= 8
                                && hdr[0] == 0x89 && hdr[1] == 'P' && hdr[2] == 'N'
                                && hdr[3] == 'G'  && hdr[4] == 0x0D&& hdr[5] == 0x0A
                                && hdr[6] == 0x1A && hdr[7] == 0x0A)
                            {
                                // PNG: IHDR chunk starts at byte 8 (4-byte length + "IHDR")
                                // Width at [16..19], Height at [20..23], big-endian.
                                if (hdr_read >= 24)
                                {
                                    c.image_width  = (static_cast<size_t>(hdr[16]) << 24)
                                                   | (static_cast<size_t>(hdr[17]) << 16)
                                                   | (static_cast<size_t>(hdr[18]) <<  8)
                                                   |  static_cast<size_t>(hdr[19]);
                                    c.image_height = (static_cast<size_t>(hdr[20]) << 24)
                                                   | (static_cast<size_t>(hdr[21]) << 16)
                                                   | (static_cast<size_t>(hdr[22]) <<  8)
                                                   |  static_cast<size_t>(hdr[23]);
                                }
                            }
                            else if (hdr_read >= 3
                                     && hdr[0] == 0xFF && hdr[1] == 0xD8 && hdr[2] == 0xFF)
                            {
                                // JPEG: scan for SOF0/SOF1/SOF2 markers (0xFF 0xC0-0xC2)
                                // which contain height and width.
                                unsigned char buf2[65536];
                                fseek(dim_file, 2, SEEK_SET);
                                size_t total = fread(buf2, 1, sizeof(buf2), dim_file);
                                for (size_t i = 0; i + 8 < total; ++i)
                                {
                                    if (buf2[i] == 0xFF
                                        && (buf2[i+1] == 0xC0
                                            || buf2[i+1] == 0xC1
                                            || buf2[i+1] == 0xC2))
                                    {
                                        // SOF marker: FF Cx LL LL PP HH HH WW WW
                                        // Height at [i+5..i+6], Width at [i+7..i+8]
                                        if (i + 8 < total)
                                        {
                                            c.image_height = (static_cast<size_t>(buf2[i+5]) << 8)
                                                            |  static_cast<size_t>(buf2[i+6]);
                                            c.image_width  = (static_cast<size_t>(buf2[i+7]) << 8)
                                            |  static_cast<size_t>(buf2[i+8]);
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                    } // dim_file closed here

                    // XEP-0448: AES-256-GCM encryption for OMEMO-enabled channels.
                    // When active: encrypt the plaintext file into a tmpfile, then
                    // upload the ciphertext (+ 16-byte GCM auth tag) in its place.
                    // The key, IV, and ciphertext hash are stored in the context for
                    // the callback to embed in the <encrypted xmlns='urn:xmpp:esfs:0'> stanza.
                    std::string esfs_tmpfile_path; // track tmp path for deletion
                    if (c.encrypted)
                    {
                        c.original_file_size = static_cast<size_t>(file_size);

                        // Generate 256-bit key and 96-bit (12-byte) IV.
                        unsigned char aes_key[32] = {};
                        unsigned char aes_iv[12]  = {};
                        if (RAND_bytes(aes_key, sizeof(aes_key)) != 1
                            || RAND_bytes(aes_iv, sizeof(aes_iv)) != 1)
                        {
                            c.success    = false;
                            c.curl_error = "esfs: RAND_bytes failed for key/IV generation";
                            ::write(c.pipe_write_fd, "x", 1);
                            return;
                        }

                        // Base64-encode key and IV using OpenSSL BIO chain (RAII).
                        auto b64_encode = [](const unsigned char *data, int len) -> std::string
                        {
                            std::unique_ptr<BIO, decltype(&BIO_free_all)>
                                b(BIO_push(BIO_new(BIO_f_base64()),
                                           BIO_new(BIO_s_mem())), BIO_free_all);
                            BIO_set_flags(b.get(), BIO_FLAGS_BASE64_NO_NL);
                            BIO_write(b.get(), data, len);
                            BIO_flush(b.get());
                            BUF_MEM *bptr;
                            BIO_get_mem_ptr(b.get(), &bptr);
                            return std::string(bptr->data, bptr->length);
                        };
                        c.esfs_key_b64 = b64_encode(aes_key, sizeof(aes_key));
                        c.esfs_iv_b64  = b64_encode(aes_iv, sizeof(aes_iv));

                        // Open plaintext source.
                        auto file_deleter_enc = [](FILE *f) { if (f) fclose(f); };
                        std::unique_ptr<FILE, decltype(file_deleter_enc)>
                            pt_guard(fopen(filepath_copy.c_str(), "rb"), file_deleter_enc);
                        if (!pt_guard)
                        {
                            c.success    = false;
                            c.curl_error = "esfs: failed to open plaintext for encryption";
                            ::write(c.pipe_write_fd, "x", 1);
                            return;
                        }

                        // Create a secure temporary file for ciphertext.
                        char tmp_tmpl[] = "/tmp/xepher-esfs-XXXXXX";
                        int tmp_fd = mkstemp(tmp_tmpl);
                        if (tmp_fd < 0)
                        {
                            c.success    = false;
                            c.curl_error = "esfs: mkstemp failed";
                            ::write(c.pipe_write_fd, "x", 1);
                            return;
                        }
                        esfs_tmpfile_path = tmp_tmpl;

                        // Wrap the raw fd in a FILE* (RAII via unique_ptr).
                        std::unique_ptr<FILE, decltype(file_deleter_enc)>
                            ct_guard(fdopen(tmp_fd, "wb"), file_deleter_enc);
                        if (!ct_guard)
                        {
                            close(tmp_fd);
                            ::unlink(esfs_tmpfile_path.c_str());
                            c.success    = false;
                            c.curl_error = "esfs: fdopen failed on tmpfile";
                            ::write(c.pipe_write_fd, "x", 1);
                            return;
                        }

                        // AES-256-GCM encryption using OpenSSL EVP.
                        std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>
                            enc_ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
                        if (!enc_ctx
                            || EVP_EncryptInit_ex(enc_ctx.get(), EVP_aes_256_gcm(),
                                                  nullptr, nullptr, nullptr) != 1
                            || EVP_CIPHER_CTX_ctrl(enc_ctx.get(), EVP_CTRL_GCM_SET_IVLEN,
                                                   12, nullptr) != 1
                            || EVP_EncryptInit_ex(enc_ctx.get(), nullptr,
                                                  nullptr, aes_key, aes_iv) != 1)
                        {
                            ::unlink(esfs_tmpfile_path.c_str());
                            c.success    = false;
                            c.curl_error = "esfs: EVP init failed";
                            ::write(c.pipe_write_fd, "x", 1);
                            return;
                        }

                        // Initialize SHA-256 over ciphertext for later hash field.
                        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>
                            ct_sha_ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
                        EVP_DigestInit_ex(ct_sha_ctx.get(), EVP_sha256(), nullptr);

                        unsigned char in_buf[8192];
                        unsigned char out_buf[8192 + 16]; // EVP may expand by block size
                        long ciphertext_len = 0;
                        bool enc_ok = true;
                        while (!feof(pt_guard.get()) && !ferror(pt_guard.get()))
                        {
                            size_t n = fread(in_buf, 1, sizeof(in_buf), pt_guard.get());
                            if (n == 0) break;
                            int out_len = 0;
                            if (EVP_EncryptUpdate(enc_ctx.get(), out_buf, &out_len,
                                                  in_buf, static_cast<int>(n)) != 1)
                            {
                                enc_ok = false;
                                break;
                            }
                            if (out_len > 0)
                            {
                                EVP_DigestUpdate(ct_sha_ctx.get(), out_buf,
                                                 static_cast<size_t>(out_len));
                                fwrite(out_buf, 1, static_cast<size_t>(out_len), ct_guard.get());
                                ciphertext_len += out_len;
                            }
                        }
                        if (!enc_ok)
                        {
                            ::unlink(esfs_tmpfile_path.c_str());
                            c.success    = false;
                            c.curl_error = "esfs: EVP_EncryptUpdate failed";
                            ::write(c.pipe_write_fd, "x", 1);
                            return;
                        }

                        // Finalize encryption (no output for GCM).
                        int final_len = 0;
                        EVP_EncryptFinal_ex(enc_ctx.get(), out_buf, &final_len);
                        if (final_len > 0)
                        {
                            EVP_DigestUpdate(ct_sha_ctx.get(), out_buf,
                                             static_cast<size_t>(final_len));
                            fwrite(out_buf, 1, static_cast<size_t>(final_len), ct_guard.get());
                            ciphertext_len += final_len;
                        }

                        // Append 16-byte GCM authentication tag.
                        unsigned char tag[16] = {};
                        EVP_CIPHER_CTX_ctrl(enc_ctx.get(), EVP_CTRL_GCM_GET_TAG, 16, tag);
                        EVP_DigestUpdate(ct_sha_ctx.get(), tag, sizeof(tag));
                        fwrite(tag, 1, sizeof(tag), ct_guard.get());
                        ciphertext_len += 16;

                        // Hash of ciphertext (including tag).
                        unsigned char ct_hash[EVP_MAX_MD_SIZE];
                        unsigned int  ct_hash_len = 0;
                        EVP_DigestFinal_ex(ct_sha_ctx.get(), ct_hash, &ct_hash_len);
                        c.esfs_cipher_hash_b64 = b64_encode(ct_hash, static_cast<int>(ct_hash_len));

                        // Flush + close tmp write handle; fopen again for reading.
                        ct_guard.reset(); // closes fdopen'd FILE* (which closes tmp_fd)

                        file_size = ciphertext_len;
                        // Replace upload_file_guard with a read handle on the tmpfile.
                        upload_file_guard.reset(fopen(esfs_tmpfile_path.c_str(), "rb"));
                        upload_file = upload_file_guard.get();
                        if (!upload_file)
                        {
                            ::unlink(esfs_tmpfile_path.c_str());
                            c.success    = false;
                            c.curl_error = "esfs: failed to reopen tmpfile for upload";
                            ::write(c.pipe_write_fd, "x", 1);
                            return;
                        }
                        // Update file_size in context for <file> metadata (ciphertext size).
                        c.file_size = static_cast<size_t>(file_size);
                    }
                    else
                    {
                        // Open a fresh handle for upload (avoids any seek/EOF state issues).
                        upload_file_guard.reset(fopen(filepath_copy.c_str(), "rb"));
                        upload_file = upload_file_guard.get();
                        if (!upload_file)
                        {
                            c.success    = false;
                            c.curl_error = "failed to reopen file for upload";
                            ::write(c.pipe_write_fd, "x", 1);
                            return;
                        }
                    }

                    // Initialize curl
                    CURL *curl = curl_easy_init();
                    if (!curl)
                    {
                        // upload_file_guard will close the file on return
                        c.success    = false;
                        c.curl_error = "failed to initialize curl";
                        ::write(c.pipe_write_fd, "x", 1);
                        return;
                    }

                    // Required for multithreaded use: suppress SIGALRM/SIGPIPE
                    // delivery from within libcurl (e.g. OpenSSL alarm-based
                    // DNS timeouts, or SIGPIPE on broken connections).
                    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

                    curl_easy_setopt(curl, CURLOPT_URL, put_url_copy.c_str());
                    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
                    curl_easy_setopt(curl, CURLOPT_READDATA, upload_file);
                    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,
                                     static_cast<curl_off_t>(file_size));

                    // Discard the response body (e.g. HTML error pages from
                    // 4xx/5xx responses) — we only care about the HTTP status code.
                    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                        +[](char *, size_t size, size_t nmemb, void *) -> size_t {
                            return size * nmemb;
                        });

                    struct curl_slist *headers = nullptr;
                    headers = curl_slist_append(
                        headers,
                        fmt::format("Content-Type: {}", content_type_copy).c_str());
                    for (const auto &hdr : put_headers_copy)
                        headers = curl_slist_append(headers, hdr.c_str());
                    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

                    CURLcode res = curl_easy_perform(curl);

                    long http_code = 0;
                    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

                    curl_slist_free_all(headers);
                    curl_easy_cleanup(curl);
                    // upload_file_guard closes the file on scope exit; no manual fclose needed

                    // XEP-0448: remove temporary ciphertext file after upload.
                    if (!esfs_tmpfile_path.empty())
                        ::unlink(esfs_tmpfile_path.c_str());

                    c.http_code = http_code;
                    c.get_url   = get_url_copy;
                    if (res != CURLE_OK)
                    {
                        c.success    = false;
                        c.curl_error = curl_easy_strerror(res);
                    }
                    else if (http_code != 201)
                    {
                        c.success    = false;
                        c.curl_error = fmt::format("HTTP {}", http_code);
                    }
                    else
                    {
                        c.success = true;
                    }

                    // Signal the main thread
                    ::write(c.pipe_write_fd, "x", 1);
                });
            }
            else
            {
                weechat_printf(account.buffer, "%s%s: upload failed - missing PUT or GET URL",
                              weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME);
                account.upload_requests.erase(req_it);
            }
        }
        else
        {
            weechat_printf(account.buffer, "%s%s: upload slot response for unknown request ID: %s",
                          weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, id);
        }
    }
    else if (id && account.upload_requests.count(id))
    {
        weechat_printf(account.buffer, "%s%s: upload slot response malformed or wrong type (type: %s)",
                      weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME, type ? type : "(null)");
    }
    
    // XEP-0060: clean up publish tracking on bare <iq type='result'/> (no pubsub child)
    // and trigger a single-item re-fetch so the buffer updates immediately.
    if (id && type && weechat_strcasecmp(type, "result") == 0)
        trigger_publish_refetch(id);

    // XEP-0363: HTTP File Upload - handle upload slot errors
    if (id && type && weechat_strcasecmp(type, "error") == 0)
    {
        weechat::account::mam_query failed_mam_query;
        if (account.mam_query_search(&failed_mam_query, id))
        {
            const bool is_global_query = failed_mam_query.with.empty();
            weechat_printf(account.buffer,
                           "%sMAM query %s failed (IQ error) — ending catchup%s",
                           weechat_prefix("error"),
                           failed_mam_query.id.c_str(),
                           is_global_query ? " and flushing deferred OMEMO key-transports" : "");
            account.mam_query_remove(failed_mam_query.id);
            if (is_global_query)
            {
                account.omemo.global_mam_catchup = false;
                account.omemo.process_postponed_key_transports(account);
                account.omemo.process_postponed_bundle_republish(account);
            }
        }

        auto req_it = account.upload_requests.find(id);
        if (req_it != account.upload_requests.end())
        {
            xmpp_stanza_t *error_elem = xmpp_stanza_get_child_by_name(stanza, "error");
            const char *error_type = error_elem ? xmpp_stanza_get_attribute(error_elem, "type") : nullptr;
            
            // Try to get error text
            std::string error_msg = "Upload slot request failed";
            if (error_elem)
            {
                xmpp_stanza_t *text_elem = xmpp_stanza_get_child_by_name(error_elem, "text");
                if (text_elem)
                {
                    char *text = xmpp_stanza_get_text(text_elem);
                    if (text)
                    {
                        error_msg = fmt::format("Upload slot request failed: {}", text);
                        xmpp_free(account.context, text);
                    }
                }
                else
                {
                    // Try to get error condition
                    xmpp_stanza_t *child = xmpp_stanza_get_children(error_elem);
                    while (child)
                    {
                        const char *name = xmpp_stanza_get_name(child);
                        if (name && std::string_view(name) != "text")
                        {
                            error_msg = fmt::format("Upload slot request failed: {}", name);
                            break;
                        }
                        child = xmpp_stanza_get_next(child);
                    }
                }
            }
            
            weechat_printf(account.buffer, "%s%s: %s (type: %s)",
                          weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                          error_msg.c_str(), error_type ? error_type : "unknown");
            
            account.upload_requests.erase(req_it);
        }

        // XEP-0442: if a disco#info IQ to a pubsub service returned an error,
        // flush deferred feeds via XEP-0060 plain items IQ (fallback path).
        // Without this, the deferred feeds hang forever since the success path
        // (inside the `if (query && type)` block) is only entered for type=result.
        if (id && account.pubsub_mam_disco_queries.count(id))
        {
            std::string svc_jid = account.pubsub_mam_disco_queries[id];
            account.pubsub_mam_disco_queries.erase(id);

            auto def_it = account.pubsub_mam_deferred_feeds.find(svc_jid);
            if (def_it != account.pubsub_mam_deferred_feeds.end())
            {
                const int max_items = 20;
                for (const auto &feed_key : def_it->second)
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
            auto pub_it = account.pubsub_publish_ids.find(id);
            if (pub_it != account.pubsub_publish_ids.end())
            {
                auto &ctx = pub_it->second;
                xmpp_stanza_t *error_elem = xmpp_stanza_get_child_by_name(stanza, "error");
                std::string error_cond = "unknown error";
                if (error_elem)
                {
                    // Prefer <text> child, fall back to first non-text child name
                    xmpp_stanza_t *text_elem = xmpp_stanza_get_child_by_name(error_elem, "text");
                    if (text_elem)
                    {
                        char *t = xmpp_stanza_get_text(text_elem);
                        if (t) { error_cond = t; xmpp_free(account.context, t); }
                    }
                    else
                    {
                        for (xmpp_stanza_t *c = xmpp_stanza_get_children(error_elem);
                             c; c = xmpp_stanza_get_next(c))
                        {
                            const char *cname = xmpp_stanza_get_name(c);
                            if (cname && std::string_view(cname) != "text")
                            { error_cond = cname; break; }
                        }
                    }
                }
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
            auto sub_it = account.pubsub_subscribe_queries.find(id);
            if (sub_it != account.pubsub_subscribe_queries.end())
            {
                std::string error_cond = "unknown error";
                xmpp_stanza_t *error_elem = xmpp_stanza_get_child_by_name(stanza, "error");
                if (error_elem)
                {
                    xmpp_stanza_t *text_el = xmpp_stanza_get_child_by_name(error_elem, "text");
                    if (text_el)
                    {
                        char *t = xmpp_stanza_get_text(text_el);
                        if (t) { error_cond = t; xmpp_free(account.context, t); }
                    }
                    else
                    {
                        for (xmpp_stanza_t *c = xmpp_stanza_get_children(error_elem);
                             c; c = xmpp_stanza_get_next(c))
                        {
                            const char *cname = xmpp_stanza_get_name(c);
                            if (cname && std::string_view(cname) != "text")
                            { error_cond = cname; break; }
                        }
                    }
                }
                struct t_gui_buffer *fb = sub_it->second.buffer
                                         ? sub_it->second.buffer : account.buffer;
                weechat_printf(fb,
                    "%s%s: subscribe to %s failed: %s",
                    weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                    sub_it->second.feed_key.c_str(), error_cond.c_str());
                account.pubsub_subscribe_queries.erase(sub_it);
            }
        }
        {
            auto unsub_it = account.pubsub_unsubscribe_queries.find(id);
            if (unsub_it != account.pubsub_unsubscribe_queries.end())
            {
                std::string error_cond = "unknown error";
                xmpp_stanza_t *error_elem = xmpp_stanza_get_child_by_name(stanza, "error");
                if (error_elem)
                {
                    xmpp_stanza_t *text_el = xmpp_stanza_get_child_by_name(error_elem, "text");
                    if (text_el)
                    {
                        char *t = xmpp_stanza_get_text(text_el);
                        if (t) { error_cond = t; xmpp_free(account.context, t); }
                    }
                    else
                    {
                        for (xmpp_stanza_t *c = xmpp_stanza_get_children(error_elem);
                             c; c = xmpp_stanza_get_next(c))
                        {
                            const char *cname = xmpp_stanza_get_name(c);
                            if (cname && std::string_view(cname) != "text")
                            { error_cond = cname; break; }
                        }
                    }
                }
                struct t_gui_buffer *fb = unsub_it->second.buffer
                                         ? unsub_it->second.buffer : account.buffer;
                weechat_printf(fb,
                    "%s%s: unsubscribe from %s failed: %s",
                    weechat_prefix("error"), WEECHAT_XMPP_PLUGIN_NAME,
                    unsub_it->second.feed_key.c_str(), error_cond.c_str());
                account.pubsub_unsubscribe_queries.erase(unsub_it);
            }
        }

        // XEP-0060: pubsub item-fetch error (e.g. forbidden, item-not-found)
        {
            auto fetch_it = account.pubsub_fetch_ids.find(id);
            if (fetch_it != account.pubsub_fetch_ids.end())
            {
                const std::string &err_service = fetch_it->second.service;
                const std::string &err_node    = fetch_it->second.node;

                std::string error_cond = "unknown error";
                xmpp_stanza_t *error_elem = xmpp_stanza_get_child_by_name(stanza, "error");
                if (error_elem)
                {
                    xmpp_stanza_t *text_el = xmpp_stanza_get_child_by_name(error_elem, "text");
                    if (text_el)
                    {
                        char *t = xmpp_stanza_get_text(text_el);
                        if (t) { error_cond = t; xmpp_free(account.context, t); }
                    }
                    else
                    {
                        for (xmpp_stanza_t *c = xmpp_stanza_get_children(error_elem);
                             c; c = xmpp_stanza_get_next(c))
                        {
                            const char *cname = xmpp_stanza_get_name(c);
                            if (cname && std::string_view(cname) != "text")
                            { error_cond = cname; break; }
                        }
                    }
                }

                // Report the error in the feed buffer if it already exists,
                // otherwise fall back to the account buffer.
                std::string feed_key = fmt::format("{}/{}", err_service, err_node);
                struct t_gui_buffer *err_buf = account.buffer;
                auto ch_it = account.channels.find(feed_key);
                if (ch_it != account.channels.end())
                    err_buf = ch_it->second.buffer;

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
    
    // XEP-0030: Service Discovery - disco#items response
    xmpp_stanza_t *items_query = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "query", "http://jabber.org/protocol/disco#items");
    
    if (items_query && type && weechat_strcasecmp(type, "result") == 0)
    {
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
        bool is_adhoc_query = iq_id && account.adhoc_queries.count(iq_id);

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
        if (iq_id && account.pubsub_disco_queries.count(iq_id))
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
        bool is_adhoc_query = iq_id && account.adhoc_queries.count(iq_id);
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
                const char *note_text = note ? xmpp_stanza_get_text_ptr(note) : nullptr;
                weechat_printf_date_tags(adhoc_buf, 0, "xmpp_adhoc,notify_none",
                                         "%s[adhoc] Command %s completed%s%s",
                                         weechat_prefix("network"),
                                         cmd_node ? cmd_node : "",
                                         note_text ? ": " : "",
                                         note_text ? note_text : "");
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
        bool is_cs_query = cs_id && account.channel_search_queries.count(cs_id);

        if (is_cs_query && type)
        {
            auto &cs_info = account.channel_search_queries[cs_id];
            struct t_gui_buffer *cs_buf = cs_info.buffer ? cs_info.buffer : account.buffer;

            if (weechat_strcasecmp(type, "error") == 0)
            {
                // Try to extract a human-readable error
                xmpp_stanza_t *error_el = xmpp_stanza_get_child_by_name(stanza, "error");
                const char *err_text = nullptr;
                const char *err_condition = nullptr;
                if (error_el)
                {
                    xmpp_stanza_t *text_el = xmpp_stanza_get_child_by_name(error_el, "text");
                    if (text_el)
                        err_text = xmpp_stanza_get_text_ptr(text_el);

                    // XMPP stanza errors usually encode the condition as a child in
                    // urn:ietf:params:xml:ns:xmpp-stanzas (e.g. <bad-request/>).
                    if (!err_text)
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
                                         err_text ? err_text
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

                    // Build search submit based on XEP-0433 fields — RAII using spec builder.
                    xmpp_ctx_t *ctx = account.context;

                    auto submit_iq = std::shared_ptr<xmpp_stanza_t> {
                        xmpp_iq_new(ctx, "get", submit_id.c_str()), xmpp_stanza_release };
                    xmpp_stanza_set_to(submit_iq.get(), cs_info.service_jid.c_str());

                    struct search_spec : stanza::spec {
                        search_spec() : spec("search") {
                            attr("xmlns", "urn:xmpp:channel-search:0:search");
                        }
                    } ss;
                    auto submit_search = ss.build(ctx);

                    struct xform_spec : stanza::spec {
                        xform_spec() : spec("x") {
                            attr("xmlns", "jabber:x:data");
                            attr("type", "submit");
                        }
                    } xfs;
                    auto submit_form = xfs.build(ctx);

                    auto add_field = [&](const char *var, const char *value,
                                         const char *type_attr = nullptr) {
                        xmpp_stanza_t *f = stanza_make_field(ctx, var, value, type_attr);
                        xmpp_stanza_add_child(submit_form.get(), f);
                        xmpp_stanza_release(f);
                    };

                    add_field("FORM_TYPE", "urn:xmpp:channel-search:0:search-params", "hidden");
                    if (!cs_info.keywords.empty())
                    {
                        add_field("q", cs_info.keywords.c_str(), "text-single");
                        // Required by XEP-0433 if q is supported.
                        add_field("sinname", "true", "boolean");
                        add_field("sindescription", "true", "boolean");
                        add_field("sinaddress", "true", "boolean");
                    }

                    // Prefer stable sort by address for broad compatibility.
                    add_field("key", "{urn:xmpp:channel-search:0:order}address", "list-single");
                    // Restrict to MUC channels when service supports the field.
                    add_field("types", "xep-0045", "list-multi");

                    xmpp_stanza_add_child(submit_search.get(), submit_form.get());
                    xmpp_stanza_add_child(submit_iq.get(), submit_search.get());

                    account.connection.send(submit_iq.get());

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

                        const char *name_raw    = name_el  ? xmpp_stanza_get_text_ptr(name_el)   : nullptr;
                        const char *nusers_raw  = nusers_el ? xmpp_stanza_get_text_ptr(nusers_el) : nullptr;
                        const char *desc_raw    = desc_el  ? xmpp_stanza_get_text_ptr(desc_el)   : nullptr;
                        const char *language_raw = language_el ? xmpp_stanza_get_text_ptr(language_el) : nullptr;
                        const char *service_type_raw = service_type_el ? xmpp_stanza_get_text_ptr(service_type_el) : nullptr;
                        const char *anonymity_raw = anonymity_el ? xmpp_stanza_get_text_ptr(anonymity_el) : nullptr;

                        std::string display = address;
                        if (name_raw && name_raw[0])
                            display = std::string(name_raw) + " <" + address + ">";

                        std::vector<std::string> meta_parts;
                        if (nusers_raw && nusers_raw[0])
                            meta_parts.emplace_back(std::string(nusers_raw) + " users");

                        bool is_open = false;
                        if (open_el)
                        {
                            const char *open_raw = xmpp_stanza_get_text_ptr(open_el);
                            if (!open_raw || !open_raw[0]
                                || weechat_strcasecmp(open_raw, "true") == 0
                                || std::string_view(open_raw) == "1")
                            {
                                is_open = true;
                            }
                        }
                        if (is_open)
                            meta_parts.emplace_back("open");

                        if (language_raw && language_raw[0])
                            meta_parts.emplace_back(std::string("lang=") + language_raw);

                        if (service_type_raw && service_type_raw[0])
                        {
                            std::string st = service_type_raw;
                            if (st == "xep-0045") st = "muc";
                            else if (st == "xep-0369") st = "mix";
                            meta_parts.emplace_back(std::string("type=") + st);
                        }

                        if (anonymity_raw && anonymity_raw[0])
                            meta_parts.emplace_back(std::string("anon=") + anonymity_raw);

                        std::string info_str;
                        if (!meta_parts.empty())
                        {
                            info_str = "[";
                            for (size_t i = 0; i < meta_parts.size(); ++i)
                            {
                                if (i) info_str += ", ";
                                info_str += meta_parts[i];
                            }
                            info_str += "]";
                        }

                        if (cs_info.picker)
                        {
                            // Picker path: add_entry with address as data, display as label.
                            // Sublabel carries metadata. Skip async disco#info enrichment
                            // since picker entries cannot be updated in-place.
                            using picker_t = weechat::ui::picker<std::string>;
                            std::string sublabel = info_str;
                            if (desc_raw && desc_raw[0])
                            {
                                std::string desc(desc_raw);
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
                            if (desc_raw && desc_raw[0])
                            {
                                std::string desc(desc_raw);
                                if (desc.length() > 120)
                                    desc = desc.substr(0, 117) + "...";
                                weechat_printf_date_tags(cs_buf, 0, "xmpp_channel_search,notify_none",
                                                         "    %s", desc.c_str());
                            }

                            // If the directory result is sparse, query room disco#info for
                            // additional metadata (name/description/occupants/language).
                            if ((!name_raw || !name_raw[0])
                                || (!desc_raw || !desc_raw[0])
                                || (!nusers_raw || !nusers_raw[0])
                                || (!language_raw || !language_raw[0]))
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
        if (stanza_id && account.channel_search_disco_queries.count(stanza_id))
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
                        xmpp_stanza_t *val = xmpp_stanza_get_child_by_name(field, "value");
                        const char *txt = val ? xmpp_stanza_get_text_ptr(val) : nullptr;
                        if (var && txt && txt[0])
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
                        for (size_t i = 0; i < meta.size(); ++i)
                        {
                            if (i) meta_s += ", ";
                            meta_s += meta[i];
                        }
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
            bool user_initiated = stanza_id && account.user_disco_queries.count(stanza_id);
            bool caps_query = stanza_id && account.caps_disco_queries.count(stanza_id);
            
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
                    const char *lang = xmpp_stanza_get_attribute(id_elem, "xml:lang");
                    const char *name = xmpp_stanza_get_attribute(id_elem, "name");
                    identities.push_back(
                        std::string(cat  ? cat  : "") + "/" +
                        std::string(typ  ? typ  : "") + "/" +
                        std::string(lang ? lang : "") + "/" +
                        std::string(name ? name : ""));
                }
                std::sort(identities.begin(), identities.end());
                for (const auto &ident : identities)
                    S += ident + "<";

                // Step 4-5: features already collected above; sort and append
                std::vector<std::string> sorted_features = features;
                std::sort(sorted_features.begin(), sorted_features.end());
                for (const auto &feat : sorted_features)
                    S += feat + "<";

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
                        // find FORM_TYPE value
                        for (xmpp_stanza_t *f = xmpp_stanza_get_child_by_name(x_elem, "field");
                             f; f = next_named(f, "field"))
                        {
                            const char *fvar = xmpp_stanza_get_attribute(f, "var");
                            if (fvar && std::string_view(fvar) == "FORM_TYPE")
                            {
                                xmpp_stanza_t *vnode = xmpp_stanza_get_child_by_name(f, "value");
                                const char *vtext = vnode ? xmpp_stanza_get_text_ptr(vnode) : nullptr;
                                fd.form_type = vtext ? vtext : "";
                                break;
                            }
                        }
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
                                    const char *vtext = xmpp_stanza_get_text_ptr(vnode);
                                    if (vtext) vals.push_back(vtext);
                                }
                                std::sort(vals.begin(), vals.end());
                                fd.fields.emplace_back(fvar, std::move(vals));
                            }
                        }
                        std::sort(fd.fields.begin(), fd.fields.end(),
                                  [](const auto &a, const auto &b){ return a.first < b.first; });
                        forms.push_back(std::move(fd));
                    }
                }
                // sort forms by FORM_TYPE
                std::sort(forms.begin(), forms.end(),
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
                unsigned char digest[20];
                unsigned int  digest_len = sizeof(digest);
                EVP_Digest(S.data(), S.size(), digest, &digest_len, EVP_sha1(), nullptr);

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
            bool upload_disco = stanza_id && account.upload_disco_queries.count(stanza_id);
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
                                    char *value_text = xmpp_stanza_get_text(value);
                                    if (value_text)
                                    {
                                        max_size = strtoull(value_text, nullptr, 10);
                                        xmpp_free(account.context, value_text);
                                    }
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
                && account.pubsub_mam_disco_queries.count(stanza_id);
            if (pubsub_mam_disco)
            {
                std::string svc_jid = account.pubsub_mam_disco_queries[stanza_id];
                account.pubsub_mam_disco_queries.erase(stanza_id);

                // Check whether this service advertises urn:xmpp:mam:2
                bool has_mam = false;
                for (const auto &feat : features)
                {
                    if (feat == "urn:xmpp:mam:2") { has_mam = true; break; }
                }

                if (has_mam)
                    account.pubsub_mam_services.insert(svc_jid);

                // Flush deferred feed restores for this service.
                auto def_it = account.pubsub_mam_deferred_feeds.find(svc_jid);
                if (def_it != account.pubsub_mam_deferred_feeds.end())
                {
                    const int max_items = 20;
                    for (const auto &feed_key : def_it->second)
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
                    auto ptr_channel = account.channels.find(from);
                    if (ptr_channel != account.channels.end())
                        ptr_channel->second.update_name(name.data());
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
                        if (std::find(kps.begin(), kps.end(), svc_jid) == kps.end())
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
            xmpp_stanza_t *gtxt = xmpp_stanza_get_children(g);
            if (gtxt) {
                char *text = xmpp_stanza_get_text(gtxt);
                if (text) {
                    account.roster[jid].groups.push_back(text);
                    xmpp_free(account.context, text);
                }
            }
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
                    bool is_new = (account.roster.find(jid) == account.roster.end());
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
                char *intext = nullptr;
                if (nick)
                {
                    text = xmpp_stanza_get_children(nick);
                    // BUG 3 fix: text may be nullptr if <nick/> element is empty
                    intext = text ? xmpp_stanza_get_text(text) : nullptr;
                }

                if (!jid)
                    continue;

                // Store bookmark
                account.bookmarks[jid].jid = jid;
                account.bookmarks[jid].name = name ? name : "";
                account.bookmarks[jid].nick = intext ? intext : "";
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
                        char **command = weechat_string_dyn_alloc(256);
                        weechat_string_dyn_concat(command, "/enter ", -1);
                        weechat_string_dyn_concat(command, jid, -1);
                        // BUG 3 fix: only append nick if intext is non-nullptr and non-empty
                        if (intext && intext[0])
                        {
                            weechat_string_dyn_concat(command, "/", -1);
                            weechat_string_dyn_concat(command, intext, -1);
                        }
                        weechat_command(account.buffer, *command);
                        auto ptr_channel = account.channels.find(jid);
                        struct t_gui_buffer *ptr_buffer =
                            ptr_channel != account.channels.end()
                            ? ptr_channel->second.buffer : nullptr;
                        if (ptr_buffer)
                            ptr_channel->second.update_name(name);
                        weechat_string_dyn_free(command, 1);
                    }
                }

                if (nick)
                    xmpp_free(account.context, intext);
            }
        }
    }

    // OMEMO PubSub publish error recovery: if the server rejects our bundle or
    // devicelist publish with <precondition-not-met/>, send a node configure IQ
    // to set access_model=open + persist_items=true, then retry the publish.
    if (type && weechat_strcasecmp(type, "error") == 0 && account.omemo)
    {
        xmpp_stanza_t *err_elem = xmpp_stanza_get_child_by_name(stanza, "error");
        if (err_elem)
        {
            xmpp_stanza_t *precond = xmpp_stanza_get_child_by_name_and_ns(
                err_elem, "precondition-not-met",
                "http://jabber.org/protocol/pubsub#errors");
            if (precond)
            {
                // Identify the node from the pubsub child of the failed IQ.
                xmpp_stanza_t *failed_pubsub = xmpp_stanza_get_child_by_name_and_ns(
                    stanza, "pubsub", "http://jabber.org/protocol/pubsub");
                std::string target_node;
                if (failed_pubsub)
                {
                    xmpp_stanza_t *pub = xmpp_stanza_get_child_by_name(failed_pubsub, "publish");
                    if (pub)
                    {
                        const char *n = xmpp_stanza_get_attribute(pub, "node");
                        if (n) target_node = n;
                    }
                }
                // Also match by known publish IQ id (e.g. "omemo-bundle").
                if (target_node.empty() && id)
                {
                    if (std::string_view(id) == "omemo-legacy-bundle")
                        target_node = fmt::format("eu.siacs.conversations.axolotl.bundles:{}",
                                                  account.omemo.device_id);
                    else if (std::string_view(id) == "announce-legacy1")
                        target_node = "eu.siacs.conversations.axolotl.devicelist";
                }

                if (!target_node.empty())
                {
                    weechat_printf(account.buffer,
                        "%somemo: publish to '%s' rejected (precondition-not-met) — "
                        "configuring node and retrying",
                        weechat_prefix("network"), target_node.c_str());

                    // Build node configure IQ using spec builder.
                    xmpp_ctx_t *ctx = account.context;

                    auto mk_field = [&](const char *var, const char *val,
                                        const char *type_attr = nullptr)
                        -> std::shared_ptr<xmpp_stanza_t>
                    {
                        xmpp_stanza_t *f = stanza_make_field(ctx, var, val, type_attr);
                        return { f, xmpp_stanza_release };
                    };

                    struct xform_cfg : stanza::spec {
                        xform_cfg() : spec("x") {
                            attr("xmlns", "jabber:x:data");
                            attr("type", "submit");
                        }
                    } xcfg;
                    auto x = xcfg.build(ctx);
                    auto f1 = mk_field("FORM_TYPE",
                        "http://jabber.org/protocol/pubsub#meta-data", "hidden");
                    auto f2 = mk_field("pubsub#access_model", "open");
                    auto f3 = mk_field("pubsub#persist_items", "true");
                    auto f4 = mk_field("pubsub#max_items", "max");
                    xmpp_stanza_add_child(x.get(), f1.get());
                    xmpp_stanza_add_child(x.get(), f2.get());
                    xmpp_stanza_add_child(x.get(), f3.get());
                    xmpp_stanza_add_child(x.get(), f4.get());

                    struct configure_spec : stanza::spec {
                        configure_spec(const std::string &node_) : spec("configure") {
                            attr("node", node_);
                        }
                    } cfgnode(target_node);
                    auto configure = cfgnode.build(ctx);
                    xmpp_stanza_add_child(configure.get(), x.get());

                    struct pubsub_owner_spec : stanza::spec {
                        pubsub_owner_spec() : spec("pubsub") {
                            attr("xmlns", "http://jabber.org/protocol/pubsub#owner");
                        }
                    } pso;
                    auto cfg_pubsub = pso.build(ctx);
                    xmpp_stanza_add_child(cfg_pubsub.get(), configure.get());

                    const std::string cfg_uuid = stanza::uuid(account.context);

                    auto cfg_iq = std::shared_ptr<xmpp_stanza_t> {
                        xmpp_iq_new(ctx, "set", cfg_uuid.c_str()), xmpp_stanza_release };
                    xmpp_stanza_set_to(cfg_iq.get(), account.jid().data());
                    xmpp_stanza_add_child(cfg_iq.get(), cfg_pubsub.get());

                    // Remember which node to re-publish after the configure succeeds.
                    if (!cfg_uuid.empty())
                        account.omemo.pending_configure_retry[cfg_uuid] = target_node;

                    account.connection.send(cfg_iq.get());
                }
            }
        }
    }

    // OMEMO devicelist fetch error handling:
    // - mark missing nodes to avoid request/error loops
    if (type && weechat_strcasecmp(type, "error") == 0 && id && account.omemo)
    {
        xmpp_stanza_t *dl_err_elem = xmpp_stanza_get_child_by_name(stanza, "error");
        xmpp_stanza_t *dl_pubsub = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "pubsub", "http://jabber.org/protocol/pubsub");
        bool is_item_not_found = dl_err_elem && xmpp_stanza_get_child_by_name_and_ns(
            dl_err_elem, "item-not-found", "urn:ietf:params:xml:ns:xmpp-stanzas");
        bool is_legacy_devicelist_err = false;
        if (dl_pubsub)
        {
            xmpp_stanza_t *dl_items = xmpp_stanza_get_child_by_name(dl_pubsub, "items");
            if (dl_items)
            {
                const char *dl_node = xmpp_stanza_get_attribute(dl_items, "node");
                if (dl_node && std::string_view(dl_node) == "eu.siacs.conversations.axolotl.devicelist")
                    is_legacy_devicelist_err = true;
            }
        }

        if (is_item_not_found && is_legacy_devicelist_err)
        {
            // Resolve which JID this looked up — use pending_iq_jid first,
            // fall back to bare `from` of the error response.
            std::string dl_target_jid;
            auto dl_jid_it = account.omemo.pending_iq_jid.find(id);
            if (dl_jid_it != account.omemo.pending_iq_jid.end())
            {
                dl_target_jid = dl_jid_it->second;
                account.omemo.pending_iq_jid.erase(dl_jid_it);
            }
            else if (from)
            {
                const std::string dl_from_bare = ::jid(nullptr, from).bare;
                if (!dl_from_bare.empty())
                    dl_target_jid = dl_from_bare;
            }

            if (!dl_target_jid.empty())
            {
                bool first_legacy_miss = account.omemo.missing_axolotl_devicelist.insert(dl_target_jid).second;

                auto dl_ch_it = account.channels.find(dl_target_jid);
                if (dl_ch_it != account.channels.end())
                {
                    auto &dl_ch = dl_ch_it->second;
                    if (!dl_ch.pending_omemo_messages.empty() && first_legacy_miss)
                    {
                        weechat_printf(dl_ch.buffer,
                            "%sOMEMO: %s has no legacy OMEMO device list either. "
                            "Keeping %zu queued message(s).",
                            weechat_prefix("error"),
                            dl_target_jid.c_str(),
                            dl_ch.pending_omemo_messages.size());
                    }
                }
            }
        }
        else if (!is_item_not_found && is_legacy_devicelist_err)
        {
            // A transient or cross-domain error — clear guard entries so next
            // encode attempt can retry.
            std::string dl_target_jid;
            auto dl_jid_it = account.omemo.pending_iq_jid.find(id);
            if (dl_jid_it != account.omemo.pending_iq_jid.end())
            {
                dl_target_jid = dl_jid_it->second;
                account.omemo.pending_iq_jid.erase(dl_jid_it);
            }
            else if (from)
            {
                const std::string dl_from_bare = ::jid(nullptr, from).bare;
                if (!dl_from_bare.empty())
                    dl_target_jid = dl_from_bare;
            }

            if (!dl_target_jid.empty())
            {
                account.omemo.missing_axolotl_devicelist.erase(dl_target_jid);

                // Log the transient error so the user has visibility.
                const char *err_text = dl_err_elem
                    ? xmpp_stanza_get_text(dl_err_elem) : nullptr;
                weechat_printf(account.buffer,
                    "%somemo: transient devicelist fetch error for %s (%s) — "
                    "guard cleared, will retry on next send",
                    weechat_prefix("network"),
                    dl_target_jid.c_str(),
                    err_text ? err_text : "unknown error");
            }
        }
    }

    // After a successful node configure, re-publish the OMEMO bundle or
    // devicelist that originally failed with precondition-not-met.
    if (type && weechat_strcasecmp(type, "result") == 0 && account.omemo && id)
    {
        auto cfg_it = account.omemo.pending_configure_retry.find(id);
        if (cfg_it != account.omemo.pending_configure_retry.end())
        {
            const std::string retry_node = cfg_it->second;
            account.omemo.pending_configure_retry.erase(cfg_it);

            weechat_printf(account.buffer,
                "%somemo: node '%s' configured — re-publishing",
                weechat_prefix("network"), retry_node.c_str());

            if (retry_node == "eu.siacs.conversations.axolotl.devicelist")
            {
                auto dl_stanza = account.get_devicelist();
                if (dl_stanza)
                    account.connection.send(dl_stanza.get());
            }
            else if (std::string_view(retry_node).starts_with(
                         "eu.siacs.conversations.axolotl.bundles:"))
            {
                std::string jid_str(account.jid());
                xmpp_stanza_t *bundle_stanza =
                    account.omemo.get_axolotl_bundle(account.context, jid_str.data(), nullptr);
                if (bundle_stanza)
                {
                    account.connection.send(bundle_stanza);
                    xmpp_stanza_release(bundle_stanza);
                }
            }
        }
    }

    pubsub = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "pubsub", "http://jabber.org/protocol/pubsub");
    if (pubsub)
    {
        const char *items_node;

        // Resolve the node owner JID from pending_iq_jid map, then bare `from`,
        // then bare `to` / own JID. Used by both OMEMO:2 and legacy devicelist handlers.
        auto resolve_node_owner = [&]() -> std::string {
            std::string owner;
            if (id) {
                auto it = account.omemo.pending_iq_jid.find(id);
                if (it != account.omemo.pending_iq_jid.end()) {
                    owner = it->second;
                    account.omemo.pending_iq_jid.erase(it);
                }
            }
            if (owner.empty()) {
                if (from) {
                    const std::string bare = ::jid(nullptr, from).bare;
                    if (!bare.empty())
                        owner = bare;
                }
            }
            if (owner.empty()) {
                if (to) {
                    const std::string bare = ::jid(nullptr, to).bare;
                    owner = bare.empty() ? account.jid() : bare;
                } else {
                    owner = account.jid();
                }
            }
            return owner;
        };

        items = xmpp_stanza_get_child_by_name(pubsub, "items");
        if (items)
        {
            items_node = xmpp_stanza_get_attribute(items, "node");
            if (items_node
                     && weechat_strcasecmp(items_node,
                                           "eu.siacs.conversations.axolotl.devicelist") == 0)
            {
                // Recover the correct JID using the same logic as OMEMO:2.
                std::string node_owner_str = resolve_node_owner();

                const std::string account_bare_s = ::jid(nullptr, account.jid()).bare;
                const std::string account_bare = account_bare_s.empty()
                    ? account.jid() : account_bare_s;
                const bool is_own_devicelist = (account_bare == node_owner_str);

                if (account.omemo)
                    account.omemo.handle_axolotl_devicelist(&account,
                                                           node_owner_str.c_str(),
                                                           items);

                if (is_own_devicelist)
                {
                    bool found_self = false;
                    xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(items, "item");
                    xmpp_stanza_t *list = item
                        ? xmpp_stanza_get_child_by_name_and_ns(
                            item, "list", "eu.siacs.conversations.axolotl")
                        : nullptr;

                    for (xmpp_stanza_t *device = list ? xmpp_stanza_get_children(list) : nullptr;
                         device; device = xmpp_stanza_get_next(device))
                    {
                        const char *name = xmpp_stanza_get_name(device);
                        if (!name || weechat_strcasecmp(name, "device") != 0)
                            continue;

                        const char *did = xmpp_stanza_get_attribute(device, "id");
                        const auto parsed_did = parse_omemo_device_id(did);
                        if (parsed_did && *parsed_did == account.omemo.device_id)
                        {
                            found_self = true;
                            break;
                        }
                    }

                    if (!found_self)
                    {
                        weechat_printf(account.buffer,
                            "%somemo: our device %u missing from server legacy devicelist — re-publishing",
                            weechat_prefix("network"), account.omemo.device_id);

                        auto dl_stanza = account.get_devicelist();
                        if (dl_stanza)
                            account.connection.send(dl_stanza.get());
                    }
                }
                else if (account.omemo)
                {
                    xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(items, "item");
                    xmpp_stanza_t *list = item
                        ? xmpp_stanza_get_child_by_name_and_ns(
                            item, "list", "eu.siacs.conversations.axolotl")
                        : nullptr;

                    int legacy_device_count = 0;
                    for (xmpp_stanza_t *device = list ? xmpp_stanza_get_children(list) : nullptr;
                         device;
                         device = xmpp_stanza_get_next(device))
                    {
                        const char *name = xmpp_stanza_get_name(device);
                        if (!name || weechat_strcasecmp(name, "device") != 0)
                            continue;

                        const char *did = xmpp_stanza_get_attribute(device, "id");
                        const auto parsed_did = parse_omemo_device_id(did);
                        if (!parsed_did)
                            continue;

                        if (*parsed_did == account.omemo.device_id)
                            continue;

                        const auto bundle_key =
                            std::make_pair(std::string(node_owner_str), *parsed_did);
                        if (account.omemo.pending_bundle_fetch.count(bundle_key) != 0)
                            continue;

                        const auto bundle_node = fmt::format(
                            "eu.siacs.conversations.axolotl.bundles:{}",
                            *parsed_did);
                        std::string uuid = stanza::uuid(account.context);
                        stanza::xep0060::items its(bundle_node);
                        stanza::xep0060::pubsub ps;
                        ps.items(its);
                        stanza::iq iq_s;
                        iq_s.id(uuid).from(account.jid()).to(node_owner_str).type("get");
                        iq_s.pubsub(ps);
                        account.omemo.pending_iq_jid[uuid] = node_owner_str;
                        account.omemo.pending_bundle_fetch.insert(bundle_key);

                        ++legacy_device_count;
                        account.connection.send(iq_s.build(account.context).get());
                    }

                    weechat_printf(account.buffer,
                                   "%somemo: requested %d legacy bundle(s) for %s",
                                   weechat_prefix("network"),
                                   legacy_device_count,
                                   node_owner_str.c_str());
                }
            }
            else if (items_node
                     && std::string_view(items_node).starts_with(
                         "eu.siacs.conversations.axolotl.bundles:"))
            {
                std::string bundle_jid;
                if (account.omemo && id)
                {
                    auto it = account.omemo.pending_iq_jid.find(id);
                    if (it != account.omemo.pending_iq_jid.end())
                    {
                        bundle_jid = it->second;
                        account.omemo.pending_iq_jid.erase(it);
                    }
                }
                if (bundle_jid.empty())
                    bundle_jid = from ? from : account.jid().data();

                const std::string_view node(items_node);
                const auto pos = node.find_last_of(':');
                std::uint32_t bundle_device_id = 0;
                if (pos != std::string_view::npos && pos + 1 < node.size())
                {
                    const auto parsed_node_device_id =
                        parse_omemo_device_id(std::string(node.substr(pos + 1)).c_str());
                    if (parsed_node_device_id)
                        bundle_device_id = *parsed_node_device_id;
                }

                if (bundle_device_id == 0)
                {
                    item = xmpp_stanza_get_child_by_name(items, "item");
                    if (item)
                    {
                        const char *item_id = xmpp_stanza_get_id(item);
                        const auto parsed_item_device_id = parse_omemo_device_id(item_id);
                        if (parsed_item_device_id)
                            bundle_device_id = *parsed_item_device_id;
                    }
                }

                if (account.omemo && bundle_device_id != 0)
                    account.omemo.pending_bundle_fetch.erase({bundle_jid, bundle_device_id});

                if (type && weechat_strcasecmp(type, "result") == 0)
                {
                    if (account.omemo && bundle_device_id != 0)
                        account.omemo.handle_axolotl_bundle(&account,
                                                           account.buffer,
                                                           bundle_jid.c_str(),
                                                           bundle_device_id,
                                                           items);
                    else
                        weechat_printf(account.buffer,
                                       "%somemo: legacy bundle result for %s has missing/invalid device id",
                                       weechat_prefix("error"),
                                       bundle_jid.c_str());
                }
                else if (type && weechat_strcasecmp(type, "error") == 0)
                {
                    weechat_printf(account.buffer,
                                   "%somemo: legacy bundle fetch for %s/%u returned error",
                                   weechat_prefix("error"),
                                   bundle_jid.c_str(), bundle_device_id);
                }
            }
        }
    }

    // XEP-0442: PubSub MAM <fin> — marks end of a pubsub node MAM query.
    // The <fin> arrives as a child of the IQ result with id= matching our query IQ.
    {
        xmpp_stanza_t *pmam_fin = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "fin", "urn:xmpp:mam:2");
        if (pmam_fin && id && type && weechat_strcasecmp(type, "result") == 0)
        {
            auto pq_it = account.pubsub_mam_queries.find(id);
            if (pq_it != account.pubsub_mam_queries.end())
            {
                std::string svc_jid   = pq_it->second.service;
                std::string node_name = pq_it->second.node;
                account.pubsub_mam_queries.erase(pq_it);

                std::string feed_key = fmt::format("{}/{}", svc_jid, node_name);

                // Persist RSM <last> cursor so next reconnect can resume.
                xmpp_stanza_t *prsm = xmpp_stanza_get_child_by_name_and_ns(
                    pmam_fin, "set", "http://jabber.org/protocol/rsm");
                if (prsm)
                {
                    xmpp_stanza_t *plast = xmpp_stanza_get_child_by_name(prsm, "last");
                    if (plast)
                    {
                        xmpp_string_guard plast_text_g(account.context,
                            xmpp_stanza_get_text(plast));
                        if (plast_text_g.ptr)
                            account.mam_cursor_set(
                                fmt::format("pubsub:{}", feed_key),
                                plast_text_g.ptr);
                    }
                }
                return true;
            }
        }
    }

    fin = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "fin", "urn:xmpp:mam:2");
    if (fin)
    {
        xmpp_stanza_t *set, *set__last;
        xmpp_string_guard set__last__text_g { account.context, nullptr };
        char *&set__last__text = set__last__text_g.ptr;
        weechat::account::mam_query mam_query;

        const char *fin_complete = xmpp_stanza_get_attribute(fin, "complete");
        const char *fin_stable = xmpp_stanza_get_attribute(fin, "stable");
        const char *fin_abort = xmpp_stanza_get_attribute(fin, "abort");
        const bool fin_is_complete = fin_complete
            && (weechat_strcasecmp(fin_complete, "true") == 0
                || weechat_strcasecmp(fin_complete, "1") == 0);
        const bool fin_has_abort = fin_abort
            && (weechat_strcasecmp(fin_abort, "true") == 0
                || weechat_strcasecmp(fin_abort, "1") == 0);

        set = xmpp_stanza_get_child_by_name_and_ns(
            fin, "set", "http://jabber.org/protocol/rsm");
        if (id && account.mam_query_search(&mam_query, id))
        {
            // Check if this is a global MAM query (empty 'with')
            bool is_global_query = mam_query.with.empty();

            if (fin_has_abort)
            {
                weechat_printf(account.buffer,
                               "%sMAM query %s aborted by server (complete=%s stable=%s)",
                               weechat_prefix("error"),
                               mam_query.id.c_str(),
                               fin_complete ? fin_complete : "(unset)",
                               fin_stable ? fin_stable : "(unset)");
                account.mam_query_remove(mam_query.id);
                if (is_global_query)
                {
                    account.omemo.global_mam_catchup = false;
                    account.omemo.process_postponed_key_transports(account);
                    account.omemo.process_postponed_bundle_republish(account);
                }
                return true;
            }
            
            auto channel = account.channels.find(mam_query.with.data());

            set__last = set ? xmpp_stanza_get_child_by_name(set, "last") : nullptr;
            set__last__text = set__last
                ? xmpp_stanza_get_text(set__last) : nullptr;

            if (channel != account.channels.end())
            {
                // Only page when the result set is not complete.
                // Some servers include <last/> even on the final page.
                if (set__last__text && !fin_is_complete)
                {
                    time_t *start_ptr = mam_query.start ? &*mam_query.start : nullptr;
                    time_t *end_ptr   = mam_query.end   ? &*mam_query.end   : nullptr;

                    channel->second.fetch_mam(id,
                                       start_ptr,
                                       end_ptr,
                                       set__last__text);
                }
                else
                {
                    // MAM fetch complete, update last fetch timestamp
                    channel->second.last_mam_fetch = time(nullptr);
                    account.mam_cache_set_last_timestamp(channel->second.id, channel->second.last_mam_fetch);
                    // Persist this PM JID so it can be restored on the next full restart
                    if (channel->second.type == weechat::channel::chat_type::PM)
                        account.pm_open_register(channel->second.id);
                    account.mam_query_remove(mam_query.id);
                }
            }
            else if (is_global_query)
            {
                // Only page when the result set is not complete.
                if (set__last__text && !fin_is_complete)
                {
                    // Persist the RSM cursor so the next reconnect resumes from here
                    account.mam_cursor_set("global", set__last__text);

                    // More pages — issue next page of global MAM query with <after> token
                    account.mam_query_remove(mam_query.id);

                    std::string next_id = stanza::uuid(account.context);
                    account.add_mam_query(next_id.c_str(), "",
                                          mam_query.start, mam_query.end);

                    stanza::xep0313::query next_q;
                    if (mam_query.start)
                    {
                        time_t tval = *mam_query.start;
                        stanza::xep0313::x_filter xf;
                        xf.start(fmt::format("{:%Y-%m-%dT%H:%M:%SZ}", fmt::gmtime(tval)));
                        next_q.filter(xf);
                    }
                    stanza::xep0059::set rsm_after;
                    rsm_after.after(set__last__text);
                    next_q.rsm(rsm_after);

                    this->send(stanza::iq()
                        .type("set")
                        .id(next_id)
                        .xep0313()
                        .query(next_q)
                        .build(account.context)
                        .get());
                }
                else
                {
                    // Global MAM query complete — persist the final RSM cursor so
                    // the next reconnect resumes from the very end of the archive
                    // rather than replaying from a stale intermediate cursor.
                    if (set__last__text)
                        account.mam_cursor_set("global", set__last__text);

                    account.mam_query_remove(mam_query.id);
                        // MAM catchup done — fire deferred key transports now
                        account.omemo.global_mam_catchup = false;
                        account.omemo.process_postponed_key_transports(account);
                        account.omemo.process_postponed_bundle_republish(account);
                }
            }
            else
            {
                if (!set__last__text)
                    account.mam_query_remove(mam_query.id);
            }
        }
    }

    return true;
}
