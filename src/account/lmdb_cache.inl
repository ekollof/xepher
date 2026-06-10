void weechat::account::mam_cache_init()
{
    try {
        {
            std::shared_ptr<char> eval_path(
                weechat_string_eval_expression(
                    fmt::format("${{weechat_data_dir}}/xmpp/mam_{}.db", name.data()).data(),
                    nullptr, nullptr, nullptr),
                &free);
            mam_db_path = eval_path.get();
        }
        
        std::filesystem::create_directories(
            std::filesystem::path(mam_db_path.data()).parent_path());

        mam_db_env = lmdb::env::create();
        mam_db_env.set_max_dbs(10);
        mam_db_env.set_mapsize((size_t)1048576 * 1000); // 1000MB
        mam_db_env.open(mam_db_path.data(), MDB_NOSUBDIR, 0600);

        lmdb::txn parentTransaction{nullptr};
        lmdb::txn transaction = lmdb::txn::begin(mam_db_env, parentTransaction);

        mam_dbi.messages = lmdb::dbi::open(transaction, "messages", MDB_CREATE);
        mam_dbi.timestamps = lmdb::dbi::open(transaction, "timestamps", MDB_CREATE);
        mam_dbi.capabilities = lmdb::dbi::open(transaction, "capabilities", MDB_CREATE);
        mam_dbi.retractions = lmdb::dbi::open(transaction, "retractions", MDB_CREATE);
        mam_dbi.cursors = lmdb::dbi::open(transaction, "cursors", MDB_CREATE);
        mam_dbi.omemo_plaintext = lmdb::dbi::open(transaction, "omemo_plaintext", MDB_CREATE);
        mam_dbi.esfs_downloads  = lmdb::dbi::open(transaction, "esfs_downloads",  MDB_CREATE);
        mam_dbi.image_previews  = lmdb::dbi::open(transaction, "image_previews",  MDB_CREATE);
        mam_dbi.og_previews     = lmdb::dbi::open(transaction, "og_previews",     MDB_CREATE);

        transaction.commit();
        
        // Load capability cache from database
        caps_cache_load();
        feed_open_sync_from_cache();
    } catch (const lmdb::error& ex) {
        weechat::UiPort::for_buffer(nullptr)->printf_error(
            fmt::format("xmpp: MAM cache init failed - {}", ex.what()));
    }
}

void weechat::account::mam_cache_cleanup()
{
    try {
        if (mam_db_env)
        {
            // Don't call mdb_dbi_close() - it's not thread-safe and handles
            // are automatically cleaned up when the environment is closed
            mam_db_env.close();
            mam_db_env = nullptr;
        }
    } catch (const lmdb::error& ex) {
        weechat::UiPort::for_buffer(nullptr)->printf_error(
            fmt::format("xmpp: MAM cache cleanup failed - {}", ex.what()));
    }
}

void weechat::account::mam_cache_message(std::string_view channel_jid,
                                         std::string_view message_id,
                                         std::string_view from,
                                         time_t timestamp,
                                         std::string_view body)
{
    if (!mam_db_env) return;
    
    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, 0);
        
        // Key: channel_jid:timestamp:message_id
        std::string key = fmt::format("{}:{:020d}:{}", channel_jid, timestamp, message_id);
        
        // Value: from|timestamp|body
        std::string value = fmt::format("{}|{}|{}", from, timestamp, body);
        
        MDB_val k = {key.size(), (void*)key.data()};
        MDB_val v = {value.size(), (void*)value.data()};
        
        mdb_put(txn.handle(), mam_dbi.messages.handle(), &k, &v, 0);
        
        txn.commit();
    } catch (const lmdb::error& ex) {
        // Silently ignore cache write errors
    }
}

void weechat::account::mam_cache_retract_message(std::string_view channel_jid, std::string_view message_id)
{
    if (!mam_db_env || message_id.empty()) return;
    
    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, 0);
        
        // Key: channel_jid:message_id
        std::string key = fmt::format("{}:{}", channel_jid, message_id);
        
        // Value: just a marker (timestamp when retracted)
        time_t now = time(nullptr);
        
        MDB_val k = {key.size(), (void*)key.data()};
        MDB_val v = {sizeof(now), &now};
        
        mdb_put(txn.handle(), mam_dbi.retractions.handle(), &k, &v, 0);
        
        txn.commit();
    } catch (const lmdb::error& ex) {
        // Silently ignore cache write errors
    }
}

