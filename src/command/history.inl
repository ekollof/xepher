// ---------------------------------------------------------------------------
// msg_entry — one buffer line's worth of message metadata, used by the
// Tier 3 message-history pickers (/retract, /reply, /moderate, /edit).
// ---------------------------------------------------------------------------
struct msg_entry {
    std::string id;           // origin-id / message id
    std::string stanza_id;    // MUC stanza-id (XEP-0359)
    std::string stanza_id_by; // JID that assigned stanza_id
    std::string nick;         // nick_ tag value
    std::string body;         // displayed message text (may have colour codes)
    bool        from_self = false;
    bool        retracted = false;
};

// Walk up to `max` buffer lines backwards, newest first.
// Returns entries that have a non-empty id OR a non-empty stanza_id, so that
// MAM-replayed MUC messages (which may only carry stanza_id_) are included.
static std::vector<msg_entry>
collect_buffer_messages(struct t_gui_buffer *buffer, int max)
{
    std::vector<msg_entry> result;

    void *lines = weechat_hdata_pointer(weechat_hdata_get("buffer"),
                                        buffer, "lines");
    if (!lines) return result;

    void *cur = weechat_hdata_pointer(weechat_hdata_get("lines"),
                                      lines, "last_line");

    struct t_hdata *hd_line      = weechat_hdata_get("line");
    struct t_hdata *hd_line_data = weechat_hdata_get("line_data");

    while (cur && static_cast<int>(result.size()) < max)
    {
        void *line_data = weechat_hdata_pointer(hd_line, cur, "data");
        if (line_data)
        {
            int tags_count = weechat_hdata_integer(hd_line_data, line_data, "tags_count");

            msg_entry e;
            for (int n = 0; n < tags_count; ++n)
            {
                std::string key = fmt::format("{}|tags_array", n);
                const char *tag = weechat_hdata_string(hd_line_data, line_data, key.c_str());
                if (!tag) continue;

                const std::string_view tag_sv { tag };
                if (tag_sv == "self_msg")
                    e.from_self = true;
                else if (tag_sv == "xmpp_retracted")
                    e.retracted = true;
                else if (tag_sv.starts_with("id_") && e.id.empty())
                    e.id = tag_sv.substr(3);
                else if (tag_sv.starts_with("stanza_id_by_"))
                    e.stanza_id_by = tag_sv.substr(13);
                else if (tag_sv.starts_with("stanza_id_") && e.stanza_id.empty())
                    e.stanza_id = tag_sv.substr(10);
                else if (tag_sv.starts_with("nick_") && e.nick.empty())
                    e.nick = tag_sv.substr(5);
            }

            // Accept lines that have at least one usable ID (origin-id or stanza-id).
            // MAM-replayed MUC messages may only have stanza_id_ with no id_ tag.
            if (!e.id.empty() || !e.stanza_id.empty())
            {
                // When id is absent, promote stanza_id as the fallback identity.
                if (e.id.empty())
                    e.id = e.stanza_id;
                const char *msg = weechat_hdata_string(hd_line_data, line_data, "message");
                if (msg)
                    e.body = weechat::clean_editable_line_body(msg);
                result.push_back(std::move(e));
            }
        }

        cur = weechat_hdata_pointer(hd_line, cur, "prev_line");
    }

    return result;
}

// Resolve which message ID to use for XEP operations (edit, retract, reply).
// In a MUC, prefer the stanza-id assigned by the room (XEP-0359) when it is
// available and was issued by the channel itself; fall back to origin-id.
static std::string
resolve_msg_id(const msg_entry &m, const weechat::channel *ch)
{
    if (ch
        && ch->type == weechat::channel::chat_type::MUC
        && !m.stanza_id.empty()
        && weechat_strcasecmp(m.stanza_id_by.c_str(), ch->id.c_str()) == 0)
        return m.stanza_id;
    return m.id;
}

