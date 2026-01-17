// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <strophe.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <fmt/core.h>
#include <libxml/xmlwriter.h>
#include <libxml/xmlerror.h>
#include <libxml/parser.h>
#include <weechat/weechat-plugin.h>
#include <filesystem>
#include <lmdb++.h>

#include "plugin.hh"
#include "xmpp/node.hh"
#include "xmpp/stanza.hh"
#include "config.hh"
#include "input.hh"
#include "omemo.hh"
#include "account.hh"
#include "connection.hh"
#include "user.hh"
#include "channel.hh"
#include "buffer.hh"

// Use a pointer that's never freed to prevent destructor running at program exit
// when plugin is already unloaded. Memory is leaked but OS reclaims it.
std::unordered_map<std::string, weechat::account>& weechat::accounts = 
    *new std::unordered_map<std::string, weechat::account>();

void weechat::log_emit(void *const userdata, const xmpp_log_level_t level,
                       const char *const area, const char *const msg)
{
    auto account = static_cast<weechat::account*>(userdata);

    static const char *log_level_name[4] = {"debug", "info", "warn", "error"};

    // Skip all debug logs - only show info, warnings, and errors
    if (level == XMPP_LEVEL_DEBUG)
        return;

    const char *tags = level > XMPP_LEVEL_DEBUG ? "no_log" : NULL;

    weechat_printf_date_tags(
        account ? account->buffer : NULL,
        0, tags,
        _("%s%s (%s): %s"),
        weechat_prefix("network"), area,
        log_level_name[level], msg);
}

bool weechat::account::search(weechat::account* &out,
                              const std::string name, bool casesensitive)
{
    if (name.empty())
        return false;

    if (casesensitive)
    {
        for (auto& account : weechat::accounts)
        {
            if (weechat_strcasecmp(account.second.name.data(), name.data()) == 0)
            {
                out = &account.second;
                return true;
            }
        }
    }
    else if (auto account = accounts.find(name); account != accounts.end())
    {
        out = &account->second;
        return true;
    }

    (void) out;
    return false;
}

bool weechat::account::search_device(weechat::account::device* out, std::uint32_t id)
{
    if (id == 0)
        return false;

    if (auto device = devices.find(id); device != devices.end())
    {
        out = &device->second;
        return true;
    }

    (void) out;
    return false;
}

void weechat::account::add_device(weechat::account::device *device)
{
    if (!devices.contains(device->id))
    {
        devices[device->id].id = device->id;
        devices[device->id].name = device->name;
        devices[device->id].label = device->label;
    }
}

void weechat::account::device_free_all()
{
    devices.clear();
}

xmpp_stanza_t *weechat::account::get_devicelist()
{
    int i = 0;

    account::device device;

    device.id = omemo.device_id;
    device.name = fmt::format("%u", device.id);
    device.label = "weechat";

    auto children = new xmpp_stanza_t*[128];
    children[i++] = stanza__iq_pubsub_publish_item_list_device(
        context, NULL, with_noop(device.name.data()), NULL);

    for (auto& device : devices)
    {
        if (device.first != omemo.device_id)
            children[i++] = stanza__iq_pubsub_publish_item_list_device(
                context, NULL, with_noop(device.second.name.data()), NULL);
    }

    children[i] = NULL;
    const char *node = "eu.siacs.conversations.axolotl";
    children[0] = stanza__iq_pubsub_publish_item_list(
        context, NULL, children, with_noop(node));
    children[1] = NULL;
    children[0] = stanza__iq_pubsub_publish_item(
        context, NULL, children, with_noop("current"));
    node = "eu.siacs.conversations.axolotl.devicelist";
    children[0] = stanza__iq_pubsub_publish(context, NULL, children, with_noop(node));
    const char *ns = "http://jabber.org/protocol/pubsub";
    children[0] = stanza__iq_pubsub(context, NULL, children, with_noop(ns));
    xmpp_stanza_t * parent = stanza__iq(context, NULL,
                                        children, NULL, strdup("announce1"),
                                        NULL, NULL, strdup("set"));
    delete[] children;

    return parent;
}

void weechat::account::add_mam_query(const std::string id, const std::string with,
                                     std::optional<time_t> start, std::optional<time_t> end)
{
    if (!mam_queries.contains(id))
    {
        mam_queries[id].id = id;
        mam_queries[id].with = with;

        mam_queries[id].start = start;
        mam_queries[id].end = end;
    }
}

