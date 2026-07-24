int command__ping(const void *pointer, void *data,
                  struct t_gui_buffer *buffer, int argc,
                  char **argv, char **argv_eol)
{
    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;
    const char *target = nullptr;

    (void) pointer;
    (void) data;
    (void) argv_eol;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    if (!ptr_account->connected())
    {
        weechat::UiPort::for_buffer(buffer)->printf_error("xmpp: you are not connected to server");
        return WEECHAT_RC_OK;
    }

    // Determine ping target: specified argument, channel JID, or server
    if (argc > 1)
        target = argv[1];
    else if (ptr_channel)
        target = ptr_channel->id.data();
    else
        target = nullptr;  // Ping the server

    std::string id = stanza::uuid(ptr_account->context);

    // Track ping time for response measurement
    ptr_account->user_ping_queries[id] = time(nullptr);

    auto ui = weechat::UiPort::for_buffer(buffer);
    if (target)
        ui->printf_network(fmt::format("Sending ping to {}...", target));
    else
        ui->printf_network("Sending ping to server...");

    auto iq = stanza::iq()
        .type("get")
        .id(id);
    if (target)
        iq.to(target);
    iq.ping();
    ptr_account->connection.send(iq.build(ptr_account->context).get());

    return WEECHAT_RC_OK;
}

int command__mood(const void *pointer, void *data,
                  struct t_gui_buffer *buffer, int argc,
                  char **argv, char **argv_eol)
{
    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;

    (void) pointer;
    (void) data;
    (void) argv;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    auto ui = weechat::UiPort::for_buffer(buffer);

    if (!ptr_account->connected())
    {
        ui->printf_error("xmpp: you are not connected to server");
        return WEECHAT_RC_OK;
    }

    // XEP-0107 mood values (from spec)
    const char *valid_moods[] = {
        "afraid", "amazed", "angry", "amorous", "annoyed", "anxious",
        "aroused", "ashamed", "bored", "brave", "calm", "cautious",
        "cold", "confident", "confused", "contemplative", "contented",
        "cranky", "crazy", "creative", "curious", "dejected", "depressed",
        "disappointed", "disgusted", "dismayed", "distracted", "embarrassed",
        "envious", "excited", "flirtatious", "frustrated", "grumpy", "guilty",
        "happy", "hopeful", "hot", "humbled", "humiliated", "hungry",
        "hurt", "impressed", "in_awe", "in_love", "indignant", "interested",
        "intoxicated", "invincible", "jealous", "lonely", "lucky", "mean",
        "moody", "nervous", "neutral", "offended", "outraged", "playful",
        "proud", "relaxed", "relieved", "remorseful", "restless", "sad",
        "sarcastic", "serious", "shocked", "shy", "sick", "sleepy",
        "spontaneous", "stressed", "strong", "surprised", "thankful",
        "thirsty", "tired", "undefined", "weak", "worried", nullptr
    };

    const char *mood = nullptr;
    const char *text = nullptr;

    if (argc >= 2)
    {
        mood = argv[1];
        if (argc >= 3)
            text = argv_eol[2];
    }

    // No arguments: open interactive picker
    if (!mood)
    {
        using picker_t = weechat::ui::picker<std::string>;
        std::vector<picker_t::entry> entries;
        for (int i = 0; valid_moods[i] != nullptr; i++)
            entries.push_back({ std::string(valid_moods[i]), valid_moods[i], "", true });

        auto p = std::make_unique<picker_t>(
            "xmpp.picker.mood",
            "Select a mood  (XEP-0107)",
            std::move(entries),
            [buf = buffer](std::string_view selected) {
                auto cmd = fmt::format("/mood {}", selected);
                weechat_command(buf, cmd.c_str());
            },
            picker_t::close_cb{},
            buffer);
        if (!*p) return WEECHAT_RC_ERROR;
        p.release();
        return WEECHAT_RC_OK;
    }

    // Validate mood
    bool valid = false;
    for (int i = 0; valid_moods[i] != nullptr; i++)
    {
        if (weechat_strcasecmp(mood, valid_moods[i]) == 0)
        {
            valid = true;
            break;
        }
    }

    if (!valid)
    {
        ui->printf_error(fmt::format("xmpp: invalid mood '{}'", mood));
        ui->printf_error("Valid moods: happy, sad, angry, excited, tired, etc.");
        return WEECHAT_RC_OK;
    }

    // Build PEP mood publish stanza using stanza:: builder
    // <iq type='set' id='...'>
    //   <pubsub xmlns='http://jabber.org/protocol/pubsub'>
    //     <publish node='http://jabber.org/protocol/mood'>
    //       <item>
    //         <mood xmlns='http://jabber.org/protocol/mood'>
    //           <happy/>  <!-- or other mood -->
    //           <text>Feeling great!</text>  <!-- optional -->
    //         </mood>
    //       </item>
    //     </publish>
    //   </pubsub>
    // </iq>

    // Inner mood element: <mood xmlns='http://jabber.org/protocol/mood'>
    struct mood_payload : virtual public stanza::spec {
        mood_payload(std::string_view mood_s, std::optional<std::string_view> text_s)
            : spec("mood") {
            xmlns<jabber_org::protocol::mood>();
            // Add mood value sub-element, e.g. <happy/>
            struct mood_value : virtual public spec {
                mood_value(std::string_view n) : spec(n) {}
            };
            mood_value mv(mood_s);
            child(mv);
            if (text_s)
            {
                struct text_el : virtual public spec {
                    text_el(std::string_view s) : spec("text") { text(s); }
                };
                text_el te(*text_s);
                child(te);
            }
        }
    };

    mood_payload mp(mood, text ? std::optional<std::string_view>(text) : std::nullopt);
    auto item = stanza::xep0060::item().payload(mp);
    auto publish = stanza::xep0060::publish("http://jabber.org/protocol/mood").item(item);
    auto pubsub = stanza::xep0060::pubsub().publish(publish);
    auto iq = stanza::iq()
        .type("set")
        .id(stanza::uuid(ptr_account->context))
        .pubsub(pubsub);
    ptr_account->connection.send(iq.build(ptr_account->context).get());

    if (text)
        ui->printf_network(fmt::format("xmpp: mood set to '{}': {}", mood, text));
    else
        ui->printf_network(fmt::format("xmpp: mood set to '{}'", mood));

    return WEECHAT_RC_OK;
}