// XEP-0461 §3.1: <reply to='…'> must be the original sender's JID.
static std::string
resolve_reply_to_jid(const weechat::channel *ch, std::string_view sender_nick)
{
    if (ch->type == weechat::channel::chat_type::MUC && !sender_nick.empty())
        return fmt::format("{}/{}", ch->name, sender_nick);
    return ch->name;
}

static std::string
reply_display_prefix(weechat::account *account, const weechat::channel *channel)
{
    if (channel->type == weechat::channel::chat_type::MUC)
    {
        const std::string_view nick = account->nickname();
        if (nick.empty())
            return account->jid();

        const std::string occupant = fmt::format("{}/{}", channel->name, nick);
        if (auto *display_user = weechat::user::search(account, occupant.c_str()))
        {
            std::string pfx;
            if (!display_user->profile.avatar_rendered.empty())
                pfx = display_user->profile.avatar_rendered + " ";
            pfx += weechat::user::as_prefix_raw(nick);
            return pfx;
        }
        return weechat::user::as_prefix_raw(nick);
    }

    auto *self_user = weechat::user::search(account, account->jid().data());
    return self_user ? std::string(self_user->as_prefix_raw()) : account->jid();
}

// Servers do not carbon-copy self-sent replies; echo locally like PM send_message.
static void
echo_outgoing_reply(weechat::account *account,
                    weechat::channel *channel,
                    struct t_gui_buffer *buffer,
                    std::string_view target_id,
                    std::string_view reply_text,
                    std::string_view origin_id)
{
    auto ui = weechat::UiPort::for_buffer(buffer);
    const std::string prefix = reply_display_prefix(account, channel);

    if (auto quote = weechat::line_store_lookup_reply_quote(buffer, target_id))
    {
        const std::string quote_line =
            xmpp::format_reply_quote_body(quote->quote_nick, quote->excerpt);
        ui->printf_date_tags(0, "notify_none,no_log,xmpp_reply_quote",
                             fmt::format("{}\t{}", prefix, quote_line));
    }

    std::string tags;
    if (channel->type == weechat::channel::chat_type::MUC)
    {
        const std::string_view nick = account->nickname();
        tags = fmt::format("xmpp_message,message,nick_{},notify_none,self_msg,log1,id_{}",
                           nick, origin_id);
    }
    else
    {
        tags = fmt::format("xmpp_message,message,private,notify_none,self_msg,log1,id_{}",
                           origin_id);
    }

    ui->printf_date_tags(0, tags.c_str(), fmt::format("{}\t{}", prefix, reply_text));
}

static void
send_reply_message(weechat::account *account,
                   weechat::channel *channel,
                   struct t_gui_buffer *buffer,
                   std::string_view target_id,
                   std::string_view reply_to_jid,
                   std::string_view reply_text)
{
    const char *type = (channel->type == weechat::channel::chat_type::MUC)
                       ? "groupchat" : "chat";

    const std::string uuid = stanza::uuid(account->context);
    const std::string origin_uuid = stanza::uuid(account->context);

    stanza::xep0461::reply reply_el(target_id, reply_to_jid);
    stanza::xep0428::fallback fallback_el("urn:xmpp:reply:0");
    stanza::xep0359::origin_id oid(origin_uuid);

    auto msg_s = stanza::message()
        .type(type)
        .to(channel->name)
        .id(uuid)
        .body(reply_text);
    msg_s.reply(reply_el);
    msg_s.fallback(fallback_el);
    msg_s.store();
    msg_s.origin_id(oid);

    account->connection.send(msg_s.build(account->context).get());
    echo_outgoing_reply(account, channel, buffer, target_id, reply_text, origin_uuid);
}

// Return up to `max` own, non-retracted messages from the buffer (newest first).
static std::vector<msg_entry>
collect_own_messages(struct t_gui_buffer *buffer, int max = 20)
{
    auto all = collect_buffer_messages(buffer, 500);
    std::vector<msg_entry> result;
    for (auto &m : all)
    {
        if (m.from_self && !m.retracted)
            result.push_back(m);
        if (static_cast<int>(result.size()) >= max)
            break;
    }
    return result;
}