bool weechat::account::mam_query_search(weechat::account::mam_query* out,
                                        const std::string id)
{
    if (id.empty())
        return false;

    if (auto mam_query = mam_queries.find(id); mam_query != mam_queries.end())
    {
        *out = mam_query->second;  // Dereference pointer to copy the query data
        return true;
    }

    (void) out;
    return false;
}

void weechat::account::mam_query_remove(const std::string id)
{
    mam_queries.erase(id);
}

void weechat::account::mam_query_free_all()
{
    mam_queries.clear();
}

xmpp_log_t make_logger(void *userdata)
{
    xmpp_log_t logger = { nullptr };
    logger.handler = &weechat::log_emit;
    logger.userdata = userdata;
    return logger;
}

xmpp_mem_t make_memory(void *userdata)
{
    xmpp_mem_t memory = { nullptr };
    memory.alloc = [](const size_t size, void *const) {
        return calloc(1, size);
    };
    memory.free = [](void *ptr, void *const) {
        free(ptr);
    };
    memory.realloc = [](void *ptr, const size_t size, void *const) {
        return realloc(ptr, size);
    };
    memory.userdata = userdata;
    return memory;
}

weechat::account::account(config_file& config_file, const std::string name)
    : config_account(config_file, config_file.configuration.section_account, name.data())
    , memory(make_memory(this)), logger(make_logger(this))
    , name(name), context(&memory, &logger), connection(*this, context)
{
    if (account* result = nullptr; account::search(result, name))
        throw std::invalid_argument("account already exists");

    this->jid(config_file.configuration.account_default.option_jid.string().data());
    this->password(config_file.configuration.account_default.option_password.string().data());
    this->tls(config_file.configuration.account_default.option_tls.string().data());
    this->nickname(config_file.configuration.account_default.option_nickname.string().data());
    this->autoconnect(config_file.configuration.account_default.option_autoconnect.string().data());
    this->resource(config_file.configuration.account_default.option_resource.string().data());
    this->status(config_file.configuration.account_default.option_status.string().data());
    this->pgp_path(config_file.configuration.account_default.option_pgp_path.string().data());
    this->pgp_keyid(config_file.configuration.account_default.option_pgp_keyid.string().data());
    
    mam_cache_init();
}

weechat::account::~account()
{
    // Safety check: if plugin is destroyed, skip all cleanup
    // Global destructors will run after plugin is destroyed
    // Let the OS reclaim resources instead of trying to clean up
    if (!weechat::plugin::instance || !weechat::plugin::instance->ptr())
        return;
    
    mam_cache_cleanup();
        
    /*
     * Don't close the buffer explicitly - let weechat handle cleanup
     * Closing it here causes segfaults because channels are destroyed
     * while their hooks are still active
     */
    // if (buffer)
    //     weechat_buffer_close(buffer);
}