int command__activity(const void *pointer, void *data,
                      struct t_gui_buffer *buffer, int argc,
                      char **argv, char **argv_eol)
{
    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;

    (void) pointer;
    (void) data;
    (void) argv;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    auto ui = weechat::UiPort::for_buffer(buffer);

    if (!ptr_account->connected())
    {
        ui->printf_error("xmpp: you are not connected to server");
        return WEECHAT_RC_OK;
    }

    // XEP-0108 activity categories and specific activities
    // Format: category/specific (e.g., "working/coding")
    const char *valid_categories[] = {
        "doing_chores", "drinking", "eating", "exercising", "grooming",
        "having_appointment", "inactive", "relaxing", "talking", "traveling",
        "working", nullptr
    };

    std::string category_s;
    std::string specific_s;
    const char *specific = nullptr;
    const char *text = nullptr;

    if (argc >= 2)
    {
        // Parse "category" or "category/specific"
        category_s = argv[1];
        std::string::size_type slash_pos = category_s.find('/');

        if (slash_pos != std::string::npos)
        {
            specific_s = category_s.substr(slash_pos + 1);
            category_s.resize(slash_pos);
            specific = specific_s.c_str();
        }

        if (argc >= 3)
            text = argv_eol[2];
    }

    const char *category = category_s.empty() ? nullptr : category_s.c_str();

    // No arguments: open interactive picker
    if (!category)
    {
        using picker_t = weechat::ui::picker<std::string>;
        std::vector<picker_t::entry> entries;
        for (int i = 0; valid_categories[i] != nullptr; i++)
            entries.push_back({ std::string(valid_categories[i]), valid_categories[i], "", true });

        auto p = std::make_unique<picker_t>(
            "xmpp.picker.activity",
            "Select an activity  (XEP-0108)",
            std::move(entries),
            [buf = buffer](std::string_view selected) {
                auto cmd = fmt::format("/activity {}", selected);
                weechat_command(buf, cmd.c_str());
            },
            picker_t::close_cb{},
            buffer);
        if (!*p) return WEECHAT_RC_ERROR;
        p.release();
        return WEECHAT_RC_OK;
    }

    // Validate category
    bool valid = false;
    for (int i = 0; valid_categories[i] != nullptr; i++)
    {
        if (weechat_strcasecmp(category, valid_categories[i]) == 0)
        {
            valid = true;
            break;
        }
    }

    if (!valid)
    {
        ui->printf_error(fmt::format("xmpp: invalid activity category '{}'", category));
        ui->printf_error("Valid categories: working, relaxing, eating, drinking, traveling, etc.");
        return WEECHAT_RC_OK;
    }

    // Build PEP activity publish stanza using stanza:: builder
    // <iq type='set'>
    //   <pubsub xmlns='http://jabber.org/protocol/pubsub'>
    //     <publish node='http://jabber.org/protocol/activity'>
    //       <item>
    //         <activity xmlns='http://jabber.org/protocol/activity'>
    //           <working>  <!-- category -->
    //             <coding/>  <!-- specific, optional -->
    //           </working>
    //           <text>Writing XMPP code</text>  <!-- optional -->
    //         </activity>
    //       </item>
    //     </publish>
    //   </pubsub>
    // </iq>

    struct activity_payload : virtual public stanza::spec {
        activity_payload(std::string_view cat, std::optional<std::string_view> spec_s,
                         std::optional<std::string_view> text_s)
            : spec("activity") {
            xmlns<jabber_org::protocol::activity>();
            // Add category element with optional specific sub-element
            struct category_el : virtual public stanza::spec {
                category_el(std::string_view n, std::optional<std::string_view> sp)
                    : stanza::spec(n) {
                    if (sp) {
                        struct specific_el : virtual public stanza::spec {
                            specific_el(std::string_view s) : stanza::spec(s) {}
                        };
                        specific_el se(*sp);
                        child(se);
                    }
                }
            };
            category_el ce(cat, spec_s);
            child(ce);
            if (text_s)
            {
                struct text_el : virtual public stanza::spec {
                    text_el(std::string_view s) : stanza::spec("text") { text(s); }
                };
                text_el te(*text_s);
                child(te);
            }
        }
    };

    std::optional<std::string_view> spec_opt = specific ? std::optional<std::string_view>(specific) : std::nullopt;
    std::optional<std::string_view> text_opt = text ? std::optional<std::string_view>(text) : std::nullopt;
    activity_payload ap(category, spec_opt, text_opt);
    auto item = stanza::xep0060::item().payload(ap);
    auto publish = stanza::xep0060::publish("http://jabber.org/protocol/activity").item(item);
    auto pubsub = stanza::xep0060::pubsub().publish(publish);
    auto iq = stanza::iq()
        .type("set")
        .id(stanza::uuid(ptr_account->context))
        .pubsub(pubsub);
    ptr_account->connection.send(iq.build(ptr_account->context).get());

    if (specific && text)
        ui->printf_network(fmt::format("xmpp: activity set to '{}/{}': {}", argv[1], specific, text));
    else if (specific)
        ui->printf_network(fmt::format("xmpp: activity set to '{}/{}'", argv[1], specific));
    else if (text)
        ui->printf_network(fmt::format("xmpp: activity set to '{}': {}", argv[1], text));
    else
        ui->printf_network(fmt::format("xmpp: activity set to '{}'", argv[1]));

    return WEECHAT_RC_OK;
}