// /edit [text] — XEP-0308 message correction.
//
// /edit          — open a picker of the last 20 own non-retracted messages;
//                  on selection pre-fill the input bar with "/edit-to <id> <body>"
//                  so the user can tweak the text before pressing Enter.
// /edit <text>   — immediately correct the last own non-retracted message with
//                  <text> as the new body (no picker).
//
// The actual correction stanza is sent by command__edit_to (below).
int command__edit(const void *pointer, void *data,
                  struct t_gui_buffer *buffer, int argc,
                  char **argv, char **argv_eol)
{
    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;

    (void) pointer;
    (void) data;
    (void) argv;  // only argv_eol and argc are used

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    auto ui = weechat::UiPort::for_buffer(buffer);

    if (!ptr_channel)
    {
        ui->printf_error("xmpp: you must be in a channel to edit messages");
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        ui->printf_error("xmpp: you are not connected to server");
        return WEECHAT_RC_OK;
    }

    // /edit <text> — fast path: correct the last own message immediately.
    if (argc >= 2)
    {
        auto own_messages = collect_own_messages(buffer, 1);
        if (own_messages.empty())
        {
        ui->printf_error("xmpp: no message found to edit");
            return WEECHAT_RC_OK;
        }

        // Delegate to /edit-to by pre-filling the input bar so the user sees
        // what happened, then executing immediately via weechat_command.
        // Simpler and avoids duplicating stanza construction here.
        std::string resolved_id = resolve_msg_id(own_messages[0], ptr_channel);
        std::string cmd = fmt::format("/edit-to {} {}", resolved_id, argv_eol[1]);
        weechat_command(buffer, cmd.c_str());
        return WEECHAT_RC_OK;
    }

    // /edit (no args) — picker path.
    auto own_messages = collect_own_messages(buffer);

    if (own_messages.empty())
    {
        ui->printf_error("xmpp: no message found to edit");
        return WEECHAT_RC_OK;
    }

    using picker_t = weechat::ui::picker<std::string>;
    struct edit_entry { std::string id; std::string body; };
    std::vector<edit_entry> entry_list;
    std::vector<picker_t::entry> entries;

    for (auto &m : own_messages)
    {
        // Resolve ID: prefer MUC stanza-id when applicable (XEP-0308)
        std::string resolved_id = resolve_msg_id(m, ptr_channel);

        entry_list.push_back({resolved_id, m.body});
        std::string label = m.body.empty() ? resolved_id : m.body;
        entries.push_back({resolved_id, label, {}});
    }

    auto p = std::make_unique<picker_t>(
        "xmpp.picker.edit",
        "Edit message  (XEP-0308)",
        std::move(entries),
        [buf = buffer, el = std::move(entry_list)](std::string_view selected) {
            // Find the body for the selected ID so we can pre-fill it
            std::string body;
            for (auto &e : el)
                if (e.id == selected) { body = e.body; break; }

            const std::string clean_body = weechat::clean_editable_line_body(body);
            std::string input = fmt::format("/edit-to {} {}", selected, clean_body);
            weechat_buffer_set(buf, "input", input.c_str());
            weechat_buffer_set(buf, "input_pos",
                               std::to_string(input.size()).c_str());
            weechat_buffer_set(buf, "display", "1");
        },
        picker_t::close_cb{},
        buffer);
    if (!*p) return WEECHAT_RC_ERROR;
    p.release();
    return WEECHAT_RC_OK;
}