bool weechat::account::mam_cache_is_retracted(std::string_view channel_jid, std::string_view message_id)
{
    if (!mam_db_env || message_id.empty()) return false;
    
    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, MDB_RDONLY);
        
        // Key: channel_jid:message_id
        std::string key = fmt::format("{}:{}", channel_jid, message_id);
        
        MDB_val k = {key.size(), (void*)key.data()};
        MDB_val v;
        
        int rc = mdb_get(txn.handle(), mam_dbi.retractions.handle(), &k, &v);
        txn.abort();
        
        return (rc == 0);  // Message is retracted if key exists
    } catch (const lmdb::error& ex) {
        return false;
    }
}

void weechat::account::mam_cache_load_messages(std::string_view channel_jid, struct t_gui_buffer *buffer)
{
    if (!mam_db_env || !buffer) return;
    
    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, MDB_RDONLY);
        
        MDB_cursor *cursor_raw = nullptr;
        mdb_cursor_open(txn.handle(), mam_dbi.messages.handle(), &cursor_raw);
        std::unique_ptr<MDB_cursor, decltype(&mdb_cursor_close)> cursor(cursor_raw, &mdb_cursor_close);

        // Preload retractions for this channel (single cursor scan instead of
        // per-message LMDB transactions — avoids hundreds of txn open/close cycles).
        const std::string retract_prefix = fmt::format("{}:", channel_jid);
        MDB_val rk = {retract_prefix.size(), (void*)retract_prefix.data()};
        MDB_val rv;
        MDB_cursor *rcursor_raw = nullptr;
        mdb_cursor_open(txn.handle(), mam_dbi.retractions.handle(), &rcursor_raw);
        std::unique_ptr<MDB_cursor, decltype(&mdb_cursor_close)> rcursor(rcursor_raw, &mdb_cursor_close);
        std::unordered_set<std::string> retracted_ids;
        int rrc = mdb_cursor_get(rcursor.get(), &rk, &rv, MDB_SET_RANGE);
        while (rrc == 0)
        {
            std::string_view rkv(static_cast<const char*>(rk.mv_data), rk.mv_size);
            if (!rkv.starts_with(retract_prefix))
                break;
            size_t colon = rkv.find(':');
            if (colon != std::string_view::npos)
                retracted_ids.emplace(rkv.substr(colon + 1));
            rrc = mdb_cursor_get(rcursor.get(), &rk, &rv, MDB_NEXT);
        }
        rcursor.reset();

        // Start with channel prefix
        const std::string prefix = fmt::format("{}:", channel_jid);
        MDB_val key = {prefix.size(), (void*)prefix.data()};
        MDB_val value;
        
        int count = 0;
        int rc = mdb_cursor_get(cursor.get(), &key, &value, MDB_SET_RANGE);
        
        while (rc == 0 && count < 100)  // Limit to last 100 cached messages
        {
            std::string key_str((char*)key.mv_data, key.mv_size);
            
            // Check if key still belongs to our channel
            if (key_str.substr(0, prefix.size()) != prefix)
                break;
            
            // Parse key: channel_jid:timestamp:message_id
            size_t colon1 = key_str.find(':', prefix.size());
            size_t colon2 = key_str.find(':', colon1 + 1);
            
            if (colon2 != std::string::npos)
            {
                std::string message_id = key_str.substr(colon2 + 1);
                // Tag with id_<message-id> so MAM dedup can suppress later replays.
                bool is_retracted = !message_id.empty() && retracted_ids.contains(message_id);
                std::string tags = is_retracted
                    ? "xmpp_cached,xmpp_retracted,no_highlight"
                    : "xmpp_cached,no_highlight";
                if (!message_id.empty())
                    tags += ",id_" + message_id;

                // Parse value: from|timestamp|body
                std::string value_str((char*)value.mv_data, value.mv_size);
                size_t pos1 = value_str.find('|');
                size_t pos2 = value_str.find('|', pos1 + 1);
                
                if (pos1 != std::string::npos && pos2 != std::string::npos)
                {
                    std::string from = value_str.substr(0, pos1);
                    std::string timestamp_str = value_str.substr(pos1 + 1, pos2 - pos1 - 1);
                    std::string body = value_str.substr(pos2 + 1);
                    const auto ts = parse_int64(timestamp_str);
                    if (!ts)
                        continue;
                    time_t timestamp = static_cast<time_t>(*ts);

                    // Display cached message with gray prefix
                    if (is_retracted)
                    {
                        weechat::UiPort::for_buffer(buffer)->printf_date_tags(timestamp, tags.c_str(),
                            fmt::format("{}{}\t{}[Message deleted]{}",
                                        weechat_color("darkgray"), from,
                                        weechat_color("darkgray"),
                                        weechat_color("resetcolor")));
                    }
                    else
                    {
                        weechat::UiPort::for_buffer(buffer)->printf_date_tags(timestamp, tags.c_str(),
                            fmt::format("{}{}\t{}",
                                        weechat_color("darkgray"), from, body));
                    }
                    count++;
                }
            }
            
            rc = mdb_cursor_get(cursor.get(), &key, &value, MDB_NEXT);
        }
        
        cursor.reset();
        txn.abort();
        
        // Only print the summary count — skip individual message replay.
        // The subsequent MAM fetch will populate the buffer with fresh messages;
        // replaying up to 100 cached messages via weechat_printf_date_tags per
        // channel during the join flood triggers expensive GUI redraws.
        if (count > 0)
        {
            weechat::UiPort::for_buffer(buffer)->printf_network(
                fmt::format("--- {} cached messages available (MAM fetch will refresh)", count));
        }
    } catch (const lmdb::error& ex) {
        // Silently ignore read errors
    } catch (const std::exception& ex) {
        // Silently ignore parsing errors
    }
}

