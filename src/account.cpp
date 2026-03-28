// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <strophe.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <fmt/core.h>
#include <fmt/ranges.h>
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
#include "debug.hh"

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
        *out = device->second;
        return true;
    }

    return false;
}

void weechat::account::add_device(weechat::account::device *device)
{
    if (!device || device->id == 0 || device->id > 0x7fffffffU)
    {
        weechat_printf(buffer,
                       "%somemo: ignoring invalid cached device id %u",
                       weechat_prefix("error"), device ? device->id : 0);
        return;
    }

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

    if (omemo.device_id == 0 || omemo.device_id > 0x7fffffffU)
    {
        weechat_printf(buffer,
                       "%somemo: refusing to publish devicelist: invalid local device id %u",
                       weechat_prefix("error"), omemo.device_id);
        return nullptr;
    }

    account::device device;

    device.id = omemo.device_id;
    device.name = fmt::format("{}", device.id);
    device.label = "weechat";

    std::vector<xmpp_stanza_t*> children_vec(devices.size() + 4, nullptr);
    xmpp_stanza_t **children = children_vec.data();
    children[i++] = stanza__iq_pubsub_publish_item_list_device(
        context, NULL, with_noop(device.name.c_str()), with_noop(nullptr));

    for (auto& device : devices)
    {
        if (device.first == omemo.device_id || device.first == 0 || device.first > 0x7fffffffU)
            continue;

        if (device.second.name == "%u")
            continue;

        const auto expected_device_name = fmt::format("{}", device.first);
        if (device.second.name != expected_device_name)
        {
            weechat_printf(buffer,
                           "%somemo: skipping cached device entry %u with mismatched published id '%s'",
                           weechat_prefix("error"),
                           device.first,
                           device.second.name.c_str());
            continue;
        }

        if (device.second.label.empty())
            device.second.label = "weechat";

        if (device.first != omemo.device_id)
            children[i++] = stanza__iq_pubsub_publish_item_list_device(
                context,
                NULL,
                with_noop(device.second.name.data()),
                with_noop(device.second.label.empty() ? nullptr : device.second.label.c_str()));
    }

    children[i] = NULL;
    const char *node = "urn:xmpp:omemo:2";
    children[0] = stanza__iq_pubsub_publish_item_list(
        context, NULL, children, with_noop(node));
    children[1] = NULL;
    children[0] = stanza__iq_pubsub_publish_item(
        context, NULL, children, with_noop("current"));
    node = "urn:xmpp:omemo:2:devices";
    children[0] = stanza__iq_pubsub_publish(context, NULL, children, with_noop(node));
    const char *ns = "http://jabber.org/protocol/pubsub";
    children[0] = stanza__iq_pubsub(context, NULL, children, with_noop(ns));

    // Add publish-options so the server delivers PubSub notifications to
    // contacts and allows them to fetch our devicelist (access_model=open).
    // Without this many servers default to presence/whitelist which silently
    // prevents remote clients from seeing our device.
    {
        xmpp_stanza_t *pubsub = children[0];

        auto make_field = [&](const char *var, const char *val, const char *type = nullptr) {
            return stanza_make_field(context, var, val, type);
        };

        xmpp_stanza_t *x = xmpp_stanza_new(context);
        xmpp_stanza_set_name(x, "x");
        xmpp_stanza_set_ns(x, "jabber:x:data");
        xmpp_stanza_set_attribute(x, "type", "submit");

        xmpp_stanza_t *f1 = make_field("FORM_TYPE",
            "http://jabber.org/protocol/pubsub#publish-options", "hidden");
        xmpp_stanza_t *f2 = make_field("pubsub#max_items", "max");
        xmpp_stanza_t *f3 = make_field("pubsub#access_model", "open");
        xmpp_stanza_t *f4 = make_field("pubsub#persist_items", "true");

        xmpp_stanza_add_child(x, f1); xmpp_stanza_release(f1);
        xmpp_stanza_add_child(x, f2); xmpp_stanza_release(f2);
        xmpp_stanza_add_child(x, f3); xmpp_stanza_release(f3);
        xmpp_stanza_add_child(x, f4); xmpp_stanza_release(f4);

        xmpp_stanza_t *publish_options = xmpp_stanza_new(context);
        xmpp_stanza_set_name(publish_options, "publish-options");
        xmpp_stanza_add_child(publish_options, x);
        xmpp_stanza_release(x);

        xmpp_stanza_add_child(pubsub, publish_options);
        xmpp_stanza_release(publish_options);
    }

    xmpp_stanza_t * parent = stanza__iq(context, NULL,
                                        children, NULL, "announce1",
                                        NULL, NULL, "set");

    weechat_printf(buffer,
                   "%somemo: publishing devicelist for device %u with open access model",
                   weechat_prefix("network"), omemo.device_id);

    return parent;
}