int command__selfping(const void *pointer, void *data,
                     struct t_gui_buffer *buffer, int argc,
                     char **argv, char **argv_eol)
{
    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;

    (void) pointer;
    (void) data;
    (void) argv;
    (void) argv_eol;
    (void) argc;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    auto ui = weechat::UiPort::for_buffer(buffer);

    if (!ptr_account->connected())
    {
        ui->printf_error("xmpp: you are not connected to server");
        return WEECHAT_RC_OK;
    }

    if (!ptr_channel)
    {
        ui->printf_error(fmt::format("{}: this command must be used in a MUC buffer", argv[0]));
        return WEECHAT_RC_OK;
    }

    // Check if this is a MUC channel
    if (ptr_channel->type != weechat::channel::chat_type::MUC)
    {
        ui->printf_error(fmt::format("{}: this command must be used in a MUC buffer", argv[0]));
        return WEECHAT_RC_OK;
    }

    // Construct our full MUC JID (room@server/nickname)
    std::string muc_jid = std::string(ptr_channel->id) + "/" + std::string(ptr_account->nickname());

        ui->printf_network(fmt::format("Sending MUC self-ping to {}...", muc_jid.c_str()));

    // Send self-ping to our own MUC nickname
    auto iq = stanza::iq()
        .type("get")
        .id(stanza::uuid(ptr_account->context))
        .to(muc_jid)
        .ping();
    ptr_account->connection.send(iq.build(ptr_account->context).get());

    return WEECHAT_RC_OK;
}