void weechat::account::mam_cache_clear_messages(std::string_view channel_jid)
{
    if (!mam_db_env) return;
    
    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, 0);
        
        MDB_cursor *cursor_raw = nullptr;
        mdb_cursor_open(txn.handle(), mam_dbi.messages.handle(), &cursor_raw);
        std::unique_ptr<MDB_cursor, decltype(&mdb_cursor_close)> cursor(cursor_raw, &mdb_cursor_close);

        // Start with channel prefix
        const std::string prefix = fmt::format("{}:", channel_jid);
        MDB_val key = {prefix.size(), (void*)prefix.data()};
        MDB_val value;
        
        int rc = mdb_cursor_get(cursor.get(), &key, &value, MDB_SET_RANGE);
        
        while (rc == 0)
        {
            std::string key_str((char*)key.mv_data, key.mv_size);
            
            // Check if key still belongs to our channel
            if (key_str.substr(0, prefix.size()) != prefix)
                break;
            
            // Delete this entry
            mdb_cursor_del(cursor.get(), 0);
            
            // Move to next
            rc = mdb_cursor_get(cursor.get(), &key, &value, MDB_NEXT);
        }
        
        cursor.reset();
        txn.commit();
    } catch (const lmdb::error& ex) {
        // Silently ignore errors
    }
}

void weechat::account::pm_open_register(std::string_view pm_jid)
{
    if (!mam_db_env || pm_jid.empty()) return;

    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, 0);

        std::string key = fmt::format("pm_open:{}", pm_jid);
        std::string value = "1";
        MDB_val k = {key.size(), (void*)key.data()};
        MDB_val v = {value.size(), (void*)value.data()};
        mdb_put(txn.handle(), mam_dbi.cursors.handle(), &k, &v, 0);

        txn.commit();
    } catch (const lmdb::error&) {}
}

void weechat::account::pm_open_unregister(std::string_view pm_jid)
{
    if (!mam_db_env || pm_jid.empty()) return;

    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, 0);

        std::string key = fmt::format("pm_open:{}", pm_jid);
        MDB_val k = {key.size(), (void*)key.data()};
        mdb_del(txn.handle(), mam_dbi.cursors.handle(), &k, nullptr);

        txn.commit();
    } catch (const lmdb::error&) {}
}

std::vector<std::string> weechat::account::pm_open_list()
{
    std::vector<std::string> result;
    if (!mam_db_env) return result;

    static constexpr std::string_view prefix = "pm_open:";
    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, MDB_RDONLY);

        MDB_cursor *cursor = nullptr;
        if (mdb_cursor_open(txn.handle(), mam_dbi.cursors.handle(), &cursor) != 0)
        {
            txn.abort();
            return result;
        }

        MDB_val k = {prefix.size(), (void*)prefix.data()};
        MDB_val v;
        int rc = mdb_cursor_get(cursor, &k, &v, MDB_SET_RANGE);
        while (rc == 0)
        {
            std::string_view kv(static_cast<const char*>(k.mv_data), k.mv_size);
            if (!kv.starts_with(prefix))
                break;
            result.emplace_back(kv.substr(prefix.size()));
            rc = mdb_cursor_get(cursor, &k, &v, MDB_NEXT);
        }

        mdb_cursor_close(cursor);
        txn.abort();
    } catch (const lmdb::error&) {}

    return result;
}