void weechat::account::disconnect(int reconnect)
{
    // Safety check: if plugin is destroyed, don't call weechat functions
    if (!weechat::plugin::instance || !weechat::plugin::instance->ptr())
        return;
        
    // Clean up Client State Indication hooks
    if (idle_timer_hook)
    {
        weechat_unhook(idle_timer_hook);
        idle_timer_hook = nullptr;
    }
    
    for (int i = 0; i < 3; i++)
    {
        if (csi_activity_hooks[i])
        {
            weechat_unhook(csi_activity_hooks[i]);
            csi_activity_hooks[i] = nullptr;
        }
    }
    
    // Clean up Stream Management hooks
    if (sm_ack_timer_hook)
    {
        weechat_unhook(sm_ack_timer_hook);
        sm_ack_timer_hook = nullptr;
    }
    
    // TEMPORARY: Disabled SM state clearing as it's causing crashes
    // TODO: Figure out why xmpp_conn_get_sm_state() is crashing
    // Clear libstrophe's internal SM state to prevent auto-resume
    // This is critical - libstrophe persists SM state across reconnects!
    // Must check if connection is valid AND connected before accessing state
    // if (is_connected)
    // {
    //     xmpp_conn_t *conn_ptr = connection;  // Uses operator xmpp_conn_t*()
    //     if (conn_ptr)
    //     {
    //         xmpp_sm_state_t *sm_state = xmpp_conn_get_sm_state(conn_ptr);
    //         if (sm_state)
    //         {
    //             xmpp_free_sm_state(sm_state);
    //             xmpp_conn_set_sm_state(conn_ptr, nullptr);
    //         }
    //     }
    // }
        
    if (is_connected)
    {
        /*
         * remove all nicks and write disconnection message on each
         * channel/private buffer
         */
      //user::free_all(this); // TOFIX
        if (buffer)
            weechat_nicklist_remove_all(buffer);
        for (auto& ptr_channel : channels)
        {
            if (ptr_channel.second.buffer)
            {
                weechat_nicklist_remove_all(ptr_channel.second.buffer);
                weechat_printf(
                    ptr_channel.second.buffer,
                    _("%s%s: disconnected from account"),
                    weechat_prefix("network"), WEECHAT_XMPP_PLUGIN_NAME);
            }
        }
        /* remove away status on account buffer */
        //weechat_buffer_set(buffer, "localvar_del_away", "");
    }

    reset();

    if (buffer)
    {
        weechat_printf(
            buffer,
            _("%s%s: disconnected from account"),
            weechat_prefix ("network"), WEECHAT_XMPP_PLUGIN_NAME);
    }

    if (reconnect)
    {
        if (current_retry++ == 0)
        {
            reconnect_delay = 5;
            reconnect_start = time(NULL) + reconnect_delay;
        }
        current_retry %= 5;
    }
    else
    {
        current_retry = 0;
        reconnect_delay = 0;
        reconnect_start = 0;
    }

    /*
    lag = 0;
    lag_displayed = -1;
    lag_check_time.tv_sec = 0;
    lag_check_time.tv_usec = 0;
    lag_next_check = time(NULL) +
        weechat_config_integer(xmpp_config_network_lag_check);
    lag_last_refresh = 0;
    account__set_lag(account);
    */ // lag based on xmpp ping

    disconnected = !reconnect;

    /* send signal "account_disconnected" with account name */
    (void) weechat_hook_signal_send("xmpp_account_disconnected",
                                    WEECHAT_HOOK_SIGNAL_STRING, name.data());
}

void weechat::account::disconnect_all()
{
    for (auto& account : accounts)
    {
        account.second.disconnect(0);
    }
}

struct t_gui_buffer *weechat::account::create_buffer()
{
    buffer = weechat_buffer_new(fmt::format("account.{}", name).data(),
                                &input__data_cb, NULL, NULL,
                                &buffer__close_cb, NULL, NULL);
    if (!buffer)
        return NULL;
    weechat_printf(buffer, "xmpp: %s", name.data());

    if (!weechat_buffer_get_integer(buffer, "short_name_is_set"))
        weechat_buffer_set(buffer, "short_name", name.data());
    weechat_buffer_set(buffer, "localvar_set_type", "server");
    weechat_buffer_set(buffer, "localvar_set_account", name.data());
    weechat_buffer_set(buffer, "localvar_set_charset_modifier",
                       fmt::format("account.{}", name).data());
    weechat_buffer_set(buffer, "title", name.data());

    weechat_buffer_set(buffer, "nicklist", "1");
    weechat_buffer_set(buffer, "nicklist_display_groups", "0");
    weechat_buffer_set_pointer(buffer, "nicklist_callback",
                               (void*)&buffer__nickcmp_cb);
    weechat_buffer_set_pointer(buffer, "nicklist_callback_pointer",
                               this);

    return buffer;
}

void weechat::account::reset()
{
    if (connection)
    {
        if (xmpp_conn_is_connected(connection))
            xmpp_disconnect(connection);
    }

    is_connected = 0;
}

int weechat::account::connect()
{
    return connect(false);  // Not a manual connect
}

int weechat::account::connect(bool manual)
{
    if (!buffer)
    {
        if (!create_buffer())
            return 0;
        // Don't auto-switch to account buffer on connect
        weechat_buffer_set(buffer, "display", "1");
    }

    reset();
    
    // CRITICAL: Disable libstrophe's built-in Stream Management
    // Libstrophe has automatic SM support that persists across reconnects
    // and can cause connection issues. We're implementing our own SM.
    xmpp_conn_t *conn_ptr = connection;
    if (conn_ptr)
    {
        long flags = xmpp_conn_get_flags(conn_ptr);
        flags |= XMPP_CONN_FLAG_DISABLE_SM;  // Set the disable flag
        xmpp_conn_set_flags(conn_ptr, flags);
    }
    
    // Only reset SM availability on MANUAL connect (allow retry)
    // Auto-reconnects preserve the sm_available state
    if (manual)
    {
        sm_available = true;
    }

    is_connected = connection.connect(std::string(jid()), std::string(password()), tls());

    (void) weechat_hook_signal_send("xmpp_account_connecting",
                                    WEECHAT_HOOK_SIGNAL_STRING, name.data());

    return is_connected;
}