// /edit-to <id> <text> — send a XEP-0308 message correction for <id>.
// This command is generated internally by the /edit picker which pre-fills the
// input bar; users may also call it directly if they know the message ID.
int command__edit_to(const void *pointer, void *data,
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

    if (!ptr_channel)
    {
        ui->printf_error("xmpp: you must be in a channel to edit messages");
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        ui->printf_error("xmpp: you are not connected to server");
        return WEECHAT_RC_OK;
    }

    if (argc < 3)
    {
        ui->printf_error("xmpp: usage: /edit-to <message-id> <new text>");
        return WEECHAT_RC_OK;
    }

    const char *target_id  = argv[1];
    std::string new_text   = argv_eol[2];
    if (weechat::config::instance
        && weechat::config::instance->look.emoticons.boolean())
    {
        new_text = replace_emoticons(new_text);
    }
    const char *type       = (ptr_channel->type == weechat::channel::chat_type::MUC)
                              ? "groupchat" : "chat";

    std::string new_id = stanza::uuid(ptr_account->context);

    stanza::xep0308::replace replace_el(target_id);
    stanza::xep0359::origin_id oid(new_id);

    auto msg_s = stanza::message()
        .type(type)
        .to(ptr_channel->id)
        .id(new_id)
        .body(new_text);
    msg_s.replace(replace_el);
    msg_s.store();
    msg_s.origin_id(oid);

    ptr_account->connection.send(msg_s.build(ptr_account->context).get());

    // Update the local buffer line in-place so the sender sees the correction
    // immediately.  Servers do not carbon-copy self-sent corrections, so without
    // this the buffer would keep showing the old text.
    {
        std::string new_msg = fmt::format("📝 {}", new_text);
        buffer__update_line_by_id(buffer, target_id, new_msg.c_str());
    }

        ui->printf_network("xmpp: message edit sent");
    return WEECHAT_RC_OK;
}

// Send a XEP-0424 message retraction for `msg_id` on `account`/`channel`.
static void
do_retract_send(weechat::account *account, weechat::channel *channel,
                struct t_gui_buffer *buffer, const std::string &msg_id)
{
    const char *type = channel->type == weechat::channel::chat_type::MUC
                       ? "groupchat" : "chat";

    std::string new_id = stanza::uuid(account->context);

    stanza::xep0424::retract retract_el(msg_id);
    stanza::xep0428::fallback fallback_el("urn:xmpp:message-retract:1");
    stanza::xep0359::origin_id oid(new_id);

    auto msg_s = stanza::message()
        .type(type)
        .to(channel->id)
        .id(new_id)
        .body("/me retracted a previous message, but it's unsupported by your client.");
    msg_s.retract(retract_el);
    msg_s.fallback(fallback_el);
    msg_s.store();
    msg_s.origin_id(oid);

    account->connection.send(msg_s.build(account->context).get());

    weechat::UiPort::for_buffer(buffer)->printf_network("xmpp: message retraction sent");
}

// Send a XEP-0425 moderation request for `msg_id` on `account`/`channel`.
static void
do_moderate_send(weechat::account *account, weechat::channel *channel,
                 struct t_gui_buffer *buffer, const std::string &msg_id,
                 const char *reason)
{
    const char *room_jid = channel->id.data();

    stanza::xep0425::moderate mod;
    if (reason)
        mod.reason(reason);

    stanza::xep0422::apply_to at(msg_id);
    at.child_el(mod);

    auto msg_s = stanza::message()
        .type("groupchat")
        .to(room_jid);
    msg_s.apply_to(at);

    account->connection.send(msg_s.build(account->context).get());

    weechat::UiPort::for_buffer(buffer)->printf_network(
        fmt::format("xmpp: moderation request sent{}", reason ? " with reason" : ""));
}