time_t weechat::account::mam_cache_get_last_timestamp(std::string_view channel_jid)
{
    if (!mam_db_env) return 0;
    
    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, MDB_RDONLY);
        
        MDB_val key = {channel_jid.size(), (void*)channel_jid.data()};
        MDB_val value;
        
        if (mdb_get(txn.handle(), mam_dbi.timestamps.handle(), &key, &value) == 0)
        {
            time_t ts = 0;
            if (value.mv_size == sizeof(time_t))
            {
                memcpy(&ts, value.mv_data, sizeof(time_t));
            }
            txn.abort();
            return ts;
        }
        
        txn.abort();
    } catch (const lmdb::error& ex) {
        // Silently ignore read errors
    }
    
    return 0;
}

void weechat::account::mam_cache_set_last_timestamp(std::string_view channel_jid, time_t timestamp)
{
    if (!mam_db_env) return;
    
    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, 0);
        
        MDB_val key = {channel_jid.size(), (void*)channel_jid.data()};
        MDB_val value = {sizeof(timestamp), &timestamp};
        
        mdb_put(txn.handle(), mam_dbi.timestamps.handle(), &key, &value, 0);
        
        txn.commit();
    } catch (const lmdb::error& ex) {
        // Silently ignore write errors
    }
}

std::string weechat::account::mam_cursor_get(std::string_view key)
{
    if (!mam_db_env) return {};

    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, MDB_RDONLY);

        MDB_val k = {key.size(), (void*)key.data()};
        MDB_val v;

        if (mdb_get(txn.handle(), mam_dbi.cursors.handle(), &k, &v) == 0)
        {
            std::string result(static_cast<const char*>(v.mv_data), v.mv_size);
            txn.abort();
            return result;
        }

        txn.abort();
    } catch (const lmdb::error& ex) {
        // Silently ignore read errors
    }

    return {};
}

void weechat::account::mam_cursor_set(std::string_view key, std::string_view cursor_id)
{
    if (!mam_db_env || cursor_id.empty()) return;

    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, 0);

        MDB_val k = {key.size(), (void*)key.data()};
        MDB_val v = {cursor_id.size(), (void*)cursor_id.data()};

        mdb_put(txn.handle(), mam_dbi.cursors.handle(), &k, &v, 0);

        txn.commit();
    } catch (const lmdb::error& ex) {
        // Silently ignore write errors
    }
}

void weechat::account::mam_cursor_clear(std::string_view key)
{
    if (!mam_db_env) return;

    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, 0);

        MDB_val k = {key.size(), (void*)key.data()};
        mdb_del(txn.handle(), mam_dbi.cursors.handle(), &k, nullptr);

        txn.commit();
    } catch (const lmdb::error& ex) {
        // Silently ignore delete errors (key may not exist)
    }
}

bool weechat::account::feed_item_seen(std::string_view feed_key, std::string_view item_id)
{
    if (!mam_db_env || item_id.empty()) return false;

    std::string key = fmt::format("feed_seen:{}:{}", feed_key, item_id);
    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, MDB_RDONLY);
        MDB_val k = {key.size(), (void*)key.data()};
        MDB_val v;
        bool found = (mdb_get(txn.handle(), mam_dbi.cursors.handle(), &k, &v) == 0);
        txn.abort();
        return found;
    } catch (const lmdb::error&) {
        return false;
    }
}

void weechat::account::feed_item_mark_seen(std::string_view feed_key, std::string_view item_id)
{
    if (!mam_db_env || item_id.empty()) return;

    std::string key = fmt::format("feed_seen:{}:{}", feed_key, item_id);
    static const char *val_str = "1";
    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, 0);
        MDB_val k = {key.size(), (void*)key.data()};
        MDB_val v = {1, (void*)val_str};
        mdb_put(txn.handle(), mam_dbi.cursors.handle(), &k, &v, 0);
        txn.commit();
    } catch (const lmdb::error&) {
        // Silently ignore write errors
    }
}

void weechat::account::feed_open_register(std::string_view feed_key)
{
    if (feed_key.empty()) return;

    feed_open_keys_.insert(std::string(feed_key));

    if (!mam_db_env) return;

    std::string key = fmt::format("feed_open:{}", feed_key);
    static const char *val_str = "1";
    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, 0);
        MDB_val k = {key.size(), (void*)key.data()};
        MDB_val v = {1, (void*)val_str};
        mdb_put(txn.handle(), mam_dbi.cursors.handle(), &k, &v, 0);
        txn.commit();
    } catch (const lmdb::error&) {
        // Silently ignore write errors
    }
}