int command__whois(const void *pointer, void *data,
                  struct t_gui_buffer *buffer, int argc,
                  char **argv, char **argv_eol)
{
    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;
    const char *target = nullptr;

    (void) pointer;
    (void) data;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    auto ui = weechat::UiPort::for_buffer(buffer);

    if (!ptr_account->connected())
    {
        ui->printf_error("xmpp: you are not connected to server");
        return WEECHAT_RC_OK;
    }

    // Determine target: specified argument, current channel, or MUC nick
    std::string target_storage;
    if (argc > 1)
    {
        const char *arg_raw = argv_eol[1];
        if (!arg_raw || !*arg_raw)
        {
        ui->printf_error(fmt::format("{}: missing JID argument", argv[0]));
            return WEECHAT_RC_OK;
        }

        std::string_view arg(arg_raw);
        // In a MUC buffer, treat arguments without '@' as nicks (argv_eol so
        // multi-word nicks like "Zero Tolerance" are not split on spaces).
        if (ptr_channel && ptr_channel->type == weechat::channel::chat_type::MUC
            && !arg.contains('@'))
        {
            target_storage = ptr_channel->find_member_by_nick(arg);
            if (target_storage.empty())
            {
        ui->printf_error(fmt::format("{}: nick '{}' not found in this MUC", argv[0], arg_raw));
                return WEECHAT_RC_OK;
            }
            target = target_storage.c_str();
        }
        else
        {
            target = arg_raw;
        }
    }
    else if (ptr_channel && ptr_channel->type == weechat::channel::chat_type::PM)
        target = ptr_channel->id.data();
    else
    {
        ui->printf_error(fmt::format("{}: missing JID argument", argv[0]));
        return WEECHAT_RC_OK;
    }

    // In MUC buffers, show occupant presence info and prefer the real JID
    // for vCard lookups when the room is non-anonymous.
    if (ptr_channel && ptr_channel->type == weechat::channel::chat_type::MUC)
    {
        auto member_opt = ptr_channel->member_search(target);
        auto user = member_opt
            ? weechat::user::search(ptr_account, (*member_opt)->id.c_str())
            : nullptr;

        ui->printf_network(fmt::format("Presence for {}:", target));

        if (user)
        {
            auto fmt_opt = [](const std::optional<std::string>& v) -> std::string_view {
                return v ? std::string_view(*v) : std::string_view{"-"};
            };
        ui->printf(fmt::format("  Role:        {}", fmt_opt(user->profile.role).data()));
        ui->printf(fmt::format("  Affiliation: {}", fmt_opt(user->profile.affiliation).data()));
        ui->printf(fmt::format("  Status:      {}{}{}", user->profile.status.has_value() ? user->profile.status->c_str() : "online", user->profile.status_text.has_value() ? " [" : "", user->profile.status_text.has_value() ? user->profile.status_text->c_str() : "", user->profile.status_text.has_value() ? "]" : ""));
            if (user->profile.idle)
        ui->printf(fmt::format("  Idle:        {}", user->profile.idle->c_str()));
        }

        if (member_opt && (*member_opt)->real_jid)
        {
            std::string_view real_jid = *(*member_opt)->real_jid;
        ui->printf(fmt::format("  Real JID:    {}", real_jid.data()));
            // vCard requests must target the bare JID (XEP-0054 §3.2, XEP-0292 §4.1.1)
            target_storage = ::jid(nullptr, std::string(real_jid)).bare;
            target = target_storage.c_str();
        }
        else
        {
        ui->printf(fmt::format("  {}Room is anonymous — vCard request sent to room JID", weechat::RuntimePort::default_runtime().color("darkgray")));
        }
    }

    // Request vCard using XEP-0054 (legacy)
    xmpp_stanza_t *iq = xmpp::xep0054::vcard_request(ptr_account->context, target);
    const std::string req_id = ::xmpp::StanzaView(iq).attr_string("id");
    if (!req_id.empty())
        ptr_account->whois_queries[req_id] = { buffer, std::string(target) };

    // Also request vCard4 via PubSub (XEP-0292) — servers that support it will
    // respond; others will return an error which we silently ignore.
    xmpp_stanza_t *iq4 = xmpp::xep0292::vcard4_request(ptr_account->context, target);
    const std::string req_id4 = ::xmpp::StanzaView(iq4).attr_string("id");
    if (!req_id4.empty())
        ptr_account->whois_queries[req_id4] = { buffer, std::string(target) };

    ui->printf_network(fmt::format("Requesting vCard for {}...", target));

    ptr_account->connection.send(iq);
    xmpp_stanza_release(iq);
    ptr_account->connection.send(iq4);
    xmpp_stanza_release(iq4);

    return WEECHAT_RC_OK;
}

