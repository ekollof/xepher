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
#include <algorithm>
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

    const char *tags = level > XMPP_LEVEL_DEBUG ? "no_log" : nullptr;

    weechat_printf_date_tags(
        account ? account->buffer : nullptr,
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

std::shared_ptr<xmpp_stanza_t> weechat::account::get_devicelist()
{
    if (omemo.device_id == 0 || omemo.device_id > 0x7fffffffU)
    {
        weechat_printf(buffer,
                       "%somemo: refusing to publish legacy devicelist: invalid local device id %u",
                       weechat_prefix("error"), omemo.device_id);
        return nullptr;
    }

    // Collect peer device IDs from the LMDB-cached axolotl devicelist for our
    // own JID.  This is the server's last-known list and must be preserved so
    // we don't overwrite it with only our own device (singleton clobber bug).
    const std::string own_bare_jid = jid();
    const auto cached_ids = omemo.get_cached_device_ids(own_bare_jid);

    // <list xmlns='eu.siacs.conversations.axolotl'>
    struct list_spec : stanza::spec {
        list_spec(std::uint32_t self_id,
                  const std::vector<std::uint32_t> &cached)
            : spec("list")
        {
            attr("xmlns", "eu.siacs.conversations.axolotl");

            struct device_spec : stanza::spec {
                device_spec(std::string_view id_sv) : spec("device") {
                    attr("id", id_sv);
                }
            };

            // Always include our own device first.
            device_spec self_dev(fmt::format("{}", self_id));
            child(self_dev);

            // Re-include all previously published devices from the server list
            // so we do not clobber them.  This preserves other clients' device
            // entries (Gajim, Conversations, …) that they registered themselves.
            for (const auto dev_id : cached)
            {
                if (dev_id == self_id || dev_id == 0 || dev_id > 0x7fffffffU)
                    continue;
                device_spec peer_dev(fmt::format("{}", dev_id));
                child(peer_dev);
            }
        }
    };

    // Same 4-field publish-options as the OMEMO:2 devicelist
    struct legacy_pub_options : stanza::spec {
        legacy_pub_options() : spec("x") {
            attr("xmlns", "jabber:x:data");
            attr("type", "submit");
            struct field_spec : stanza::spec {
                field_spec(std::string_view var, std::string_view val,
                           std::string_view type_sv = {}) : spec("field") {
                    attr("var", var);
                    if (!type_sv.empty()) attr("type", type_sv);
                    struct value_spec : stanza::spec {
                        value_spec(std::string_view v) : spec("value") { text(v); }
                    } v(val);
                    child(v);
                }
            };
            field_spec f1("FORM_TYPE",
                "http://jabber.org/protocol/pubsub#publish-options", "hidden");
            child(f1);
            field_spec f2("pubsub#max_items", "max");
            child(f2);
            field_spec f3("pubsub#access_model", "open");
            child(f3);
            field_spec f4("pubsub#persist_items", "true");
            child(f4);
        }
    };

    list_spec list_el(omemo.device_id, cached_ids);

    // Keep legacy node public/persistent as well so clients still using
    // OMEMO:1 can reliably discover this device.
    legacy_pub_options pub_opts;
    auto pubsub_el = stanza::xep0060::pubsub()
        .publish(stanza::xep0060::publish("eu.siacs.conversations.axolotl.devicelist")
            .item(stanza::xep0060::item().id("current").payload(list_el)))
        .publish_options(stanza::xep0060::publish_options().child_spec(pub_opts));

    auto iq_s = stanza::iq().type("set").id(stanza::uuid(context));
    static_cast<stanza::xep0060::iq&>(iq_s).pubsub(pubsub_el);

    weechat_printf(buffer,
                   "%somemo: publishing legacy devicelist for device %u (%zu total device(s)) with open access model",
                   weechat_prefix("network"), omemo.device_id,
                   cached_ids.size() + 1);

    return iq_s.build(context);
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
        reconnect_start = time(nullptr) + reconnect_delay;
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
                                &input__data_cb, nullptr, nullptr,
                                &buffer__close_cb, nullptr, nullptr);
    if (!buffer)
        return nullptr;
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
                     && ptr_account.second.reconnect_start < time(nullptr))
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
    char **pairs = weechat_string_split(keys_str.data(), ",", nullptr, 0, 0, nullptr);
    if (!pairs)
        return;
    
    for (int i = 0; pairs[i]; i++)
    {
        char **parts = weechat_string_split(pairs[i], ":", nullptr, 0, 2, nullptr);
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

// (account/lmdb_cache.inl and account/callbacks.inl are compiled as
//  separate translation units: src/account/lmdb_cache.cpp and
//  src/account/callbacks.cpp)

// ---------------------------------------------------------------------------
// Feed draft saving: writes a pending_feed_post back to disk so the user
// can recover their work if an embed upload fails.
// ---------------------------------------------------------------------------
std::string weechat::account::save_feed_draft(const xepher::pending_feed_post &post)
{
    const char *data_dir = weechat_info_get("weechat_data_dir", nullptr);
    if (!data_dir)
        return {};

    // Ensure drafts directory exists
    std::string drafts_dir = fmt::format("{}/xmpp/drafts", data_dir);
    std::filesystem::create_directories(std::filesystem::path(drafts_dir));

    // Filename: <account>-<timestamp>.md
    std::string path = fmt::format("{}/{}-{}.md", drafts_dir, name, post.timestamp);
    // Replace colons in timestamp (Windows-safe, also avoids shell issues)
    for (auto &c : path)
        if (c == ':') c = '-';

    FILE *f = fopen(path.c_str(), "w");
    if (!f)
        return {};

    // YAML-like frontmatter so the user knows where to re-post
    fprintf(f, "---\n");
    fprintf(f, "service: %s\n", post.service.c_str());
    fprintf(f, "node: %s\n", post.node.c_str());
    if (!post.title.empty())
        fprintf(f, "title: %s\n", post.title.c_str());
    if (post.is_reply && !post.reply_to_id.empty())
        fprintf(f, "reply_to: %s\n", post.reply_to_id.c_str());
    fprintf(f, "---\n");
    fprintf(f, "%s\n", post.raw_body_template.c_str());
    fclose(f);

    return path;
}

// Build and publish an Atom <entry> for a completed pending_feed_post.
// Called after all embed uploads have finished (or immediately for tag-free posts).
void weechat::account::build_and_publish_post(const xepher::pending_feed_post &post)
{
    // Render the body (substitute {{ }} tags with Markdown)
    const std::string body = xepher::render_body(post.raw_body_template, post.embeds);

    // Convenience alias
    const std::string &pub_service       = post.service;
    const std::string &pub_node          = post.node;
    const std::string &reply_to_id       = post.reply_to_id;
    const std::string &reply_target_service = post.reply_target_service;
    const std::string &reply_target_node = post.reply_target_node;
    const std::string &item_uuid         = post.item_uuid;
    const std::string &atom_id           = post.atom_id;
    const std::string &post_title        = post.title;
    const std::string &ts_str            = post.timestamp;
    const bool         access_open       = post.access_open;
    const bool         is_comment        = !reply_to_id.empty();

    // Build Atom <entry> using stanza builder helpers (dynamic/conditional children require runtime)
    auto make_sp = [this](const char *tag, const char *ns = nullptr,
                          std::initializer_list<std::pair<const char*,const char*>> attrs = {})
        -> std::shared_ptr<xmpp_stanza_t>
    {
        return stanza_node(context, tag, ns, attrs);
    };
    auto make_text_el = [this](const char *tag, std::string_view text_sv,
                               const char *ns = nullptr)
        -> std::shared_ptr<xmpp_stanza_t>
    {
        return stanza_text_node(context, tag, std::string(text_sv).c_str(), ns);
    };

    auto entry = make_sp("entry", "http://www.w3.org/2005/Atom");

    // <title type='text'>
    // For comments (XEP-0277 §3.2) the body goes into <title> and there is no
    // <content> element.  For regular posts, use the explicit title when provided,
    // or fall back to the first 60 characters of the body (RFC 4287 §4.2.14 requires
    // <title> on every entry).
    {
        std::string_view title_text;
        std::string body_excerpt;
        if (is_comment)
        {
            title_text = body;
        }
        else if (!post_title.empty())
        {
            title_text = post_title;
        }
        else
        {
            // Derive title from body: up to first newline or 60 chars
            std::string_view bv(body);
            auto nl = bv.find('\n');
            body_excerpt = std::string(bv.substr(0, std::min(nl == std::string_view::npos
                                                             ? bv.size() : nl,
                                                             size_t{60})));
            title_text = body_excerpt;
        }
        auto title_el = make_sp("title", nullptr, {{"type", "text"}});
        auto t = make_text_el("", std::string(title_text));
        xmpp_stanza_add_child(title_el.get(), t.get());
        xmpp_stanza_add_child(entry.get(), title_el.get());
    }

    // <id>
    {
        auto id_el = make_text_el("id", atom_id);
        xmpp_stanza_add_child(entry.get(), id_el.get());
    }

    // <published> <updated>
    {
        auto pub_el = make_text_el("published", ts_str);
        xmpp_stanza_add_child(entry.get(), pub_el.get());
        auto upd_el = make_text_el("updated", ts_str);
        xmpp_stanza_add_child(entry.get(), upd_el.get());
    }

    // <author>
    {
        const std::string bare_jid = ::jid(nullptr, jid()).bare;
        auto author_el = make_sp("author");
        auto name_el = make_text_el("name", bare_jid.empty() ? jid() : bare_jid);
        xmpp_stanza_add_child(author_el.get(), name_el.get());
        std::string xmpp_uri = fmt::format("xmpp:{}", bare_jid);
        auto uri_el = make_text_el("uri", xmpp_uri);
        xmpp_stanza_add_child(author_el.get(), uri_el.get());
        xmpp_stanza_add_child(entry.get(), author_el.get());
    }

    // <content type='text'> — omitted for comments (body is in <title> per XEP-0277 §3.2)
    if (!is_comment)
    {
        auto content_el = make_sp("content", nullptr, {{"type", "text"}});
        auto ct = make_text_el("", body);
        xmpp_stanza_add_child(content_el.get(), ct.get());
        xmpp_stanza_add_child(entry.get(), content_el.get());
    }

    // <file-sharing xmlns='urn:xmpp:sfs:0'> for each embed (XEP-0447)
    for (const auto &emb : post.embeds)
    {
        if (!emb.uploaded()) continue;

        auto fs = make_sp("file-sharing", "urn:xmpp:sfs:0",
                          {{"disposition", emb.disposition()}});

        // <file>
        auto file_el = make_sp("file");
        {
            auto name_el = make_text_el("name", emb.filename);
            xmpp_stanza_add_child(file_el.get(), name_el.get());
        }
        if (!emb.mime.empty())
        {
            auto mt_el = make_text_el("media-type", emb.mime);
            xmpp_stanza_add_child(file_el.get(), mt_el.get());
        }
        if (emb.size > 0)
        {
            auto sz_el = make_text_el("size", fmt::format("{}", emb.size));
            xmpp_stanza_add_child(file_el.get(), sz_el.get());
        }
        if (!emb.sha256_b64.empty())
        {
            auto hash_el = make_sp("hash", "urn:xmpp:hashes:2", {{"algo", "sha-256"}});
            auto ht = make_text_el("", emb.sha256_b64);
            xmpp_stanza_add_child(hash_el.get(), ht.get());
            xmpp_stanza_add_child(file_el.get(), hash_el.get());
        }
        if (emb.width > 0)
        {
            auto w_el = make_text_el("width", fmt::format("{}", emb.width));
            xmpp_stanza_add_child(file_el.get(), w_el.get());
        }
        if (emb.height > 0)
        {
            auto h_el = make_text_el("height", fmt::format("{}", emb.height));
            xmpp_stanza_add_child(file_el.get(), h_el.get());
        }
        xmpp_stanza_add_child(fs.get(), file_el.get());

        // <sources><url-data target='…'/></sources>
        auto sources_el = make_sp("sources");
        {
            auto ud = make_sp("url-data", "http://jabber.org/protocol/url-data",
                              {{"target", emb.get_url.c_str()}});
            xmpp_stanza_add_child(sources_el.get(), ud.get());
        }
        xmpp_stanza_add_child(fs.get(), sources_el.get());

        xmpp_stanza_add_child(entry.get(), fs.get());
    }

    // <thr:in-reply-to> for replies
    if (!reply_to_id.empty())
    {
        constexpr std::string_view kCommentsPfx2 = "urn:xmpp:microblog:0:comments/";
        const std::string reply_ref_node =
            (pub_node.rfind(kCommentsPfx2, 0) == 0)
            ? "urn:xmpp:microblog:0"
            : pub_node;
        const std::string reply_feed_key = fmt::format("{}/{}", pub_service, reply_ref_node);
        const std::string reply_xmpp_uri = fmt::format(
            "xmpp:{}?;node={};item={}", pub_service, reply_ref_node, reply_to_id);
        const std::string reply_atom_id = feed_atom_id_get(reply_feed_key, reply_to_id);
        auto reply_el = make_sp("thr:in-reply-to", nullptr,
                                {{"xmlns:thr", "http://purl.org/syndication/thread/1.0"},
                                 {"ref", reply_atom_id.empty() ? reply_xmpp_uri.c_str()
                                                               : reply_atom_id.c_str()},
                                 {"href", reply_xmpp_uri.c_str()}});
        xmpp_stanza_add_child(entry.get(), reply_el.get());
    }

    // Determine target service/node
    constexpr std::string_view k_comments_pfx = "urn:xmpp:microblog:0:comments/";
    const bool is_comments_node = (pub_node.rfind(k_comments_pfx, 0) == 0);
    const std::string target_service = (!reply_target_service.empty())
        ? reply_target_service : pub_service;
    const std::string target_node    = (!reply_target_node.empty())
        ? reply_target_node    : pub_node;

    // <link rel='replies'>
    if (reply_to_id.empty() && !is_comments_node)
    {
        const std::string comments_node_name =
            fmt::format("urn:xmpp:microblog:0:comments/{}", item_uuid);
        const std::string comments_xmpp_uri = fmt::format(
            "xmpp:{}?;node={}", target_service, comments_node_name);
        auto replies_el = make_sp("link", nullptr,
                                  {{"rel",   "replies"},
                                   {"title", "comments"},
                                   {"href",  comments_xmpp_uri.c_str()}});
        xmpp_stanza_add_child(entry.get(), replies_el.get());
    }

    // <generator>
    {
        auto gen_el = make_sp("generator", nullptr,
                              {{"uri", "https://github.com/ekollof/xepher"},
                               {"version", WEECHAT_XMPP_PLUGIN_VERSION}});
        auto gt = make_text_el("", "Xepher");
        xmpp_stanza_add_child(gen_el.get(), gt.get());
        xmpp_stanza_add_child(entry.get(), gen_el.get());
    }

    // Wrap in <item id='uuid'><entry/></item> → <publish> → <pubsub> → <iq>
    // All built with shared_ptr RAII since entry is already a live stanza_t

    // <item id='uuid'> wrapping the entry
    auto item_el = make_sp("item", nullptr, {{"id", item_uuid.c_str()}});
    xmpp_stanza_add_child(item_el.get(), entry.get());

    // <publish node='...'>
    auto publish_el = make_sp("publish", nullptr, {{"node", target_node.c_str()}});
    xmpp_stanza_add_child(publish_el.get(), item_el.get());

    // <pubsub xmlns='http://jabber.org/protocol/pubsub'>
    auto pubsub_el = make_sp("pubsub", "http://jabber.org/protocol/pubsub");
    xmpp_stanza_add_child(pubsub_el.get(), publish_el.get());

    // publish-options (only when access_open)
    if (access_open)
    {
        auto x = make_sp("x", "jabber:x:data", {{"type", "submit"}});

        auto add_field = [&](std::string_view var, std::string_view val) {
            auto f = make_sp("field", nullptr, {{"var", std::string(var).c_str()}});
            auto v = make_text_el("value", val);
            xmpp_stanza_add_child(f.get(), v.get());
            xmpp_stanza_add_child(x.get(), f.get());
        };
        add_field("FORM_TYPE", "http://jabber.org/protocol/pubsub#publish-options");
        add_field("pubsub#access_model", "open");
        // XEP-0472 §5.1.1 Base profile MUST requirements for social feed nodes
        add_field("pubsub#type",          "urn:xmpp:microblog:0");
        add_field("pubsub#persist_items", "true");
        add_field("pubsub#max_items",     "max");
        add_field("pubsub#notify_retract","true");

        auto pub_opts = make_sp("publish-options");
        xmpp_stanza_add_child(pub_opts.get(), x.get());
        xmpp_stanza_add_child(pubsub_el.get(), pub_opts.get());
    }

    // <iq type='set' id='...' from='...' to='...'>
    const std::string uid = stanza::uuid(context);
    auto iq_el = make_sp("iq");
    xmpp_stanza_set_name(iq_el.get(), "iq");
    xmpp_stanza_set_attribute(iq_el.get(), "type", "set");
    xmpp_stanza_set_attribute(iq_el.get(), "id",   uid.c_str());
    xmpp_stanza_set_attribute(iq_el.get(), "from", jid().c_str());
    xmpp_stanza_set_attribute(iq_el.get(), "to",   target_service.c_str());
    xmpp_stanza_add_child(iq_el.get(), pubsub_el.get());

    connection.send(iq_el.get());

    pubsub_publish_ids[uid] = {target_service, target_node, item_uuid, post.buffer};

    if (post.is_reply)
        weechat_printf(post.buffer, "%sPosted reply to %s on %s/%s",
                       weechat_prefix("network"),
                       reply_to_id.c_str(), target_service.c_str(), target_node.c_str());
    else
        weechat_printf(post.buffer, "%sPosted to %s/%s (id: %s)",
                       weechat_prefix("network"),
                       pub_service.c_str(), pub_node.c_str(), item_uuid.c_str());
}