// Definition of the global unloading flag
bool weechat::g_plugin_unloading = false;

int weechat::account::timer_cb(const void *pointer, void *data, int remaining_calls)
{
    (void) pointer;
    (void) data;
    (void) remaining_calls;

    // Wrap EVERYTHING in try-catch to prevent crashes from any source
    try {
        // Quick check before accessing any plugin state
        if (weechat::g_plugin_unloading)
            return WEECHAT_RC_OK;

        // Safety check: ensure plugin is still valid
        if (!weechat::plugin::instance)
            return WEECHAT_RC_OK;
        if (!weechat::plugin::instance->ptr())
            return WEECHAT_RC_OK;

        if (accounts.empty()) 
            return WEECHAT_RC_ERROR;

        for (auto& ptr_account : accounts)
        {
            if (ptr_account.second.is_connected
                && (xmpp_conn_is_connecting(ptr_account.second.connection)
                    || xmpp_conn_is_connected(ptr_account.second.connection)))
                ptr_account.second.connection.process(ptr_account.second.context, 10);
            else if (ptr_account.second.disconnected);
            else if (ptr_account.second.reconnect_start > 0
                     && ptr_account.second.reconnect_start < time(NULL))
            {
                ptr_account.second.connect();
            }
        }

        return WEECHAT_RC_OK;
    }
    catch (...) {
        // Silently catch all exceptions during plugin reload
        return WEECHAT_RC_OK;
    }
}

void weechat::account::save_pgp_keys()
{
    std::string keys_str;
    
    for (auto& channel_pair : channels)
    {
        auto& channel = channel_pair.second;
        if (!channel.pgp.ids.empty())
        {
            for (const auto& key : channel.pgp.ids)
            {
                if (!keys_str.empty())
                    keys_str += ",";
                keys_str += channel.id + ":" + key;
            }
        }
    }
    
    option_pgp_keys = keys_str;
}

void weechat::account::load_pgp_keys()
{
    std::string_view keys_str = option_pgp_keys.string();
    if (keys_str.empty())
        return;
    
    // Parse "jid:keyid,jid2:keyid2,..."
    char **pairs = weechat_string_split(keys_str.data(), ",", NULL, 0, 0, NULL);
    if (!pairs)
        return;
    
    for (int i = 0; pairs[i]; i++)
    {
        char **parts = weechat_string_split(pairs[i], ":", NULL, 0, 2, NULL);
        if (parts && parts[0] && parts[1])
        {
            std::string jid = parts[0];
            std::string keyid = parts[1];
            
            // Find or create channel and add key
            auto channel_it = channels.find(jid);
            if (channel_it != channels.end())
            {
                channel_it->second.pgp.ids.emplace(keyid);
            }
        }
        if (parts)
            weechat_string_free_split(parts);
    }
    
    weechat_string_free_split(pairs);
}

void weechat::account::mam_cache_init()
{
    try {
        mam_db_path = std::shared_ptr<char>(
            weechat_string_eval_expression(
                fmt::format("${{weechat_data_dir}}/xmpp/mam_{}.db", name.data()).data(),
                NULL, NULL, NULL),
            &free).get();
        
        std::filesystem::create_directories(
            std::filesystem::path(mam_db_path.data()).parent_path());

        mam_db_env = lmdb::env::create();
        mam_db_env.set_max_dbs(10);
        mam_db_env.set_mapsize((size_t)1048576 * 1000); // 1000MB
        mam_db_env.open(mam_db_path.data(), MDB_NOSUBDIR, 0664);

        lmdb::txn parentTransaction{nullptr};
        lmdb::txn transaction = lmdb::txn::begin(mam_db_env, parentTransaction);

        mam_dbi.messages = lmdb::dbi::open(transaction, "messages", MDB_CREATE);
        mam_dbi.timestamps = lmdb::dbi::open(transaction, "timestamps", MDB_CREATE);
        mam_dbi.capabilities = lmdb::dbi::open(transaction, "capabilities", MDB_CREATE);
        mam_dbi.retractions = lmdb::dbi::open(transaction, "retractions", MDB_CREATE);

        transaction.commit();
        
        // Load capability cache from database
        caps_cache_load();
    } catch (const lmdb::error& ex) {
        weechat_printf(NULL, "%sxmpp: MAM cache init failed - %s",
                      weechat_prefix("error"), ex.what());
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
        weechat_printf(NULL, "%sxmpp: MAM cache cleanup failed - %s",
                      weechat_prefix("error"), ex.what());
    }
}