void weechat::account::feed_open_unregister(std::string_view feed_key)
{
    if (feed_key.empty()) return;

    feed_open_keys_.erase(std::string(feed_key));

    if (!mam_db_env) return;

    std::string key = fmt::format("feed_open:{}", feed_key);
    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, 0);
        MDB_val k = {key.size(), (void*)key.data()};
        mdb_del(txn.handle(), mam_dbi.cursors.handle(), &k, nullptr);
        txn.commit();
    } catch (const lmdb::error&) {
        // Silently ignore delete errors
    }
}

bool weechat::account::feed_is_open(const std::string_view feed_key) const
{
    return !feed_key.empty() && feed_open_keys_.contains(std::string(feed_key));
}

void weechat::account::feed_open_sync_from_cache()
{
    feed_open_keys_.clear();
    for (const auto &feed_key : feed_open_list())
        feed_open_keys_.insert(feed_key);
}

void weechat::account::feed_dismiss(std::string_view feed_key)
{
    if (feed_key.empty()) return;

    feed_open_unregister(feed_key);

    const auto slash = feed_key.find('/');
    if (slash == std::string_view::npos) return;

    const std::string service{feed_key.substr(0, slash)};
    const std::string node{feed_key.substr(slash + 1)};

    if (auto def_it = pubsub_mam_deferred_feeds.find(service);
        def_it != pubsub_mam_deferred_feeds.end())
    {
        auto &deferred = def_it->second;
        std::erase(deferred, std::string(feed_key));
        if (deferred.empty())
            pubsub_mam_deferred_feeds.erase(def_it);
    }

    for (auto it = pubsub_mam_queries.begin(); it != pubsub_mam_queries.end();)
    {
        const auto &[_, pq] = *it;
        if (pq.service == service && pq.node == node)
            it = pubsub_mam_queries.erase(it);
        else
            ++it;
    }

    for (auto it = pubsub_fetch_ids.begin(); it != pubsub_fetch_ids.end();)
    {
        const auto &[_, fi] = *it;
        if (fi.service == service && fi.node == node)
            it = pubsub_fetch_ids.erase(it);
        else
            ++it;
    }
}

std::vector<std::string> weechat::account::feed_open_list()
{
    std::vector<std::string> result;
    if (!mam_db_env) return result;

    static constexpr std::string_view prefix = "feed_open:";
    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, MDB_RDONLY);

        MDB_cursor *cursor = nullptr;
        if (mdb_cursor_open(txn.handle(), mam_dbi.cursors.handle(), &cursor) != 0)
        {
            txn.abort();
            return result;
        }

        // Position cursor at the first key >= "feed_open:"
        MDB_val k = {prefix.size(), (void*)prefix.data()};
        MDB_val v;
        int rc = mdb_cursor_get(cursor, &k, &v, MDB_SET_RANGE);
        while (rc == 0)
        {
            std::string_view kv(static_cast<const char*>(k.mv_data), k.mv_size);
            if (!kv.starts_with(prefix))
                break;
            result.emplace_back(kv.substr(prefix.size()));
            rc = mdb_cursor_get(cursor, &k, &v, MDB_NEXT);
        }

        mdb_cursor_close(cursor);
        txn.abort();
    } catch (const lmdb::error&) {
        // Return whatever we got
    }

    return result;
}

void weechat::account::mam_cache_store_omemo_plaintext(std::string_view channel_jid,
                                                       std::string_view msg_id,
                                                       std::string_view body)
{
    if (!mam_db_env || channel_jid.empty() || msg_id.empty() || body.empty()) return;

    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, 0);

        std::string key = fmt::format("{}:{}", channel_jid, msg_id);
        MDB_val k = {key.size(), (void*)key.data()};
        MDB_val v = {body.size(), (void*)body.data()};

        mdb_put(txn.handle(), mam_dbi.omemo_plaintext.handle(), &k, &v, 0);
        txn.commit();
    } catch (const lmdb::error&) {
        // Silently ignore write errors
    }
}