xmpp_stanza_t *weechat::account::get_legacy_devicelist()
{
    if (omemo.device_id == 0 || omemo.device_id > 0x7fffffffU)
    {
        weechat_printf(buffer,
                       "%somemo: refusing to publish legacy devicelist: invalid local device id %u",
                       weechat_prefix("error"), omemo.device_id);
        return nullptr;
    }

    xmpp_stanza_t *list = xmpp_stanza_new(context);
    xmpp_stanza_set_name(list, "list");
    xmpp_stanza_set_ns(list, "eu.siacs.conversations.axolotl");

    auto add_device = [&](const std::string &id) {
        xmpp_stanza_t *dev = xmpp_stanza_new(context);
        xmpp_stanza_set_name(dev, "device");
        xmpp_stanza_set_attribute(dev, "id", id.c_str());
        xmpp_stanza_add_child(list, dev);
        xmpp_stanza_release(dev);
    };

    add_device(fmt::format("{}", omemo.device_id));

    for (auto &device : devices)
    {
        if (device.first == omemo.device_id || device.first == 0 || device.first > 0x7fffffffU)
            continue;

        if (device.second.name == "%u")
            continue;

        const auto expected_device_name = fmt::format("{}", device.first);
        if (device.second.name != expected_device_name)
        {
            weechat_printf(buffer,
                           "%somemo: skipping cached device entry %u with mismatched published id '%s' (legacy devicelist)",
                           weechat_prefix("error"),
                           device.first,
                           device.second.name.c_str());
            continue;
        }

        add_device(device.second.name);
    }

    xmpp_stanza_t *item_children[] = {list, nullptr};
    xmpp_stanza_t *item = stanza__iq_pubsub_publish_item(
        context, nullptr, item_children, with_noop("current"));

    xmpp_stanza_t *publish_children[] = {item, nullptr};
    xmpp_stanza_t *publish = stanza__iq_pubsub_publish(
        context, nullptr, publish_children,
        with_noop("eu.siacs.conversations.axolotl.devicelist"));

    xmpp_stanza_t *pubsub_children[] = {publish, nullptr};
    xmpp_stanza_t *pubsub = stanza__iq_pubsub(
        context, nullptr, pubsub_children, with_noop("http://jabber.org/protocol/pubsub"));

    // Keep legacy node public/persistent as well so clients still using
    // OMEMO:1 can reliably discover this device.
    {
        auto make_field = [&](const char *var, const char *val, const char *type = nullptr) {
            return stanza_make_field(context, var, val, type);
        };

        xmpp_stanza_t *x = xmpp_stanza_new(context);
        xmpp_stanza_set_name(x, "x");
        xmpp_stanza_set_ns(x, "jabber:x:data");
        xmpp_stanza_set_attribute(x, "type", "submit");

        xmpp_stanza_t *f1 = make_field("FORM_TYPE",
            "http://jabber.org/protocol/pubsub#publish-options", "hidden");
        xmpp_stanza_t *f2 = make_field("pubsub#max_items", "max");
        xmpp_stanza_t *f3 = make_field("pubsub#access_model", "open");
        xmpp_stanza_t *f4 = make_field("pubsub#persist_items", "true");

        xmpp_stanza_add_child(x, f1); xmpp_stanza_release(f1);
        xmpp_stanza_add_child(x, f2); xmpp_stanza_release(f2);
        xmpp_stanza_add_child(x, f3); xmpp_stanza_release(f3);
        xmpp_stanza_add_child(x, f4); xmpp_stanza_release(f4);

        xmpp_stanza_t *publish_options = xmpp_stanza_new(context);
        xmpp_stanza_set_name(publish_options, "publish-options");
        xmpp_stanza_add_child(publish_options, x);
        xmpp_stanza_release(x);

        xmpp_stanza_add_child(pubsub, publish_options);
        xmpp_stanza_release(publish_options);
    }

    xmpp_stanza_t *iq_children[] = {pubsub, nullptr};
    xmpp_stanza_t *parent = stanza__iq(context, nullptr,
                                       iq_children, nullptr, "announce-legacy1",
                                       nullptr, nullptr, "set");

    weechat_printf(buffer,
                   "%somemo: publishing legacy devicelist for device %u with open access model",
                   weechat_prefix("network"), omemo.device_id);

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

    // sm_outqueue holds shared_ptr<xmpp_stanza_t> which call xmpp_stanza_release()
    // on destruction. xmpp_stanza_release() needs the libstrophe context to be alive.
    // However, `context` is a member declared after `sm_outqueue`, so it is destroyed
    // FIRST in reverse-declaration order — meaning the deque destructor would call
    // xmpp_stanza_release() on an already-freed context, causing a segfault.
    // Explicitly drain the queue here, while the context is still valid.
    sm_outqueue.clear();

    // channels are destroyed after mam_db_env in member reverse-destruction order,
    // but channel::~channel() updates MAM state for PM buffers. Destroy channels
    // explicitly here while the cache environment is still alive.
    channels.clear();

    mam_cache_cleanup();
}

