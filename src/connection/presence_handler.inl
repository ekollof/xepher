bool weechat::connection::presence_handler(xmpp_stanza_t *stanza, bool top_level)
{
    // SM counter incremented in libstrophe wrapper, not here
    // top_level parameter kept for nested/recursive calls

    (void) top_level;
    append_raw_xml_trace(account, "RECV", stanza);

    weechat::user *user;
    weechat::channel *channel;

    auto binding = xml::presence(account.context, stanza);
    if (!binding.from)
        return 1;

    std::string clientid;
    if (auto caps = binding.capabilities())
    {
        auto node = caps->node;
        auto ver = caps->verification;

        clientid = fmt::format("{}#{}", node, ver);
        
        // Check if we have this capability hash cached (XEP-0115)
        std::vector<std::string> cached_features;
        if (account.caps_cache_get(ver, cached_features))
        {
            // We have cached features, no need to query
            // Could use cached_features here if needed
        }
        else
        {
            // Not cached, send disco query and mark for caching
            xmpp_string_guard disco_id_g(account.context, xmpp_uuid_gen(account.context));
            const char *disco_id = disco_id_g.ptr;
            account.caps_disco_queries[disco_id] = ver;  // Track this query for caching
            
            std::string to_jid = binding.from ? binding.from->full : std::string();

            account.connection.send(stanza::iq()
                        .from(binding.to ? binding.to->full : "")
                        .to(to_jid)
                        .type("get")
                        .id(disco_id)
                        .xep0030()
                        .query()
                        .build(account.context)
                        .get());
            
            // freed by disco_id_g
        }
    }

    channel = account.channels.contains(binding.from->bare.data())
        ? &account.channels.find(binding.from->bare.data())->second : nullptr;
    
    // Note: We don't auto-create PM channels from presence anymore.
    // PM channels are only created when:
    // - User explicitly opens with /query
    // - We receive a message from them
    // This allows roster contacts to appear in account nicklist instead of creating buffers

    if (binding.type && *binding.type == "error" && channel)
    {
        if (auto error = binding.error())
        {
            const char *error_reason = error->reason();
            const char *from_str = binding.from ? binding.from->full.data() : "unknown";
            
            // Only log if the error has meaningful information
            // Skip generic "Unspecified" errors that don't have useful context
            if (strcmp(error_reason, "Unspecified") != 0 || error->description)
            {
                weechat_printf(channel->buffer, "[!]\t%s%sError: %s%s%s%s",
                               weechat_color("gray"),
                               binding.muc() ? "MUC " : "",
                               error_reason,
                               error->description ? " (" : "",
                               error->description ? error->description->data() : "",
                               error->description ? ")" : "");
            }
            else
            {
                // Debug: log unspecified errors with JID
                weechat_printf(account.buffer, "%s[DEBUG] Received unspecified error from %s (presence)",
                              weechat_prefix("network"), from_str);
            }
        }
        return 1;
    }

    if (auto x = binding.muc_user())
    {
        bool is_nick_change = false;
        bool is_new_room = false;
        bool is_server_nick = false;
        for (int& status : x->statuses)
        {
            switch (status)
            {
                case 100: // Non-Anonymous: [message | Entering a room]: Inform user that any occupant is allowed to see the user's full JID
                    if (channel)
                        weechat_buffer_set(channel->buffer, "notify", "2");
                    break;
                case 101: // Affiliation change: room visibility changed, JID now visible to all
                    if (channel)
                        weechat_printf_date_tags(channel->buffer, 0, "xmpp_presence,notify_none",
                                                 "%s%s[Room] Your affiliation changed; your full JID is now visible to all occupants",
                                                 weechat_prefix("network"), weechat_color("gray"));
                    break;
                case 102: // : [message | Configuration change]: Inform occupants that room now shows unavailable members
                    break;
                case 103: // : [message | Configuration change]: Inform occupants that room now does not show unavailable members
                    break;
                case 104: // : [message | Configuration change]: Inform occupants that a non-privacy-related room configuration change has occurred
                    break;
                case 110: // Self-Presence: [presence | Any room presence]: Inform user that presence refers to one of its own room occupants
                    // Status 110 is sent last in the initial presence flood — clear joining flag
                    if (channel)
                        channel->joining = false;
                    break;
                case 170: // Logging Active: room logging enabled — privacy notice
                    if (channel)
                        weechat_printf_date_tags(channel->buffer, 0, "xmpp_presence,notify_none",
                                                 "%s%s[Room] Warning: room logging is now %senabled%s — messages are being stored",
                                                 weechat_prefix("network"), weechat_color("yellow"),
                                                 weechat_color("yellow,bold"), weechat_color("reset"));
                    break;
                case 171: // Logging disabled
                    if (channel)
                        weechat_printf_date_tags(channel->buffer, 0, "xmpp_presence,notify_none",
                                                 "%s%s[Room] Room logging is now disabled",
                                                 weechat_prefix("network"), weechat_color("gray"));
                    break;
                case 172: // Room is now non-anonymous (full JIDs visible)
                    if (channel)
                        weechat_printf_date_tags(channel->buffer, 0, "xmpp_presence,notify_none",
                                                 "%s%s[Room] Room is now %snon-anonymous%s — full JIDs are visible to all occupants",
                                                 weechat_prefix("network"), weechat_color("yellow"),
                                                 weechat_color("yellow,bold"), weechat_color("reset"));
                    break;
                case 173: // Room is now semi-anonymous
                    if (channel)
                        weechat_printf_date_tags(channel->buffer, 0, "xmpp_presence,notify_none",
                                                 "%s%s[Room] Room is now semi-anonymous — full JIDs visible to moderators only",
                                                 weechat_prefix("network"), weechat_color("gray"));
                    break;
                case 174: // Room is now fully-anonymous
                    if (channel)
                        weechat_printf_date_tags(channel->buffer, 0, "xmpp_presence,notify_none",
                                                 "%s%s[Room] Room is now fully-anonymous — JIDs are hidden from all occupants",
                                                 weechat_prefix("network"), weechat_color("gray"));
                    break;
                case 201: // New room created — must accept default config to unlock it
                    is_new_room = true;
                    if (channel)
                        weechat_printf_date_tags(channel->buffer, 0, "xmpp_presence,notify_none",
                                                 "%s%s[Room] New room created; accepting default configuration",
                                                 weechat_prefix("network"), weechat_color("gray"));
                    break;
                case 210: // Server assigned/modified nick
                    is_server_nick = true;
                    break;
                case 301: // : [presence | Removal from room]: Inform user that he or she has been banned from the room
                    weechat_printf(channel->buffer, "[!]\t%sBanned from Room", weechat_color("gray"));
                    break;
                case 303: // Nick change — this unavailable presence means occupant is changing nick
                    is_nick_change = true;
                    break;
                case 307: // : [presence | Removal from room]: Inform user that he or she has been kicked from the room
                    weechat_printf(channel->buffer, "[!]\t%sKicked from room", weechat_color("gray"));
                    break;
                case 321: // : [presence | Removal from room]: Inform user that he or she is being removed from the room because of an affiliation change
                    weechat_printf(channel->buffer, "[!]\t%sRoom Affiliation changed, kicked", weechat_color("gray"));
                    break;
                case 322: // : [presence | Removal from room]: Inform user that he or she is being removed from the room because the room has been changed to members-only and the user is not a member
                    weechat_printf(channel->buffer, "[!]\t%sRoom now members-only, kicked", weechat_color("gray"));
                    break;
                case 332: // : [presence | Removal from room]: Inform user that he or she is being removed from the room because of a system shutdown
                    weechat_printf(channel->buffer, "[!]\t%sRoom Shutdown", weechat_color("gray"));
                    break;
                default:
                    break;
            }
        }

        for (auto& item : x->items)
        {
            using xml::xep0045;

            std::string role(item.role ? xep0045::format_role(*item.role) : "");
            std::string affiliation(item.affiliation ? xep0045::format_affiliation(*item.affiliation) : "");
            std::string jid = item.target ? item.target->full : clientid;

            user = weechat::user::search(&account, binding.from->full.data());
            if (!user)
            {
                auto name = binding.from->full.data();
                user = &account.users.emplace(std::piecewise_construct,
                                              std::forward_as_tuple(name),
                                              std::forward_as_tuple(&account, channel, name,
                                                                    channel && binding.from->bare.data() == channel->id
                                                                    ? (binding.from->resource.size() ? binding.from->resource.data() : "")
                                                                    : binding.from->full.data())).first->second;
            }
            auto status = binding.status();
            auto show = binding.show();
            auto idle = binding.idle_since();
            user->profile.status_text = status ? std::optional<std::string>(status->data()) : std::nullopt;
            user->profile.status = show ? std::optional<std::string>(show->data()) : std::nullopt;
            user->profile.idle = idle ? fmt::format("{}", *idle) : std::string();
            user->is_away = show ? *show == "away" : false;
            user->profile.role = role.size() ? std::optional<std::string>(role.data()) : std::nullopt;
            user->profile.affiliation = (affiliation.size() && affiliation != "none")
                ? std::optional<std::string>(affiliation.data()) : std::nullopt;
            if (channel)
            {
                if (auto signature = binding.signature())
                {
                    char *pgp_raw = account.pgp.verify(channel->buffer, signature->data());
                    user->profile.pgp_id = pgp_raw ? std::optional<std::string>(pgp_raw) : std::nullopt;
                    free(pgp_raw);
                    if (user->profile.pgp_id.has_value() && channel->type != weechat::channel::chat_type::MUC)
                        channel->pgp.ids.emplace(user->profile.pgp_id.value());
                }

                if (weechat_strcasecmp(role.data(), "none") == 0)
                {
                    if (is_nick_change && item.nick && channel)
                    {
                        // Status 303: nick change — print rename notice instead of "left"
                        const char *old_nick = binding.from->resource.size()
                            ? binding.from->resource.data() : binding.from->full.data();
                        // Smart filter: suppress nick-change for users who haven't spoken recently
                        const char *nick_tags = channel->smart_filter_nick(old_nick)
                            ? "xmpp_presence,nick,log4,xmpp_smart_filter"
                            : "xmpp_presence,nick,log4";
                        weechat_printf_date_tags(channel->buffer, 0, nick_tags,
                                                 "%s%s%s%s is now known as %s%s",
                                                 weechat_prefix("network"),
                                                 weechat_color("irc.color.nick_change"),
                                                 old_nick,
                                                 weechat_color("reset"),
                                                 weechat_color("irc.color.nick_change"),
                                                 item.nick->data());
                        // Still remove from nicklist so the new presence can re-add
                        weechat::user *leaving = weechat::user::search(&account, binding.from->full.data());
                        if (leaving)
                            leaving->nicklist_remove(&account, channel);
                    }
                    else
                        channel->remove_member(binding.from->full.data(), status ? status->data() : nullptr);
                }
                else
                {
                    channel->add_member(binding.from->full.data(), jid.data());
                    if (is_server_nick && channel)
                    {
                        // Status 210: server assigned a different nick than requested
                        const char *assigned = binding.from->resource.size()
                            ? binding.from->resource.data() : binding.from->full.data();
                        weechat_printf_date_tags(channel->buffer, 0, "xmpp_presence,notify_none",
                                                 "%s%s[Room] Server assigned you the nick: %s%s%s",
                                                 weechat_prefix("network"), weechat_color("gray"),
                                                 weechat_color("irc.color.nick_change"),
                                                 assigned,
                                                 weechat_color("reset"));
                    }
                }
            }
        }

        if (is_new_room && channel)
        {
            // Status 201: new room was created — send empty config submit to unlock it
            char *room_jid = xmpp_jid_bare(account.context, binding.from->full.data());
            xmpp_string_guard owner_id_g(account.context, xmpp_uuid_gen(account.context));
            const char *owner_id = owner_id_g.ptr;
            xmpp_stanza_t *iq = xmpp_iq_new(account.context, "set", owner_id);
            xmpp_stanza_set_to(iq, room_jid);
            xmpp_stanza_t *query = xmpp_stanza_new(account.context);
            xmpp_stanza_set_name(query, "query");
            xmpp_stanza_set_ns(query, "http://jabber.org/protocol/muc#owner");
            xmpp_stanza_t *x_form = xmpp_stanza_new(account.context);
            xmpp_stanza_set_name(x_form, "x");
            xmpp_stanza_set_ns(x_form, "jabber:x:data");
            xmpp_stanza_set_attribute(x_form, "type", "submit");
            xmpp_stanza_add_child(query, x_form);
            xmpp_stanza_add_child(iq, query);
            xmpp_stanza_release(x_form);
            xmpp_stanza_release(query);
            xmpp_send(account.connection, iq);
            xmpp_stanza_release(iq);
            // freed by owner_id_g
            xmpp_free(account.context, room_jid);
        }
    }
    else
    {
        user = user::search(&account, binding.from->full.data());
        if (!user)
        {
            auto name = binding.from->full.data();
            user = &account.users.emplace(std::piecewise_construct,
                                          std::forward_as_tuple(name),
                                          std::forward_as_tuple(&account, channel, name,
                                                                channel && binding.from->bare.data() == channel->id
                                                                ? (binding.from->resource.size() ? binding.from->resource.data() : "")
                                                                : binding.from->full.data())).first->second;
        }
        auto status = binding.status();
        auto show = binding.show();
        auto idle = binding.idle_since();
        user->profile.status_text = status ? std::optional<std::string>(status->data()) : std::nullopt;
        user->profile.status = show ? std::optional<std::string>(show->data()) : std::nullopt;
        user->profile.idle = idle ? fmt::format("{}", *idle) : std::string();
        user->is_away = show ? *show == "away" : false;
        user->profile.role = std::nullopt;
        user->profile.affiliation = std::nullopt;
        
        // For roster contacts (not in a MUC), manage account buffer nicklist
        if (!channel)
        {
            if (binding.type && *binding.type == "unavailable")
            {
                // User went offline, remove from account nicklist
                user->nicklist_remove(&account, nullptr);
            }
            else
            {
                // User is online (or status update), add/update in account nicklist
                user->nicklist_remove(&account, nullptr);  // Remove first to avoid duplicates
                user->nicklist_add(&account, nullptr);
            }
        }
        
        if (channel)
        {
            if (auto signature = binding.signature(); signature)
            {
                char *pgp_raw = account.pgp.verify(channel->buffer, signature->data());
                user->profile.pgp_id = pgp_raw ? std::optional<std::string>(pgp_raw) : std::nullopt;
                free(pgp_raw);
                if (user->profile.pgp_id.has_value() && channel->type != weechat::channel::chat_type::MUC)
                    channel->pgp.ids.emplace(user->profile.pgp_id.value());
            }

            if (user->profile.role.has_value())
                channel->remove_member(binding.from->full.data(), status ? status->data() : nullptr);
            else
                channel->add_member(binding.from->full.data(), clientid.data());
        }
    }

    // XEP-0153: vCard-Based Avatars — parse photo hash from presence <x>
    // Only fetch vCards for roster contacts (non-MUC presences).
    // MUC occupant presences come from room@conference.example/nick — the bare
    // JID is the room itself, not a real user, so vcard-temp requests there
    // are meaningless and would spam the server.
    {
        xmpp_stanza_t *vcard_x = xmpp_stanza_get_child_by_name_and_ns(
            stanza, "x", "vcard-temp:x:update");
        bool is_muc_presence = channel && channel->type == weechat::channel::chat_type::MUC;
        if (vcard_x && user && !is_muc_presence)
        {
            xmpp_stanza_t *photo = xmpp_stanza_get_child_by_name(vcard_x, "photo");
            if (photo)
            {
                char *photo_hash = xmpp_stanza_get_text(photo);
                if (photo_hash && strlen(photo_hash) > 0)
                {
                    // Store the vCard avatar hash if we don't already have one
                    // from XEP-0084 (prefer PEP avatar over legacy vCard avatar)
                    if (user->profile.avatar_hash.empty())
                    {
                        user->profile.avatar_hash = photo_hash;
                    }
                    // Auto-fetch vCard to retrieve the photo and profile info if
                    // we haven't fetched it yet, or if the hash changed.
                    bool hash_changed = (user->profile.avatar_hash != photo_hash);
                    if (!user->profile.vcard_fetched || hash_changed)
                    {
                        // Use the bare JID for the vCard request
                        const char *from_full = xmpp_stanza_get_from(stanza);
                        if (from_full)
                        {
                            char *bare = xmpp_jid_bare(account.context, from_full);
                            if (bare)
                            {
                                xmpp_stanza_t *iq = ::xmpp::xep0054::vcard_request(account.context, bare);
                                const char *req_id = xmpp_stanza_get_id(iq);
                                if (req_id)
                                    account.whois_queries[req_id] = { account.buffer, std::string(bare) };
                                send(iq);
                                xmpp_stanza_release(iq);
                                xmpp_free(account.context, bare);
                            }
                        }
                    }
                    xmpp_free(account.context, photo_hash);
                }
            }
        }
    }

    return true;
}