std::expected<std::string, std::string> weechat::account::mam_cache_lookup_omemo_plaintext(
    std::string_view channel_jid, std::string_view msg_id)
{
    if (!mam_db_env || channel_jid.empty() || msg_id.empty()) return std::unexpected("invalid args");

    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, MDB_RDONLY);

        std::string key = fmt::format("{}:{}", channel_jid, msg_id);
        MDB_val k = {key.size(), (void*)key.data()};
        MDB_val v;

        if (mdb_get(txn.handle(), mam_dbi.omemo_plaintext.handle(), &k, &v) == 0)
        {
            std::string body(static_cast<const char*>(v.mv_data), v.mv_size);
            txn.abort();
            return body;
        }

        txn.abort();
    } catch (const lmdb::error&) {
        // Silently ignore read errors
    }

    return std::unexpected("not found or db error");
}

void weechat::account::mam_cache_store_esfs_download(std::string_view channel_jid,
                                                      std::string_view stable_id,
                                                      std::string_view saved_path)
{
    if (!mam_db_env || channel_jid.empty() || stable_id.empty() || saved_path.empty()) return;

    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, 0);

        std::string key = fmt::format("{}:{}", channel_jid, stable_id);
        MDB_val k = {key.size(), (void*)key.data()};
        MDB_val v = {saved_path.size(), (void*)saved_path.data()};

        mdb_put(txn.handle(), mam_dbi.esfs_downloads.handle(), &k, &v, 0);
        txn.commit();
    } catch (const lmdb::error&) {
        // Silently ignore write errors
    }
}

std::expected<std::string, std::string> weechat::account::mam_cache_lookup_esfs_download(
    std::string_view channel_jid, std::string_view stable_id)
{
    if (!mam_db_env || channel_jid.empty() || stable_id.empty()) return std::unexpected("invalid args");

    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, MDB_RDONLY);

        std::string key = fmt::format("{}:{}", channel_jid, stable_id);
        MDB_val k = {key.size(), (void*)key.data()};
        MDB_val v;

        if (mdb_get(txn.handle(), mam_dbi.esfs_downloads.handle(), &k, &v) == 0)
        {
            std::string path(static_cast<const char*>(v.mv_data), v.mv_size);
            txn.abort();
            return path;
        }

        txn.abort();
    } catch (const lmdb::error&) {
        // Silently ignore read errors
    }

    return std::unexpected("not found or db error");
}

void weechat::account::mam_cache_store_image_preview(std::string_view channel_jid,
                                                     std::string_view stable_id,
                                                     std::string_view local_path)
{
    if (!mam_db_env || channel_jid.empty() || stable_id.empty() || local_path.empty()) return;

    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, 0);

        std::string key = fmt::format("{}:{}", channel_jid, stable_id);
        MDB_val k = {key.size(), (void*)key.data()};
        MDB_val v = {local_path.size(), (void*)local_path.data()};

        mdb_put(txn.handle(), mam_dbi.image_previews.handle(), &k, &v, 0);
        txn.commit();
    } catch (const lmdb::error&) {
        // Silently ignore write errors
    }
}

std::expected<std::string, std::string> weechat::account::mam_cache_lookup_image_preview(
    std::string_view channel_jid, std::string_view stable_id)
{
    if (!mam_db_env || channel_jid.empty() || stable_id.empty()) return std::unexpected("invalid args");

    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, MDB_RDONLY);

        std::string key = fmt::format("{}:{}", channel_jid, stable_id);
        MDB_val k = {key.size(), (void*)key.data()};
        MDB_val v;

        if (mdb_get(txn.handle(), mam_dbi.image_previews.handle(), &k, &v) == 0)
        {
            std::string path(static_cast<const char*>(v.mv_data), v.mv_size);
            txn.abort();
            if (std::filesystem::is_regular_file(path))
                return path;
            return std::unexpected("cached path missing");
        }

        txn.abort();
    } catch (const lmdb::error&) {
        // Silently ignore read errors
    }

    return std::unexpected("not found or db error");
}