// /setvcard field value
// Sets a single field of your own vCard (XEP-0054 IQ set).
// Usage: /setvcard <field> <value>
// Fields: fn, nickname, email, url, desc, org, title, tel, bday, note
int command__setvcard(const void *pointer, void *data,
                      struct t_gui_buffer *buffer, int argc,
                      char **argv, char **argv_eol)
{
    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;

    (void) pointer;
    (void) data;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    auto ui = weechat::UiPort::for_buffer(buffer);

    if (!ptr_account->connected())
    {
        ui->printf_error("xmpp: you are not connected to server");
        return WEECHAT_RC_OK;
    }

    if (argc < 3)
    {
        ui->printf_error(fmt::format("{}: usage: /setvcard <field> <value>\n"
                       "  fields: fn, nickname, email, url, desc, org, title, tel, bday, note", argv[0]));
        return WEECHAT_RC_OK;
    }

    std::string field(argv[1]);
    std::string value(argv_eol[2]);

    // Validate the field name first
    static const std::vector<std::string> valid_fields = {
        "fn", "nickname", "email", "url", "desc", "org", "title", "tel", "bday", "note"
    };
    if (!std::ranges::contains(valid_fields, field))
    {
        ui->printf_error(fmt::format("{}: unknown vCard field '{}'\n"
                       "  valid fields: fn, nickname, email, url, desc, org, title, tel, bday, note", argv[0], argv[1]));
        return WEECHAT_RC_OK;
    }

    // Fetch our own vCard first so we can merge the single field change without
    // clobbering the rest of the vCard (XEP-0054 IQ set replaces the entire vCard).
    xmpp_stanza_t *iq = xmpp::xep0054::vcard_request(ptr_account->context, nullptr);
    const std::string req_id = ::xmpp::StanzaView(iq).attr_string("id");
    if (!req_id.empty())
        ptr_account->setvcard_queries[req_id] = { buffer, field, value };
    ui->printf_network(fmt::format("Fetching current vCard before updating {}...", field.c_str()));
    ptr_account->connection.send(iq);
    xmpp_stanza_release(iq);

    return WEECHAT_RC_OK;
}

// /setavatar <filepath>
// Publish a local image file as own avatar via XEP-0084 (User Avatar).
// Supported formats: PNG, JPEG, GIF, WEBP (detected from extension).
int command__setavatar(const void *pointer, void *data,
                       struct t_gui_buffer *buffer, int argc,
                       char **argv, char **argv_eol)
{
    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;

    (void) pointer;
    (void) data;
    (void) argv;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    auto ui = weechat::UiPort::for_buffer(buffer);

    if (!ptr_account->connected())
    {
        ui->printf_error("xmpp: you are not connected to server");
        return WEECHAT_RC_OK;
    }

    if (argc < 2)
    {
        ui->printf_error(fmt::format("{}: usage: /setavatar <filepath>\n"
                       "  filepath: path to a PNG, JPEG, GIF, or WEBP image file", argv[0]));
        return WEECHAT_RC_OK;
    }

    std::string filepath = argv_eol[1] ? argv_eol[1] : "";
    if (filepath.size() >= 2
        && ((filepath.front() == '"' && filepath.back() == '"')
            || (filepath.front() == '\'' && filepath.back() == '\'')))
    {
        filepath = filepath.substr(1, filepath.size() - 2);
    }
    filepath = expand_tilde_path(filepath);
    weechat::avatar::publish(*ptr_account, filepath);

    return WEECHAT_RC_OK;
}