void weechat::account::disconnect(int reconnect)
{
    // Safety check: if plugin is destroyed, don't call weechat functions
    if (!weechat::plugin::instance || !weechat::plugin::instance->ptr())
        return;

    // Clean up any in-flight XEP-0363 upload threads BEFORE unhooking fd hooks.
    // std::thread::~thread() calls std::terminate() if the thread is still joinable,
    // so we must join every pending thread here. We close the write-end of the pipe
    // first so curl_easy_perform will encounter a broken connection sooner, then
    // we drain the read-end and join. The thread always writes one byte before
    // exiting, so join() will return quickly once the upload finishes or errors.
    if (!pending_uploads.empty())
    {
        for (auto& [read_fd, ctx] : pending_uploads)
        {
            // Close the write end to cause curl's transfer to fail/abort sooner
            if (ctx->pipe_write_fd >= 0)
            {
                close(ctx->pipe_write_fd);
                ctx->pipe_write_fd = -1;
            }

            // Join the worker thread (it will exit after its current curl call)
            if (ctx->worker.joinable())
                ctx->worker.join();

            // Unhook the fd watcher and close the read end
            if (ctx->hook)
            {
                weechat_unhook(ctx->hook);
                ctx->hook = nullptr;
            }
            // Drain the pipe so we don't leave stale data
            char sig[1];
            (void)::read(read_fd, sig, sizeof(sig));
            close(read_fd);
        }
        pending_uploads.clear();
    }

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
    
    // libstrophe's built-in SM is disabled via XMPP_CONN_FLAG_DISABLE_SM
    // (set in connect()), so xmpp_conn_get_sm_state() will always return
    // nullptr here — no clearing needed.


    if (is_connected)
    {
        /*
         * remove all nicks and write disconnection message on each
         * channel/private buffer
         */
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
        // Exponential backoff: 5 → 10 → 20 → 40 → 80 → 120s (capped)
        if (reconnect_delay == 0)
            reconnect_delay = 5;
        else
            reconnect_delay = std::min(reconnect_delay * 2, 120);
        current_retry++;
        reconnect_start = time(NULL) + reconnect_delay;
        weechat_printf(buffer,
                       "%sxmpp: reconnecting in %ds (attempt %d)…",
                       weechat_prefix("network"),
                       reconnect_delay,
                       current_retry);
    }
    else
    {
        current_retry = 0;
        reconnect_delay = 0;
        reconnect_start = 0;
    }

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
                // Clear reconnect_start BEFORE calling connect() so that a
                // failed connect() does not cause an immediate re-fire on the
                // next timer tick. The next disconnect() call will set a new
                // (longer) reconnect_start via the exponential backoff.
                ptr_account.second.reconnect_start = 0;
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
    std::vector<std::string> key_entries;
    for (auto& channel_pair : channels)
    {
        auto& channel = channel_pair.second;
        for (const auto& key : channel.pgp.ids)
            key_entries.push_back(fmt::format("{}:{}", channel.id, key));
    }
    option_pgp_keys = fmt::format("{}", fmt::join(key_entries, ","));
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

// ---------------------------------------------------------------------------
// LMDB cache: MAM message store, capability cache, MAM cursors, bookmarks.
// ---------------------------------------------------------------------------
#include "account/lmdb_cache.inl"

// ---------------------------------------------------------------------------
// WeeChat timer / event callbacks (idle, activity, SM ack, upload fd).
// ---------------------------------------------------------------------------
#include "account/callbacks.inl"