void weechat::account::send_bookmarks()
{
    // XEP-0402: PEP Native Bookmarks.
    // Build a PEP publish IQ for the urn:xmpp:bookmarks:1 node.
    //
    // NOTE: We intentionally omit <publish-options> even though XEP-0402 §5
    // recommends it.  Sending publish-options with pubsub#access_model=whitelist
    // causes some servers (e.g. older Ejabberd) to close the stream with
    // undefined-condition when the node already exists with a different
    // access model.  PEP nodes are inherently owner-only, so the access model
    // precondition adds no security and only risks breaking compatibility.
    //
    // If there are no bookmarks, skip the IQ entirely — XEP-0060 §7.1 requires
    // at least one <item> in a publish request; an empty <publish> is invalid.
    if (bookmarks.empty())
        return;

    // <iq type='set' id='...'>
    //   <pubsub xmlns='http://jabber.org/protocol/pubsub'>
    //     <publish node='urn:xmpp:bookmarks:1'>
    //       <item id='roomjid@conference.example.org'>
    //         <conference xmlns='urn:xmpp:bookmarks:1' name='...' autojoin='true'>
    //           <nick>...</nick>
    //         </conference>
    //       </item>
    //     </publish>
    //   </pubsub>
    // </iq>

    // conference spec — <conference xmlns='urn:xmpp:bookmarks:1' ...>
    struct conference_spec : stanza::spec {
        conference_spec(const weechat::account::bookmark_item &bm) : spec("conference") {
            attr("xmlns", "urn:xmpp:bookmarks:1");
            if (!bm.name.empty())  attr("name", bm.name);
            if (bm.autojoin)       attr("autojoin", "true");
            if (!bm.nick.empty()) {
                struct nick_spec : stanza::spec {
                    nick_spec(std::string_view s) : spec("nick") { text(s); }
                } nick_ch(bm.nick);
                child(nick_ch);
            }
            // XEP-0492: persist per-chat notification preference in <extensions>
            if (!bm.notify_setting.empty()) {
                struct setting_spec : stanza::spec {
                    setting_spec(std::string_view name) : spec(name) {}
                };
                struct notify_spec : stanza::spec {
                    notify_spec(std::string_view setting) : spec("notify") {
                        attr("xmlns", "urn:xmpp:notification-settings:1");
                        setting_spec s(setting);
                        child(s);
                    }
                };
                struct extensions_spec : stanza::spec {
                    extensions_spec(std::string_view setting) : spec("extensions") {
                        notify_spec n(setting);
                        child(n);
                    }
                } ext_ch(bm.notify_setting);
                child(ext_ch);
            }
        }
    };

    auto publish_el = stanza::xep0060::publish("urn:xmpp:bookmarks:1");
    for (const auto& [_, bookmark] : bookmarks)
    {
        conference_spec conf_ch(bookmark);
        publish_el.item(stanza::xep0060::item().id(bookmark.jid).payload(conf_ch));
    }

    auto iq_s = stanza::iq().type("set").id(stanza::uuid(context));
    static_cast<stanza::xep0060::iq&>(iq_s)
        .pubsub(stanza::xep0060::pubsub().publish(publish_el));
    connection.send(iq_s.build(context).get());
}

// XEP-0402 §3.5: Remove a bookmark via PubSub <retract> with notify='true'.
// This sends a targeted retract for the single item instead of re-publishing
// the remaining bookmarks, which is the correct spec-mandated behaviour.
void weechat::account::retract_bookmark(std::string_view jid_sv)
{
    auto retract_el = stanza::xep0060::retract("urn:xmpp:bookmarks:1")
        .notify(true)
        .item(stanza::xep0060::item().id(jid_sv));
    auto iq_s = stanza::iq().type("set").id(stanza::uuid(context));
    static_cast<stanza::xep0060::iq&>(iq_s)
        .pubsub(stanza::xep0060::pubsub().retract(retract_el));
    connection.send(iq_s.build(context).get());
}

// Capability cache implementation (XEP-0115)

void weechat::account::caps_cache_load()
{
    if (!mam_db_env) return;
    
    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, MDB_RDONLY);
        
        MDB_cursor *cursor_raw = nullptr;
        mdb_cursor_open(txn.handle(), mam_dbi.capabilities.handle(), &cursor_raw);
        std::unique_ptr<MDB_cursor, decltype(&mdb_cursor_close)> cursor(cursor_raw, &mdb_cursor_close);

        MDB_val key, value;
        int count = 0;
        
        while (mdb_cursor_get(cursor.get(), &key, &value, MDB_NEXT) == 0)
        {
            std::string ver_hash((char*)key.mv_data, key.mv_size);
            std::string features_str((char*)value.mv_data, value.mv_size);
            
            // Parse features (stored as comma-separated)
            std::vector<std::string> features;
            for (auto r : std::views::split(features_str, ','))
                features.emplace_back(r.begin(), r.end());
            
            caps_cache[ver_hash] = features;
            count++;
        }
        
        cursor.reset();
        txn.abort();
        
        if (count > 0)
        {
            weechat::UiPort::for_buffer(buffer)->printf_network(
                fmt::format("Loaded {} capability entries from cache", count));
        }
    } catch (const lmdb::error& ex) {
        // Silently ignore read errors
    }
}