// XEP-0071: XHTML-IM — recursively extract plain text from an XHTML stanza tree.
// Handles block elements (p, div, br) by inserting newlines/spaces.
// Map a CSS color value (name or #rrggbb/#rgb) to a WeeChat color string.
// Returns empty string if no reasonable mapping found.
static std::string css_color_to_weechat(const std::string &css)
{
    // Named colors → WeeChat terminal color names
    struct { const char *css; const char *wc; } table[] = {
        {"black",   "black"},
        {"white",   "white"},
        {"red",     "red"},
        {"green",   "green"},
        {"blue",    "blue"},
        {"yellow",  "yellow"},
        {"cyan",    "cyan"},
        {"magenta", "magenta"},
        {"orange",  "214"},    // WeeChat 256-color index
        {"gray",    "gray"},
        {"grey",    "gray"},
        {"darkgray","darkgray"},
        {"darkgrey","darkgray"},
        {"purple",  "magenta"},
        {"pink",    "213"},
        {"brown",   "130"},
        {"lime",    "46"},
        {"teal",    "30"},
        {"navy",    "18"},
        {"silver",  "250"},
        {nullptr, nullptr}
    };
    // Lowercase the input for comparison
    std::string lc = css;
    for (auto &c : lc) c = (char)tolower((unsigned char)c);
    // Strip leading/trailing spaces
    auto s = lc.find_first_not_of(" \t");
    auto e = lc.find_last_not_of(" \t");
    if (s == std::string::npos) return "";
    lc = lc.substr(s, e - s + 1);

    for (int i = 0; table[i].css; ++i)
        if (lc == table[i].css) return table[i].wc;

    // #rrggbb or #rgb → WeeChat uses "color(r,g,b)" syntax for 24-bit,
    // but for broad terminal compat map to nearest of the 16 named colors.
    if (lc.size() >= 4 && lc[0] == '#')
    {
        unsigned r = 0, g = 0, b = 0;
        if (lc.size() == 7)
        {
            r = std::stoul(lc.substr(1,2), nullptr, 16);
            g = std::stoul(lc.substr(3,2), nullptr, 16);
            b = std::stoul(lc.substr(5,2), nullptr, 16);
        }
        else if (lc.size() == 4)
        {
            r = std::stoul(lc.substr(1,1), nullptr, 16) * 17;
            g = std::stoul(lc.substr(2,1), nullptr, 16) * 17;
            b = std::stoul(lc.substr(3,1), nullptr, 16) * 17;
        }
        // Map to nearest basic color
        if (r > 200 && g < 100 && b < 100) return "red";
        if (r < 100 && g > 150 && b < 100) return "green";
        if (r < 100 && g < 100 && b > 150) return "blue";
        if (r > 150 && g > 150 && b < 100) return "yellow";
        if (r < 100 && g > 150 && b > 150) return "cyan";
        if (r > 150 && g < 100 && b > 150) return "magenta";
        if (r > 180 && g > 120 && b < 80)  return "214"; // orange
        if (r > 180 && g > 180 && b > 180) return "white";
        if (r < 80  && g < 80  && b < 80)  return "black";
        if (r > 100 && g > 100 && b > 100) return "gray";
    }
    return "";
}