void weechat::account::mam_cache_message(const std::string& channel_jid,
                                         const std::string& message_id,
                                         const std::string& from,
                                         time_t timestamp,
                                         const std::string& body)
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

void weechat::account::mam_cache_retract_message(const std::string& channel_jid, const std::string& message_id)
{
    if (!mam_db_env || message_id.empty()) return;
    
    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, 0);
        
        // Key: channel_jid:message_id
        std::string key = fmt::format("{}:{}", channel_jid, message_id);
        
        // Value: just a marker (timestamp when retracted)
        time_t now = time(NULL);
        
        MDB_val k = {key.size(), (void*)key.data()};
        MDB_val v = {sizeof(now), &now};
        
        mdb_put(txn.handle(), mam_dbi.retractions.handle(), &k, &v, 0);
        
        txn.commit();
    } catch (const lmdb::error& ex) {
        // Silently ignore cache write errors
    }
}

bool weechat::account::mam_cache_is_retracted(const std::string& channel_jid, const std::string& message_id)
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

void weechat::account::mam_cache_load_messages(const std::string& channel_jid, struct t_gui_buffer *buffer)
{
    if (!mam_db_env || !buffer) return;
    
    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, MDB_RDONLY);
        
        MDB_cursor *cursor;
        mdb_cursor_open(txn.handle(), mam_dbi.messages.handle(), &cursor);
        
        // Start with channel prefix
        std::string prefix = channel_jid + ":";
        MDB_val key = {prefix.size(), (void*)prefix.data()};
        MDB_val value;
        
        int count = 0;
        int rc = mdb_cursor_get(cursor, &key, &value, MDB_SET_RANGE);
        
        while (rc == 0 && count < 100)  // Limit to last 100 cached messages
        {
            std::string key_str((char*)key.mv_data, key.mv_size);
            
            // Check if key still belongs to our channel
            if (key_str.substr(0, prefix.size()) != prefix)
                break;
            
            // Parse key: channel_jid:timestamp:message_id
            size_t colon1 = key_str.find(':', prefix.size());
            size_t colon2 = key_str.find(':', colon1 + 1);
            std::string message_id;
            if (colon2 != std::string::npos)
                message_id = key_str.substr(colon2 + 1);
            
            // Parse value: from|timestamp|body
            std::string value_str((char*)value.mv_data, value.mv_size);
            size_t pos1 = value_str.find('|');
            size_t pos2 = value_str.find('|', pos1 + 1);
            
            if (pos1 != std::string::npos && pos2 != std::string::npos)
            {
                std::string from = value_str.substr(0, pos1);
                std::string timestamp_str = value_str.substr(pos1 + 1, pos2 - pos1 - 1);
                std::string body = value_str.substr(pos2 + 1);
                
                time_t timestamp = std::stoll(timestamp_str);
                
                // Check if message is retracted
                bool is_retracted = !message_id.empty() && mam_cache_is_retracted(channel_jid, message_id);
                
                // Display cached message with gray prefix
                if (is_retracted)
                {
                    weechat_printf_date_tags(buffer, timestamp, "xmpp_cached,xmpp_retracted,no_highlight",
                                            "%s%s\t%s[Message deleted]%s",
                                            weechat_color("darkgray"),
                                            from.c_str(),
                                            weechat_color("darkgray"),
                                            weechat_color("resetcolor"));
                }
                else
                {
                    weechat_printf_date_tags(buffer, timestamp, "xmpp_cached,no_highlight",
                                            "%s%s\t%s",
                                            weechat_color("darkgray"),
                                            from.c_str(),
                                            body.c_str());
                }
                count++;
            }
            
            rc = mdb_cursor_get(cursor, &key, &value, MDB_NEXT);
        }
        
        mdb_cursor_close(cursor);
        txn.abort();
        
        if (count > 0)
        {
            weechat_printf(buffer, "%s--- %d cached messages loaded ---",
                          weechat_prefix("network"), count);
        }
    } catch (const lmdb::error& ex) {
        // Silently ignore read errors
    } catch (const std::exception& ex) {
        // Silently ignore parsing errors
    }
}