void weechat::account::caps_cache_save(std::string_view verification_hash, 
                                        const std::vector<std::string>& features)
{
    if (!mam_db_env) return;
    
    // Store in memory cache
    caps_cache[std::string(verification_hash)] = features;
    
    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, 0);
        
        // Join features with commas
        std::string features_str = fmt::format("{}", fmt::join(features, ","));
        
        MDB_val key = {verification_hash.size(), (void*)verification_hash.data()};
        MDB_val value = {features_str.size(), (void*)features_str.data()};
        
        mdb_put(txn.handle(), mam_dbi.capabilities.handle(), &key, &value, 0);
        
        txn.commit();
    } catch (const lmdb::error& ex) {
        // Silently ignore write errors
    }
}

bool weechat::account::caps_cache_get(std::string_view verification_hash,
                                       std::vector<std::string>& features)
{
    if (auto it = caps_cache.find(std::string(verification_hash)); it != caps_cache.end())
    {
        auto& [_, fs] = *it;
        features = fs;
        return true;
    }
    return false;
}

void weechat::account::peer_features_update(std::string_view jid,
                                            const std::vector<std::string>& features)
{
    if (jid.empty() || features.empty())
        return;

    std::string bare(jid);
    if (const auto slash = bare.find('/'); slash != std::string::npos)
        bare.resize(slash);

    auto &merged = peer_features[bare];
    for (const auto &feature : features)
        merged.insert(feature);
}

bool weechat::account::peer_supports_feature(std::string_view jid,
                                             std::string_view feature) const
{
    if (jid.empty() || feature.empty())
        return false;

    std::string bare(jid);
    if (const auto slash = bare.find('/'); slash != std::string::npos)
        bare.resize(slash);

    auto it = peer_features.find(bare);
    if (it == peer_features.end())
        return false;

    const auto& [_, features] = *it;
    return features.contains(std::string(feature));
}

bool weechat::account::peer_has_legacy_axolotl_only(std::string_view jid) const
{
    static const std::string legacy_axolotl = "eu.siacs.conversations.axolotl";
    return peer_supports_feature(jid, legacy_axolotl);
}

// OG preview cache (XEP-0511) — keyed by URL, value is TAB-delimited title\tdesc\turl\timage
// TAB is used as delimiter because OG fields never legitimately contain raw TAB.

void weechat::account::og_cache_store(std::string_view url,
                                       const og_preview& preview)
{
    if (!mam_db_env || url.empty()) return;
    try {
        // Encode: replace any literal \t in field values with a space (safe lossy).
        auto sanitize = [](std::string_view s) {
            std::string out(s);
            std::ranges::for_each(out, [](char& c){ if (c == '\t') c = ' '; });
            return out;
        };
        std::string value = fmt::format("{}\t{}\t{}\t{}",
            sanitize(preview.title),
            sanitize(preview.description),
            sanitize(preview.url),
            sanitize(preview.image));

        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, 0);
        MDB_val k = {url.size(),   (void*)url.data()};
        MDB_val v = {value.size(), (void*)value.data()};
        mdb_put(txn.handle(), mam_dbi.og_previews.handle(), &k, &v, 0);
        txn.commit();
    } catch (const lmdb::error&) {}
}

std::expected<weechat::account::og_preview, std::string>
weechat::account::og_cache_lookup(std::string_view url)
{
    if (!mam_db_env || url.empty()) return std::unexpected("no db or empty url");
    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, MDB_RDONLY);
        MDB_val k = {url.size(), (void*)url.data()};
        MDB_val v{};
        if (mdb_get(txn.handle(), mam_dbi.og_previews.handle(), &k, &v) != 0)
            return std::unexpected("preview not found");
        std::string_view sv(static_cast<const char*>(v.mv_data), v.mv_size);
        // Split on \t into up to 4 fields: title, description, url, image
        og_preview p;
        std::array<std::string*, 4> fields = {&p.title, &p.description, &p.url, &p.image};
        size_t field_idx = 0;
        for (auto part : sv | std::views::split('\t')) {
            if (field_idx >= 4) break;
            *fields[field_idx++] = std::string(part.begin(), part.end());
        }
        return p;
    } catch (const lmdb::error&) {
        return std::unexpected("db error");
    }
}

// Client State Indication (XEP-0352) - Idle timer callback