int command__retract(const void *pointer, void *data,
                     struct t_gui_buffer *buffer, int argc,
                     char **argv, char **argv_eol)
{
    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;

    (void) pointer;
    (void) data;
    (void) argv;
    (void) argv_eol;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    auto ui = weechat::UiPort::for_buffer(buffer);

    if (!ptr_channel)
    {
        ui->printf_error("xmpp: you must be in a channel to retract messages");
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        ui->printf_error("xmpp: you are not connected to server");
        return WEECHAT_RC_OK;
    }

    if (argc > 2)
    {
        ui->printf_error("xmpp: too many arguments (use /retract with no arguments)");
        return WEECHAT_RC_OK;
    }

    // Collect last 20 own messages for picker
    auto own_messages = collect_own_messages(buffer);

    if (own_messages.empty())
    {
        ui->printf_error("xmpp: no message found to retract");
        return WEECHAT_RC_OK;
    }

    using picker_t = weechat::ui::picker<std::string>;
    std::vector<picker_t::entry> entries;
    for (auto &m : own_messages)
    {
        // Resolve ID: prefer MUC stanza-id when applicable
        std::string resolved_id = resolve_msg_id(m, ptr_channel);

        std::string label = m.body.empty() ? m.id : m.body;
        entries.push_back({resolved_id, label, {}});
    }

    auto p = std::make_unique<picker_t>(
        "xmpp.picker.retract",
        "Retract message  (XEP-0424)",
        std::move(entries),
        [ptr_account, ptr_channel, buf = buffer](std::string_view selected) {
            do_retract_send(ptr_account, ptr_channel, buf, std::string(selected));
        },
        picker_t::close_cb{},
        buffer);
    if (!*p) return WEECHAT_RC_ERROR;
    p.release();
    return WEECHAT_RC_OK;
}

int command__react(const void *pointer, void *data,
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

    if (!ptr_channel)
    {
        ui->printf_error("xmpp: you must be in a channel to react to messages");
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        ui->printf_error("xmpp: you are not connected to server");
        return WEECHAT_RC_OK;
    }

    if (argc < 2)
    {
        ui->printf_error("xmpp: missing emoji reaction");
        return WEECHAT_RC_OK;
    }

    const std::string emoji = resolve_emoji_shortcode(argv_eol[1]);

    // Find the last message in buffer (not from us)
    struct t_hdata *hdata_line = weechat_hdata_get("line");
    struct t_hdata *hdata_line_data = weechat_hdata_get("line_data");
    struct t_gui_lines *own_lines = (struct t_gui_lines*)weechat_hdata_pointer(
        weechat_hdata_get("buffer"), buffer, "own_lines");
    
    if (!own_lines)
    {
        ui->printf_error("xmpp: cannot access buffer lines");
        return WEECHAT_RC_OK;
    }

    struct t_gui_line *line = (struct t_gui_line*)weechat_hdata_pointer(
        hdata_line, own_lines, "last_line");
    
    std::string target_msg_id;
    
    // Search backwards for last message with an ID (skip our own messages)
    while (line && target_msg_id.empty())
    {
        struct t_gui_line_data *line_data = (struct t_gui_line_data*)weechat_hdata_pointer(
            hdata_line, line, "data");
        
        if (line_data)
        {
            const char *tags = (const char*)weechat_hdata_string(hdata_line_data, line_data, "tags");
            
            // Look for messages with ID that aren't from us
            if (tags && std::string_view(tags).contains("id_") && !std::string_view(tags).contains("self_msg"))
            {
                // Extract the message ID and (for MUC) stanza-id from tags
                std::string msg_id;
                std::string sid;
                std::string sid_by;
                char **tag_array = weechat_string_split(tags, ",", nullptr, 0, 0, nullptr);
                if (tag_array)
                {
                    for (int i = 0; tag_array[i]; i++)
                    {
                        const std::string_view t { tag_array[i] };
                        if (t.starts_with("id_") && msg_id.empty())
                            msg_id = t.substr(3);
                        else if (t.starts_with("stanza_id_by_"))
                            sid_by = t.substr(13);
                        else if (t.starts_with("stanza_id_"))
                            sid = t.substr(10);
                    }
                    weechat_string_free_split(tag_array);
                }
                // XEP-0444 §4.2: For groupchat, MUST use the MUC-assigned stanza-id.
                if (!msg_id.empty())
                {
                    if (ptr_channel->type == weechat::channel::chat_type::MUC
                            && !sid.empty()
                            && weechat_strcasecmp(sid_by.c_str(), ptr_channel->id.c_str()) == 0)
                        target_msg_id = sid;
                    else
                        target_msg_id = msg_id;
                }
                break;
            }
        }
        
        line = (struct t_gui_line*)weechat_hdata_move(hdata_line, line, -1);
    }
    
    if (target_msg_id.empty())
    {
        ui->printf_error("xmpp: no message found to react to");
        return WEECHAT_RC_OK;
    }

    // Send reaction (XEP-0444)
    std::string msg_id = stanza::uuid(ptr_account->context);

    stanza::xep0444::reactions reactions_el(target_msg_id);
    reactions_el.reaction(emoji.c_str());

    auto msg_s = stanza::message()
        .type(ptr_channel->type == weechat::channel::chat_type::MUC
              ? "groupchat" : "chat")
        .to(ptr_channel->id)
        .id(msg_id);
    msg_s.reactions(reactions_el);
    msg_s.store();

    ptr_account->connection.send(msg_s.build(ptr_account->context).get());

        ui->printf_network(fmt::format("xmpp: reaction {} sent", emoji));

    return WEECHAT_RC_OK;
}