void weechat::account::mam_cache_clear_messages(const std::string& channel_jid)
{
    if (!mam_db_env) return;
    
    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, 0);
        
        MDB_cursor *cursor;
        mdb_cursor_open(txn.handle(), mam_dbi.messages.handle(), &cursor);
        
        // Start with channel prefix
        std::string prefix = channel_jid + ":";
        MDB_val key = {prefix.size(), (void*)prefix.data()};
        MDB_val value;
        
        int rc = mdb_cursor_get(cursor, &key, &value, MDB_SET_RANGE);
        
        while (rc == 0)
        {
            std::string key_str((char*)key.mv_data, key.mv_size);
            
            // Check if key still belongs to our channel
            if (key_str.substr(0, prefix.size()) != prefix)
                break;
            
            // Delete this entry
            mdb_cursor_del(cursor, 0);
            
            // Move to next
            rc = mdb_cursor_get(cursor, &key, &value, MDB_NEXT);
        }
        
        mdb_cursor_close(cursor);
        txn.commit();
    } catch (const lmdb::error& ex) {
        // Silently ignore errors
    }
}

time_t weechat::account::mam_cache_get_last_timestamp(const std::string& channel_jid)
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

void weechat::account::mam_cache_set_last_timestamp(const std::string& channel_jid, time_t timestamp)
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