// Parse an inline CSS style string and return opening/closing WeeChat color codes.
// Handles: color, background-color, font-weight, font-style, text-decoration.
static std::pair<std::string, std::string> css_style_to_weechat(const char *style)
{
    if (!style) return {"", ""};

    std::string open, close;
    std::string s(style);

    // Iterate over semicolon-separated declarations
    size_t pos = 0;
    while (pos < s.size())
    {
        size_t semi = s.find(';', pos);
        std::string decl = s.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
        pos = (semi == std::string::npos) ? s.size() : semi + 1;

        size_t colon = decl.find(':');
        if (colon == std::string::npos) continue;
        std::string prop = decl.substr(0, colon);
        std::string val  = decl.substr(colon + 1);

        // Trim
        auto trim = [](std::string &str) {
            auto a = str.find_first_not_of(" \t");
            auto b = str.find_last_not_of(" \t");
            str = (a == std::string::npos) ? "" : str.substr(a, b - a + 1);
            for (auto &c : str) c = (char)tolower((unsigned char)c);
        };
        trim(prop); trim(val);

        if (prop == "font-weight" && val == "bold")
        {
            open  += weechat_color("bold");
            close  = std::string(weechat_color("-bold")) + close;
        }
        else if (prop == "font-style" && val == "italic")
        {
            open  += weechat_color("italic");
            close  = std::string(weechat_color("-italic")) + close;
        }
        else if (prop == "text-decoration" && val == "underline")
        {
            open  += weechat_color("underline");
            close  = std::string(weechat_color("-underline")) + close;
        }
        else if (prop == "color")
        {
            std::string wc = css_color_to_weechat(val);
            if (!wc.empty())
            {
                open  += weechat_color(wc.c_str());
                close  = std::string(weechat_color("resetcolor")) + close;
            }
        }
        // background-color: intentionally ignored (terminal BG control is risky)
    }
    return {open, close};
}