int command__reply(const void *pointer, void *data,
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

    if (!ptr_channel)
    {
        ui->printf_error("xmpp: you must be in a channel to reply to messages");
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        ui->printf_error("xmpp: you are not connected to server");
        return WEECHAT_RC_OK;
    }

    if (argc < 2)
    {
        // No reply text given — open a picker over the last 20 non-own messages.
        // On selection, pre-fill the input bar with /reply <id> so the user
        // can type the reply body.  We encode the resolved ID into the command
        // so the argc>=2 path can do the actual send without another hdata walk.
        auto messages = collect_buffer_messages(buffer, 500);

        using picker_t = weechat::ui::picker<std::string>;
        std::vector<picker_t::entry> entries;
        for (auto &m : messages)
        {
            if (m.from_self || m.id.empty()) continue;

            // Resolve ID: prefer MUC stanza-id when applicable (XEP-0461 §4.1)
            std::string resolved_id = resolve_msg_id(m, ptr_channel);

            std::string label = m.body.empty() ? m.id : m.body;
            std::string sublabel = m.nick.empty() ? "" : "from: " + m.nick;
            entries.push_back({resolved_id, label, sublabel});
            if (static_cast<int>(entries.size()) >= 20)
                break;
        }

        if (entries.empty())
        {
        ui->printf_error("xmpp: no message found to reply to");
            return WEECHAT_RC_OK;
        }

        auto p = std::make_unique<picker_t>(
            "xmpp.picker.reply",
            "Reply to message  (XEP-0461)",
            std::move(entries),
            [buf = buffer](std::string_view selected) {
                // Pre-fill input bar: user types the reply body after this
                std::string input = fmt::format("/reply-to {} ", selected);
                weechat_buffer_set(buf, "input", input.c_str());
                weechat_buffer_set(buf, "input_pos",
                    std::to_string(input.size()).c_str());
                weechat_buffer_set(buf, "display", "1");
            },
            picker_t::close_cb{},
            buffer);
        if (!*p) return WEECHAT_RC_ERROR;
        p.release();
        return WEECHAT_RC_OK;
    }

    const char *reply_text = argv_eol[1];

    // Find the last non-self message in the buffer using the same hdata walker
    // used by the picker path — this correctly uses the self_msg tag rather than
    // a fragile nick-vs-JID comparison which breaks for PMs (full JID vs bare JID).
    auto messages = collect_buffer_messages(buffer, 500);
    const msg_entry *target = nullptr;
    for (auto &m : messages)
    {
        if (!m.from_self && !m.id.empty())
        {
            target = &m;
            break;
        }
    }

    if (!target)
    {
        ui->printf_error("xmpp: no message found to reply to");
        return WEECHAT_RC_OK;
    }

    const std::string target_id_str = resolve_msg_id(*target, ptr_channel);
    const std::string reply_to_jid =
        resolve_reply_to_jid(ptr_channel, target->nick);

    send_reply_message(ptr_account, ptr_channel, buffer,
                       target_id_str, reply_to_jid, reply_text);

    ui->printf_network("xmpp: reply sent");

    return WEECHAT_RC_OK;
}