void weechat::account::send_bookmarks()
{
    // XEP-0402: PEP Native Bookmarks (preferred)
    // Build PEP bookmark publish stanza
    // <iq type='set' id='...'>
    //   <pubsub xmlns='http://jabber.org/protocol/pubsub'>
    //     <publish node='urn:xmpp:bookmarks:1'>
    //       <item id='roomjid@conference.example.org'>
    //         <conference xmlns='urn:xmpp:bookmarks:1' name='...' autojoin='true'>
    //           <nick>...</nick>
    //         </conference>
    //       </item>
    //       ... more items ...
    //     </publish>
    //     <publish-options>
    //       <x xmlns='jabber:x:data' type='submit'>
    //         <field var='FORM_TYPE' type='hidden'>
    //           <value>http://jabber.org/protocol/pubsub#publish-options</value>
    //         </field>
    //         <field var='pubsub#persist_items'><value>true</value></field>
    //         <field var='pubsub#access_model'><value>whitelist</value></field>
    //       </x>
    //     </publish-options>
    //   </pubsub>
    // </iq>
    
    char *id = xmpp_uuid_gen(context);
    xmpp_stanza_t *iq = xmpp_iq_new(context, "set", id);
    xmpp_free(context, id);
    
    xmpp_stanza_t *pubsub = xmpp_stanza_new(context);
    xmpp_stanza_set_name(pubsub, "pubsub");
    xmpp_stanza_set_ns(pubsub, "http://jabber.org/protocol/pubsub");
    
    xmpp_stanza_t *publish = xmpp_stanza_new(context);
    xmpp_stanza_set_name(publish, "publish");
    xmpp_stanza_set_attribute(publish, "node", "urn:xmpp:bookmarks:1");
    
    // Add all bookmarks as items
    for (const auto& bookmark_pair : bookmarks)
    {
        const auto& bookmark = bookmark_pair.second;
        
        // <item id='jid'>
        xmpp_stanza_t *item = xmpp_stanza_new(context);
        xmpp_stanza_set_name(item, "item");
        xmpp_stanza_set_id(item, bookmark.jid.data());
        
        // <conference xmlns='urn:xmpp:bookmarks:1'>
        xmpp_stanza_t *conference = xmpp_stanza_new(context);
        xmpp_stanza_set_name(conference, "conference");
        xmpp_stanza_set_ns(conference, "urn:xmpp:bookmarks:1");
        
        if (!bookmark.name.empty())
            xmpp_stanza_set_attribute(conference, "name", bookmark.name.data());
        
        if (bookmark.autojoin)
            xmpp_stanza_set_attribute(conference, "autojoin", "true");
        
        // <nick>
        if (!bookmark.nick.empty())
        {
            xmpp_stanza_t *nick = xmpp_stanza_new(context);
            xmpp_stanza_set_name(nick, "nick");
            xmpp_stanza_t *nick_text = xmpp_stanza_new(context);
            xmpp_stanza_set_text(nick_text, bookmark.nick.data());
            xmpp_stanza_add_child(nick, nick_text);
            xmpp_stanza_add_child(conference, nick);
            xmpp_stanza_release(nick_text);
            xmpp_stanza_release(nick);
        }
        
        xmpp_stanza_add_child(item, conference);
        xmpp_stanza_add_child(publish, item);
        xmpp_stanza_release(conference);
        xmpp_stanza_release(item);
    }
    
    // Add publish-options to ensure persistence and privacy
    xmpp_stanza_t *publish_options = xmpp_stanza_new(context);
    xmpp_stanza_set_name(publish_options, "publish-options");
    
    xmpp_stanza_t *x = xmpp_stanza_new(context);
    xmpp_stanza_set_name(x, "x");
    xmpp_stanza_set_ns(x, "jabber:x:data");
    xmpp_stanza_set_attribute(x, "type", "submit");
    
    // FORM_TYPE field
    xmpp_stanza_t *field1 = xmpp_stanza_new(context);
    xmpp_stanza_set_name(field1, "field");
    xmpp_stanza_set_attribute(field1, "var", "FORM_TYPE");
    xmpp_stanza_set_attribute(field1, "type", "hidden");
    xmpp_stanza_t *value1 = xmpp_stanza_new(context);
    xmpp_stanza_set_name(value1, "value");
    xmpp_stanza_t *value1_text = xmpp_stanza_new(context);
    xmpp_stanza_set_text(value1_text, "http://jabber.org/protocol/pubsub#publish-options");
    xmpp_stanza_add_child(value1, value1_text);
    xmpp_stanza_add_child(field1, value1);
    xmpp_stanza_add_child(x, field1);
    xmpp_stanza_release(value1_text);
    xmpp_stanza_release(value1);
    xmpp_stanza_release(field1);
    
    // persist_items field
    xmpp_stanza_t *field2 = xmpp_stanza_new(context);
    xmpp_stanza_set_name(field2, "field");
    xmpp_stanza_set_attribute(field2, "var", "pubsub#persist_items");
    xmpp_stanza_t *value2 = xmpp_stanza_new(context);
    xmpp_stanza_set_name(value2, "value");
    xmpp_stanza_t *value2_text = xmpp_stanza_new(context);
    xmpp_stanza_set_text(value2_text, "true");
    xmpp_stanza_add_child(value2, value2_text);
    xmpp_stanza_add_child(field2, value2);
    xmpp_stanza_add_child(x, field2);
    xmpp_stanza_release(value2_text);
    xmpp_stanza_release(value2);
    xmpp_stanza_release(field2);
    
    // access_model field
    xmpp_stanza_t *field3 = xmpp_stanza_new(context);
    xmpp_stanza_set_name(field3, "field");
    xmpp_stanza_set_attribute(field3, "var", "pubsub#access_model");
    xmpp_stanza_t *value3 = xmpp_stanza_new(context);
    xmpp_stanza_set_name(value3, "value");
    xmpp_stanza_t *value3_text = xmpp_stanza_new(context);
    xmpp_stanza_set_text(value3_text, "whitelist");
    xmpp_stanza_add_child(value3, value3_text);
    xmpp_stanza_add_child(field3, value3);
    xmpp_stanza_add_child(x, field3);
    xmpp_stanza_release(value3_text);
    xmpp_stanza_release(value3);
    xmpp_stanza_release(field3);
    
    xmpp_stanza_add_child(publish_options, x);
    xmpp_stanza_release(x);
    
    xmpp_stanza_add_child(pubsub, publish);
    xmpp_stanza_add_child(pubsub, publish_options);
    xmpp_stanza_add_child(iq, pubsub);
    
    connection.send(iq);
    
    xmpp_stanza_release(publish_options);
    xmpp_stanza_release(publish);
    xmpp_stanza_release(pubsub);
    xmpp_stanza_release(iq);
}

// Capability cache implementation (XEP-0115)

void weechat::account::caps_cache_load()
{
    if (!mam_db_env) return;
    
    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, MDB_RDONLY);
        
        MDB_cursor *cursor;
        mdb_cursor_open(txn.handle(), mam_dbi.capabilities.handle(), &cursor);
        
        MDB_val key, value;
        int count = 0;
        
        while (mdb_cursor_get(cursor, &key, &value, MDB_NEXT) == 0)
        {
            std::string ver_hash((char*)key.mv_data, key.mv_size);
            std::string features_str((char*)value.mv_data, value.mv_size);
            
            // Parse features (stored as comma-separated)
            std::vector<std::string> features;
            size_t pos = 0;
            while (pos < features_str.length())
            {
                size_t next = features_str.find(',', pos);
                if (next == std::string::npos)
                {
                    features.push_back(features_str.substr(pos));
                    break;
                }
                features.push_back(features_str.substr(pos, next - pos));
                pos = next + 1;
            }
            
            caps_cache[ver_hash] = features;
            count++;
        }
        
        mdb_cursor_close(cursor);
        txn.abort();
        
        if (count > 0)
        {
            weechat_printf(buffer, "%sLoaded %d capability entries from cache",
                          weechat_prefix("network"), count);
        }
    } catch (const lmdb::error& ex) {
        // Silently ignore read errors
    }
}

