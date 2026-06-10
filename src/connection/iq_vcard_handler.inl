bool weechat::connection::handle_vcard_iq_event(xmpp_stanza_t *stanza, std::string_view own_jid_str)
{
    const char *id = xmpp_stanza_get_id(stanza);
    const char *from = xmpp_stanza_get_from(stanza);
    const char *type = xmpp_stanza_get_attribute(stanza, "type");

    xmpp_stanza_t *vcard = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "vCard", "vcard-temp");
    if (!vcard || !type || weechat_strcasecmp(type, "result") != 0)
        return false;

    const char *from_jid = from ? from : own_jid_str.data();

    if (id)
    {
        if (auto sv_it = account.setvcard_queries.find(id); sv_it != account.setvcard_queries.end())
        {
            auto& [_, sv] = *sv_it;
            struct t_gui_buffer *sv_buf = sv.buffer;

            const auto parsed = ::xmpp::vcard_fields_from_stanza(::xmpp::StanzaView(vcard));
            auto f = parsed;
            (void)::xmpp::apply_vcard_set_field_override(f, sv.field, sv.value);

            ::xmpp::xep0054::vcard_fields out;
            out.fn       = f.fn;
            out.nickname = f.nickname;
            out.email    = f.email;
            out.url      = f.url;
            out.desc     = f.desc;
            out.org      = f.org;
            out.title    = f.title;
            out.tel      = f.tel;
            out.bday     = f.bday;
            out.note     = f.note;

            xmpp_stanza_t *set_iq = ::xmpp::xep0054::vcard_set(account.context, out);
            account.connection.send(set_iq);
            xmpp_stanza_release(set_iq);
            weechat::UiPort::for_buffer(sv_buf)->printf_network(fmt::format(
                "vCard field {} updated", sv.field));

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

    auto target_ui = weechat::UiPort::for_buffer(target_buf);

    // Helper: print a labelled line only if value is non-empty
    auto print_field = [&](const char *label, const std::string &val) {
        if (!val.empty())
            target_ui->printf(fmt::format(
                "  {}{}{} {}",
                weechat_color("bold"), label,
                weechat_color("reset"), val));
    };

    if (is_whois)
    {
        target_ui->printf_network(fmt::format("vCard for {}:", from_jid));
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

    std::string adr;
    if (xmpp_stanza_t *adr_el = xmpp_stanza_get_child_by_name(vcard, "ADR"))
        adr = ::xmpp::format_vcard_temp_adr(::xmpp::StanzaView(adr_el));

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

bool weechat::connection::handle_vcard4_pubsub_iq_event(xmpp_stanza_t *stanza, std::string_view own_jid_str)
{
    const char *id = xmpp_stanza_get_id(stanza);
    const char *from = xmpp_stanza_get_from(stanza);
    const char *type = xmpp_stanza_get_attribute(stanza, "type");

    xmpp_stanza_t *pubsub_vc4 = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "pubsub", "http://jabber.org/protocol/pubsub");
    if (!pubsub_vc4 || !type || weechat_strcasecmp(type, "result") != 0)
        return false;

    xmpp_stanza_t *items = xmpp_stanza_get_child_by_name(pubsub_vc4, "items");
    if (!items)
        return false;

    const char *node = xmpp_stanza_get_attribute(items, "node");
    if (!node || !::xmpp::is_vcard4_pubsub_node(node))
        return false;

    const char *from_jid = from ? from : own_jid_str.data();
    if (id)
        account.whois_queries.erase(id);

    xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(items, "item");
    if (!item)
        return false;

    xmpp_stanza_t *vcard4 = xmpp_stanza_get_child_by_name_and_ns(
        item, "vcard", NS_VCARD4);
    if (!vcard4)
        return false;

    XDEBUG("vCard4 auto-fetched for {}", from_jid);
    return true;
}

bool weechat::connection::handle_bookmarks_iq_event(xmpp_stanza_t *stanza)
{
    const char *type = xmpp_stanza_get_attribute(stanza, "type");
    const char *to = xmpp_stanza_get_to(stanza);

    xmpp_stanza_t *query = xmpp_stanza_get_child_by_name_and_ns(
        stanza, "query", "jabber:iq:private");
    if (!query || !type || weechat_strcasecmp(type, "result") != 0)
        return false;

    xmpp_stanza_t *storage = xmpp_stanza_get_child_by_name_and_ns(
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

        for (xmpp_stanza_t *conference = xmpp_stanza_get_children(storage);
             conference; conference = xmpp_stanza_get_next(conference))
        {
            const char *name = xmpp_stanza_get_name(conference);
            if (weechat_strcasecmp(name, "conference") != 0)
                continue;

            const char *jid = xmpp_stanza_get_attribute(conference, "jid");
            const char *autojoin = xmpp_stanza_get_attribute(conference, "autojoin");
            name = xmpp_stanza_get_attribute(conference, "name");
            xmpp_stanza_t *nick_el = xmpp_stanza_get_child_by_name(conference, "nick");
            const std::string bookmark_nick = nick_el ? stanza_element_text(nick_el) : std::string {};

            if (!jid)
                continue;

            // Store bookmark
            account.bookmarks[jid].jid = jid;
            account.bookmarks[jid].name = name ? name : "";
            account.bookmarks[jid].nick = bookmark_nick;
            account.bookmarks[jid].autojoin = autojoin
                && ::xmpp::is_bookmark_autojoin_true(autojoin);

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
                if (::xmpp::is_biboumi_gateway_room(jid))
                {
                    weechat::UiPort::for_buffer(account.buffer)->printf_network(fmt::format(
                        "Skipping autojoin for IRC gateway room: {}", jid));
                }
                else
                {
                    const std::string cmd =
                        ::xmpp::bookmark_enter_command(jid, bookmark_nick);
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
    return false;
}