// Recursively convert an XHTML-IM stanza tree to a WeeChat-formatted string.
// Inline elements emit WeeChat color/attribute codes; block elements add newlines.
// blockquote content is prefixed with "> " in green on each line.
static std::string xhtml_to_weechat(xmpp_stanza_t *stanza, bool in_blockquote = false)
{
    std::string result;
    for (xmpp_stanza_t *child = xmpp_stanza_get_children(stanza);
         child; child = xmpp_stanza_get_next(child))
    {
        if (xmpp_stanza_is_text(child))
        {
            const char *raw = xmpp_stanza_get_text_ptr(child);
            if (!raw) continue;
            if (in_blockquote)
            {
                // Prefix every line with "> "
                std::string txt(raw);
                std::string line;
                for (char c : txt)
                {
                    if (c == '\n')
                    {
                        result += weechat_color("green");
                        result += "> ";
                        result += weechat_color("resetcolor");
                        result += line;
                        result += '\n';
                        line.clear();
                    }
                    else line += c;
                }
                if (!line.empty())
                {
                    result += weechat_color("green");
                    result += "> ";
                    result += weechat_color("resetcolor");
                    result += line;
                }
            }
            else
            {
                result += raw;
            }
            continue;
        }

        const char *name = xmpp_stanza_get_name(child);
        if (!name) continue;

        // ── Block-level elements ──────────────────────────────────────
        bool is_block = (strcmp(name, "p") == 0
                      || strcmp(name, "div") == 0
                      || strcmp(name, "li") == 0);
        bool is_br         = (strcmp(name, "br") == 0);
        bool is_blockquote = (strcmp(name, "blockquote") == 0);
        bool is_pre        = (strcmp(name, "pre") == 0);

        if (is_br)
        {
            result += '\n';
            if (in_blockquote)
            {
                result += weechat_color("green");
                result += "> ";
                result += weechat_color("resetcolor");
            }
            continue;
        }

        if (is_blockquote)
        {
            if (!result.empty() && result.back() != '\n') result += '\n';
            // Render blockquote contents with in_blockquote=true
            std::string inner = xhtml_to_weechat(child, true);
            // Prefix the very first line too
            result += weechat_color("green");
            result += "> ";
            result += weechat_color("resetcolor");
            result += inner;
            if (!result.empty() && result.back() != '\n') result += '\n';
            continue;
        }

        if (is_pre)
        {
            if (!result.empty() && result.back() != '\n') result += '\n';
            result += weechat_color("gray");
            result += xhtml_to_weechat(child, in_blockquote);
            result += weechat_color("resetcolor");
            if (!result.empty() && result.back() != '\n') result += '\n';
            continue;
        }

        if (is_block)
        {
            if (!result.empty() && result.back() != '\n') result += '\n';
            result += xhtml_to_weechat(child, in_blockquote);
            if (!result.empty() && result.back() != '\n') result += '\n';
            continue;
        }

        // ── Inline elements ───────────────────────────────────────────
        if (strcmp(name, "b") == 0 || strcmp(name, "strong") == 0)
        {
            result += weechat_color("bold");
            result += xhtml_to_weechat(child, in_blockquote);
            result += weechat_color("-bold");
            continue;
        }

        if (strcmp(name, "i") == 0 || strcmp(name, "em") == 0)
        {
            result += weechat_color("italic");
            result += xhtml_to_weechat(child, in_blockquote);
            result += weechat_color("-italic");
            continue;
        }

        if (strcmp(name, "u") == 0)
        {
            result += weechat_color("underline");
            result += xhtml_to_weechat(child, in_blockquote);
            result += weechat_color("-underline");
            continue;
        }

        if (strcmp(name, "del") == 0 || strcmp(name, "s") == 0 || strcmp(name, "strike") == 0)
        {
            // No real strikethrough in most terminals; use dim/darkgray
            result += weechat_color("darkgray");
            result += xhtml_to_weechat(child, in_blockquote);
            result += weechat_color("resetcolor");
            continue;
        }

        if (strcmp(name, "code") == 0 || strcmp(name, "tt") == 0)
        {
            result += weechat_color("gray");
            result += xhtml_to_weechat(child, in_blockquote);
            result += weechat_color("resetcolor");
            continue;
        }

        if (strcmp(name, "span") == 0)
        {
            const char *style_attr = xmpp_stanza_get_attribute(child, "style");
            auto [open, close] = css_style_to_weechat(style_attr);
            result += open;
            result += xhtml_to_weechat(child, in_blockquote);
            result += close;
            continue;
        }

        if (strcmp(name, "a") == 0)
        {
            const char *href = xmpp_stanza_get_attribute(child, "href");
            std::string link_text = xhtml_to_weechat(child, in_blockquote);
            if (href && *href)
            {
                // If link text equals the URL, just show it in blue
                if (link_text == href)
                {
                    result += weechat_color("blue");
                    result += href;
                    result += weechat_color("resetcolor");
                }
                else
                {
                    result += link_text;
                    result += ' ';
                    result += weechat_color("blue");
                    result += '(';
                    result += href;
                    result += ')';
                    result += weechat_color("resetcolor");
                }
            }
            else
            {
                result += link_text;
            }
            continue;
        }

        if (strcmp(name, "img") == 0)
        {
            // Show alt text, or a placeholder
            const char *alt = xmpp_stanza_get_attribute(child, "alt");
            const char *src = xmpp_stanza_get_attribute(child, "src");
            result += weechat_color("darkgray");
            result += '[';
            result += (alt && *alt) ? alt : (src ? src : "image");
            result += ']';
            result += weechat_color("resetcolor");
            continue;
        }

        // Fallback: recurse without any special formatting (e.g. <body>, <html>, unknown tags)
        result += xhtml_to_weechat(child, in_blockquote);
    }
    return result;
}