void weechat::account::caps_cache_save(const std::string& verification_hash, 
                                        const std::vector<std::string>& features)
{
    if (!mam_db_env) return;
    
    // Store in memory cache
    caps_cache[verification_hash] = features;
    
    try {
        lmdb::txn parentTransaction{nullptr};
        lmdb::txn txn = lmdb::txn::begin(mam_db_env, parentTransaction, 0);
        
        // Join features with commas
        std::string features_str;
        for (size_t i = 0; i < features.size(); i++)
        {
            if (i > 0) features_str += ",";
            features_str += features[i];
        }
        
        MDB_val key = {verification_hash.size(), (void*)verification_hash.data()};
        MDB_val value = {features_str.size(), (void*)features_str.data()};
        
        mdb_put(txn.handle(), mam_dbi.capabilities.handle(), &key, &value, 0);
        
        txn.commit();
    } catch (const lmdb::error& ex) {
        // Silently ignore write errors
    }
}

bool weechat::account::caps_cache_get(const std::string& verification_hash,
                                       std::vector<std::string>& features)
{
    auto it = caps_cache.find(verification_hash);
    if (it != caps_cache.end())
    {
        features = it->second;
        return true;
    }
    return false;
}

// Client State Indication (XEP-0352) - Idle timer callback
int weechat::account::idle_timer_cb(const void *pointer, void *data, int remaining_calls)
{
    (void) data;
    (void) remaining_calls;

    if (weechat::g_plugin_unloading || !weechat::plugin::instance)
        return WEECHAT_RC_OK;

    account *account = (struct account *)pointer;
    if (!account || !account->connection || !xmpp_conn_is_connected(account->connection))
        return WEECHAT_RC_OK;

    time_t now = time(NULL);
    time_t idle_time = now - account->last_activity;

    // If idle for more than 5 minutes and currently active, send inactive
    if (idle_time > 300 && account->csi_active)
    {
        account->connection.send(stanza::xep0352::inactive()
                                .build(account->context)
                                .get());
        account->csi_active = false;
        weechat_printf(account->buffer, "%sClient state: inactive (idle for %ld seconds)",
                      weechat_prefix("network"), idle_time);
    }

    return WEECHAT_RC_OK;
}

// Client State Indication (XEP-0352) - Activity callback
int weechat::account::activity_cb(const void *pointer, void *data,
                                  const char *signal, const char *type_data,
                                  void *signal_data)
{
    (void) data;
    (void) signal;
    (void) type_data;
    (void) signal_data;

    if (weechat::g_plugin_unloading || !weechat::plugin::instance)
        return WEECHAT_RC_OK;

    account *account = (struct account *)pointer;
    if (!account || !account->connection || !xmpp_conn_is_connected(account->connection))
        return WEECHAT_RC_OK;

    // Update last activity timestamp
    account->last_activity = time(NULL);

    // If currently inactive, send active
    if (!account->csi_active)
    {
        account->connection.send(stanza::xep0352::active()
                                .build(account->context)
                                .get());
        account->csi_active = true;
        weechat_printf(account->buffer, "%sClient state: active",
                      weechat_prefix("network"));
    }

    return WEECHAT_RC_OK;
}

// Stream Management (XEP-0198) - Ack timer callback
int weechat::account::sm_ack_timer_cb(const void *pointer, void *data, int remaining_calls)
{
    (void) data;
    (void) remaining_calls;

    if (weechat::g_plugin_unloading || !weechat::plugin::instance)
        return WEECHAT_RC_OK;

    account *account = (struct account *)pointer;
    if (!account || !account->connection || !xmpp_conn_is_connected(account->connection))
        return WEECHAT_RC_OK;

    if (!account->sm_enabled)
        return WEECHAT_RC_OK;

    // Request acknowledgement from server
    account->connection.send(stanza::xep0198::request()
                            .build(account->context)
                            .get());

    return WEECHAT_RC_OK;
}