// /reply-to <id> <message> — explicit-ID reply generated by the picker.
int command__reply_to(const void *pointer, void *data,
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

    if (!ptr_channel)
    {
        ui->printf_error("xmpp: you must be in a channel to reply to messages");
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        ui->printf_error("xmpp: you are not connected to server");
        return WEECHAT_RC_OK;
    }

    if (argc < 3)
    {
        ui->printf_error("xmpp: usage: /reply-to <message-id> <text>");
        return WEECHAT_RC_OK;
    }

    const char *target_id  = argv[1];
    const char *reply_text = argv_eol[2];

    std::string reply_to_jid = ptr_channel->name;
    for (auto &m : collect_buffer_messages(buffer, 500))
    {
        if (resolve_msg_id(m, ptr_channel) == target_id)
        {
            reply_to_jid = resolve_reply_to_jid(ptr_channel, m.nick);
            break;
        }
    }

    send_reply_message(ptr_account, ptr_channel, buffer,
                       target_id, reply_to_jid, reply_text);

    ui->printf_network("xmpp: reply sent");
    return WEECHAT_RC_OK;
}

int command__moderate(const void *pointer, void *data,
                      struct t_gui_buffer *buffer, int argc,
                      char **argv, char **argv_eol)
{
    weechat::account *ptr_account = nullptr;
    weechat::channel *ptr_channel = nullptr;

    (void) pointer;
    (void) data;
    (void) argv;
    (void) argv_eol;

    buffer__get_account_and_channel(buffer, &ptr_account, &ptr_channel);

    if (!ptr_account)
        return WEECHAT_RC_ERROR;

    auto ui = weechat::UiPort::for_buffer(buffer);

    if (!ptr_channel)
    {
        ui->printf_error("xmpp: you must be in a MUC channel to moderate messages");
        return WEECHAT_RC_OK;
    }

    // XEP-0425 is only for MUC moderation
    if (ptr_channel->type != weechat::channel::chat_type::MUC)
    {
        ui->printf_error("xmpp: message moderation is only available in MUC rooms");
        return WEECHAT_RC_OK;
    }

    if (!ptr_account->connected())
    {
        ui->printf_error("xmpp: you are not connected to server");
        return WEECHAT_RC_OK;
    }

    // Optional: reason for moderation (argc > 1)
    const char *reason = (argc > 1) ? argv_eol[1] : nullptr;

    // Open picker over last 20 messages (any sender, skip retracted)
    auto messages = collect_buffer_messages(buffer, 500);

    using picker_t = weechat::ui::picker<std::string>;
    std::vector<picker_t::entry> entries;
    for (auto &m : messages)
    {
        if (m.retracted || m.id.empty()) continue;
        std::string label = m.body.empty() ? m.id : m.body;
        std::string sublabel = m.nick.empty() ? "" : "from: " + m.nick;
        entries.push_back({m.id, label, sublabel});
        if (static_cast<int>(entries.size()) >= 20)
            break;
    }

    if (entries.empty())
    {
        ui->printf_error("xmpp: no message found to moderate");
        return WEECHAT_RC_OK;
    }

    std::string reason_str = reason ? reason : "";
    auto p = std::make_unique<picker_t>(
        "xmpp.picker.moderate",
        "Moderate message  (XEP-0425)",
        std::move(entries),
        [ptr_account, ptr_channel, buf = buffer, reason_str](std::string_view selected) {
            do_moderate_send(ptr_account, ptr_channel, buf, std::string(selected),
                             reason_str.empty() ? nullptr : reason_str.c_str());
        },
        picker_t::close_cb{},
        buffer);
    if (!*p) return WEECHAT_RC_ERROR;
    p.release();
    return WEECHAT_RC_OK;
}

