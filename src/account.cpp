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
#include <ranges>
#include <string_view>
#include <span>
#include <expected>
#include <fmt/core.h>
#include <fmt/ranges.h>
#include <libxml/xmlwriter.h>
#include <libxml/xmlerror.h>
#include <libxml/parser.h>
#include <weechat/weechat-plugin.h>
#include <filesystem>
#include <lmdb++.h>

#include "plugin.hh"
#include "weechat/buffer_port.hh"
#include "weechat/runtime_port.hh"
#include "xmpp/node.hh"
#include "xmpp/stanza.hh"
#include "config.hh"
#include "input.hh"
#include "omemo.hh"
#include "account.hh"
#include "connection.hh"
#include "connection/internal.hh"
#include "user.hh"
#include "nicklist.hh"
#include "channel.hh"
#include "buffer.hh"
#include "debug.hh"
#include "util.hh"
#include "weechat/ui_port.hh"
#include "xmpp/message_forward.hh"

// Use a pointer that's never freed to prevent destructor running at program exit
// when plugin is already unloaded. Memory is leaked intentionally; OS reclaims it.
std::unordered_map<std::string, weechat::account>& weechat::accounts = 
    *std::make_unique<std::unordered_map<std::string, weechat::account>>().release();

void weechat::log_emit(void *const userdata, const xmpp_log_level_t level,
                       const char *const area, const char *const msg)
{
    auto account = static_cast<weechat::account*>(userdata);

    static const char *log_level_name[4] = {"debug", "info", "warn", "error"};

    // Skip all debug logs - only show info, warnings, and errors
    if (level == XMPP_LEVEL_DEBUG)
        return;

    const char *tags = level > XMPP_LEVEL_DEBUG ? "no_log" : nullptr;

    weechat::UiPort::for_buffer(account ? account->buffer : nullptr)->printf_date_tags_network(
        0, tags ? tags : "",
        fmt::format(fmt::runtime(_("%s (%s): %s")),
                    area, log_level_name[level], msg));
}

bool weechat::account::search(weechat::account* &out,
                              const std::string name, bool casesensitive)
{
    if (name.empty())
        return false;

    if (casesensitive)
    {
        for (auto& [_, acc] : weechat::accounts)
        {
            if (weechat_strcasecmp(acc.name.data(), name.data()) == 0)
            {
                out = &acc;
                return true;
            }
        }
    }
    else if (auto it = accounts.find(name); it != accounts.end())
    {
        auto& [_, acc] = *it;
        out = &acc;
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
        auto& [_, d] = *device;
        *out = d;
        return true;
    }

    return false;
}

void weechat::account::add_device(weechat::account::device *device)
{
    if (!device || device->id == 0 || device->id > 0x7fffffffU)
    {
        weechat::UiPort::for_buffer(buffer)->printf_error(
            fmt::format("omemo: ignoring invalid cached device id {}",
                        device ? device->id : 0));
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
        weechat::UiPort::for_buffer(buffer)->printf_error(
            fmt::format("omemo: refusing to publish legacy devicelist: invalid local device id {}",
                        omemo.device_id));
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

    // Standard pubsub publish-options (open access, persistent, max items)
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

    weechat::UiPort::for_buffer(buffer)->printf_network(
        fmt::format("omemo: publishing legacy devicelist for device {} ({} total device(s)) with open access model",
                    omemo.device_id, cached_ids.size() + 1));

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
        auto& [_, q] = *mam_query;
        *out = q;  // copy the query data
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

bool weechat::account::try_acquire_mam_slot()
{
    int max_c = weechat::config::instance
        ? weechat::config::instance->look.mam_max_concurrent.integer()
        : 4;
    if (mam_inflight >= max_c)
        return false;
    mam_inflight++;
    return true;
}

void weechat::account::release_mam_slot()
{
    if (mam_inflight > 0)
        mam_inflight--;
    // Wake up any deferred work (including both RSM pages and initial fetches
    // that were blocked by the concurrency limit).
    if (!mam_deferred_pages.empty())
        schedule_next_mam_page();
}

void weechat::account::schedule_next_mam_page()
{
    if (!mam_defer_timer)
        mam_defer_timer = (struct t_hook *)weechat_hook_timer(
            1, 0, 0, &account::process_deferred_mam_page_cb,
            this, nullptr);
}

int weechat::account::process_deferred_mam_page_cb(const void *pointer, void *data, int remaining_calls)
{
    (void)data;
    (void)remaining_calls;
    auto *acct = const_cast<account *>(static_cast<const account *>(pointer));
    if (!acct || acct->mam_deferred_pages.empty()) {
        if (acct && acct->mam_defer_timer) {
            weechat_unhook(acct->mam_defer_timer);
            acct->mam_defer_timer = nullptr;
        }
        return WEECHAT_RC_OK;
    }

    auto page = acct->mam_deferred_pages.front();
    acct->mam_deferred_pages.pop_front();

    // #2: Small randomized delay for initial (non-RSM) per-room MAM requests.
    // This smooths out the burst when many rooms join at once during initial
    // connect or bulk bookmark processing. RSM continuation pages are never
    // delayed.
    if (page.after.empty() && !page.channel_id.empty() && !acct->mam_jitter_next_initial)
    {
        // First time we see this initial room request in the processor → apply jitter.
        acct->mam_jitter_next_initial = true;

        int jitter_ms = 50 + (rand() % 250);

        // Re-queue it. On the next timer firing (after jitter) we will let it through.
        acct->mam_deferred_pages.push_front(std::move(page));

        if (!acct->mam_defer_timer)
        {
            acct->mam_defer_timer = (struct t_hook *)weechat_hook_timer(
                jitter_ms, 0, 0, &account::process_deferred_mam_page_cb,
                acct, nullptr);
        }
        return WEECHAT_RC_OK;
    }

    // If we get here with an initial room request, it means we just applied jitter
    // on the previous timer firing. Clear the flag and process it normally now.
    if (page.after.empty() && !page.channel_id.empty())
    {
        acct->mam_jitter_next_initial = false;
    }

    if (page.channel_id.empty())
    {
        if (page.after.empty())
        {
            weechat::UiPort::for_buffer(acct->buffer)->printf_error(
                "xmpp: deferred global MAM page has no <after> token — skipping");
            return WEECHAT_RC_OK;
        }

        std::string next_id = stanza::uuid(acct->context);
        acct->add_mam_query(next_id.c_str(), "",
                           page.start, page.end);

        stanza::xep0313::query next_q;
        if (page.start)
        {
            stanza::xep0313::x_filter xf;
            xf.start(format_utc_timestamp(*page.start));
            next_q.filter(xf);
        }
        stanza::xep0059::set rsm_after;
        rsm_after.max(50).after(page.after);
        next_q.rsm(rsm_after);

        acct->connection.send(stanza::iq()
            .type("set")
            .id(next_id)
            .xep0313()
            .query(next_q)
            .build(acct->context)
            .get());
    }
    else
    {
        if (auto channel = acct->channels.find(page.channel_id); channel != acct->channels.end())
        {
            auto& [_, ch] = *channel;
            time_t *start_ptr = page.start ? &*page.start : nullptr;
            time_t *end_ptr   = page.end   ? &*page.end   : nullptr;
            ch.fetch_mam(
                page.mam_query_id.empty() ? stanza::uuid(acct->context).c_str() : page.mam_query_id.c_str(),
                start_ptr, end_ptr,
                page.after.empty() ? nullptr : page.after.c_str());
        }
        else
        {
            XDEBUG("deferred MAM page for channel {} (no longer exists) — discarded", page.channel_id);
        }
    }

    if (acct->mam_deferred_pages.empty() && acct->mam_defer_timer)
    {
        weechat_unhook(acct->mam_defer_timer);
        acct->mam_defer_timer = nullptr;
    }

    return WEECHAT_RC_OK;
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

std::string weechat::account::resolve_channel_key(std::string_view partner_bare) const
{
    if (partner_bare.empty())
        return {};

    const std::string partner(partner_bare);
    if (channels.contains(partner))
    {
        channel_key_cache_[::xmpp::bare_jid_fold_key(partner_bare)] = partner;
        return partner;
    }

    const std::string folded = ::xmpp::bare_jid_fold_key(partner_bare);
    if (auto cit = channel_key_cache_.find(folded); cit != channel_key_cache_.end())
    {
        if (channels.contains(cit->second))
            return cit->second;
    }

    if (auto match = std::ranges::find_if(channels, [&](const auto &entry) {
            const auto &[key, _] = entry;
            return ::xmpp::bare_jid_iequals(key, partner_bare);
        });
        match != channels.end())
    {
        const auto &[key, _] = *match;
        channel_key_cache_[folded] = key;
        return key;
    }
    return partner;
}

void weechat::account::invalidate_channel_key_cache(std::string_view canonical_key)
{
    if (canonical_key.empty())
        return;

    std::erase_if(channel_key_cache_, [&](const auto &entry) {
        const auto &[_, key] = entry;
        return key == canonical_key;
    });
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
    // Block conn_handler re-entry before any member teardown. ~connection
    // (later) releases xmpp_conn_t and libstrophe invokes disconnect while
    // later-declared fields (e.g. muc_modes_fetched) are already destroyed.
    tearing_down_ = true;

    // Safety check: if plugin is destroyed, skip WeeChat/plugin cleanup
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
    channel_key_cache_.clear();
    channels.clear();

    mam_cache_cleanup();
}

void weechat::account::disconnect(int reconnect)
{
    disconnect_impl(reconnect, false);
}

void weechat::account::disconnect_reconnect_immediate()
{
    disconnect_impl(true, true);
}

void weechat::account::disconnect_impl(int reconnect, bool immediate_reconnect)
{
    // Safety check: if plugin is destroyed, don't call weechat functions
    if (!weechat::plugin::instance || !weechat::plugin::instance->ptr())
        return;

    stream_features_sniff_restore(connection);

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
    
    for (auto &h : csi_activity_hooks)
    {
        if (h)
        {
            weechat_unhook(h);
            h = nullptr;
        }
    }
    
    // XEP-0198 §4: final ack before graceful stream close (best-effort).
    if (is_connected && connection && sm_enabled)
        connection.send_sm_graceful_ack();

    // Clean up Stream Management hooks
    if (sm_ack_timer_hook)
    {
        weechat_unhook(sm_ack_timer_hook);
        sm_ack_timer_hook = nullptr;
    }

    if (connect_disco_summary_timer_hook_)
    {
        weechat_unhook(connect_disco_summary_timer_hook_);
        connect_disco_summary_timer_hook_ = nullptr;
    }

    sm_awaiting_negotiation = false;
    sm_resume_attempted = false;
    sm_pending_replay.clear();
    
    // Clean up deferred MAM page timer and queue
    if (mam_defer_timer)
    {
        weechat_unhook(mam_defer_timer);
        mam_defer_timer = nullptr;
    }
    mam_deferred_pages.clear();
    mam_inflight = 0;
    mam_jitter_next_initial = false;

    // XEP-0045 room mode disco#info: drop in-flight queries and the
    // "already fetched" cache so a fresh /enter re-fetches on reconnect.
    muc_modes_queries.clear();
    muc_modes_fetched.clear();

    // XEP-0045 §10: drop in-flight muc#owner / muc#admin IQ tracking.
    muc_owner_queries.clear();

    clear_server_capability_snapshot();

    // XEP-0045 §10.1: clear any pending reserved-room create intents so a
    // fresh /create on reconnect starts clean.
    muc_reserved_pending.clear();

    // XEP-0045 §7.8.2: drop pending mediated invites on disconnect.
    pending_mediated_invites.clear();

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
            weechat::BufferPort::default_port_ref().nicklist_remove_all(buffer);
        for (auto& [_, ch] : channels)
        {
            if (ch.buffer)
            {
                weechat::BufferPort::default_port_ref().nicklist_remove_all(ch.buffer);
                weechat::UiPort::for_buffer(ch.buffer)->printf_network(
                    fmt::format("{}: disconnected from account", WEECHAT_XMPP_PLUGIN_NAME));
            }
        }
    }

    reset();

    if (buffer)
    {
        weechat::UiPort::for_buffer(buffer)->printf_network(
            fmt::format("{}: disconnected from account", WEECHAT_XMPP_PLUGIN_NAME));
    }

    // On manual disconnect, close all channel buffers (PM, MUC, FEED)
    // and the account buffer. Channel destructors persist closed state to
    // LMDB for proper restore on /account connect. Do this AFTER reset()
    // so the XMPP connection is already dead — buffer__close_cb checks
    // connected() and skips sending <unavailable>/<gone> stanzas when
    // already disconnected, and the account buffer close callback skips
    // calling disconnect() again (avoiding recursion).
    if (!reconnect)
    {
        // XEP-0198: Clear SM state on manual disconnect — user explicitly
        // ended the session, so don't attempt resumption on next connect.
        sm_id.clear();
        sm_h_inbound = 0;
        sm_h_outbound = 0;
        sm_last_ack = 0;
        sm_outqueue.clear();
        sm_enabled = false;
        std::vector<struct t_gui_buffer *> to_close;
        for (auto &[name, ch] : channels)
            if (ch.buffer)
                to_close.push_back(ch.buffer);
        for (auto *buf : to_close)
            weechat_buffer_close(buf);

        if (buffer)
            weechat_buffer_close(buffer);
    }

    if (reconnect)
    {
        if (immediate_reconnect)
        {
            reconnect_delay = 0;
            reconnect_start = time(nullptr) - 1;
            weechat::UiPort::for_buffer(buffer)->printf_network(
                "xmpp: reconnecting immediately…");
        }
        else
        {
            // Exponential backoff: 5 → 10 → 20 → 40 → 80 → 120s (capped)
            if (reconnect_delay == 0)
                reconnect_delay = 5;
            else
                reconnect_delay = std::min(reconnect_delay * 2, 120);
            current_retry++;
            reconnect_start = time(nullptr) + reconnect_delay;
            weechat::UiPort::for_buffer(buffer)->printf_network(
                fmt::format("xmpp: reconnecting in {}s (attempt {})…",
                            reconnect_delay, current_retry));
        }
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
    for (auto& [_, acc] : accounts)
    {
        acc.disconnect(0);
    }
}

bool weechat::account::roster_bare_jid_online(std::string_view bare_jid) const
{
    if (bare_jid.empty())
        return false;

    return std::ranges::any_of(users, [&](const auto& entry) {
        const auto& [id, user] = entry;
        return user.is_online && ::jid(nullptr, id).bare == bare_jid;
    });
}

void weechat::account::track_pending_mediated_invite(
    std::string_view room_jid,
    std::string_view inviter_bare,
    std::optional<std::string_view> password)
{
    pending_mediated_invite pending;
    pending.room_jid = room_jid;
    pending.inviter_bare = inviter_bare;
    if (password && !password->empty())
        pending.password = std::string(*password);
    std::erase_if(pending_mediated_invites,
                  [&](const auto& p) {
                      return p.room_jid == pending.room_jid
                          && p.inviter_bare == pending.inviter_bare;
                  });
    pending_mediated_invites.push_back(std::move(pending));
}

std::optional<std::size_t> weechat::account::find_pending_mediated_invite(
    std::string_view room_jid,
    std::string_view inviter_bare) const
{
    for (std::size_t i = 0; i < pending_mediated_invites.size(); ++i)
    {
        const auto& pending = pending_mediated_invites[i];
        if (pending.room_jid == room_jid && pending.inviter_bare == inviter_bare)
            return i;
    }
    return std::nullopt;
}

void weechat::account::erase_pending_mediated_invite(std::size_t index)
{
    if (index >= pending_mediated_invites.size())
        return;
    pending_mediated_invites.erase(
        pending_mediated_invites.begin() + static_cast<std::ptrdiff_t>(index));
}

void weechat::account::update_roster_nicklist_entry(std::string_view bare_jid)
{
    if (bare_jid.empty() || !buffer)
        return;

    auto roster_it = roster.find(std::string(bare_jid));
    if (roster_it == roster.end() || roster_it->second.subscription == "none")
        return;

    const bool online = roster_bare_jid_online(bare_jid);
    bool away = false;
    if (online)
    {
        for (const auto& [id, u] : users)
        {
            if (u.is_online && u.is_away && ::jid(nullptr, id).bare == bare_jid)
            {
                away = true;
                break;
            }
        }
    }

    weechat::user *roster_user = user::search(this, bare_jid);
    if (!roster_user)
    {
        const std::string display = roster_it->second.name.empty()
            ? std::string(bare_jid)
            : roster_it->second.name;
        try
        {
            auto [it_u, _ins] = users.emplace(
                std::piecewise_construct,
                std::forward_as_tuple(std::string(bare_jid)),
                std::forward_as_tuple(this, nullptr, bare_jid, display));
            roster_user = &it_u->second;
        }
        catch (const std::invalid_argument&)
        {
            roster_user = user::search(this, bare_jid);
            if (!roster_user)
                return;
        }
    }

    if (!roster_it->second.name.empty())
        roster_user->profile.display_name = roster_it->second.name;

    roster_user->is_online = online;
    roster_user->is_away = away;
    roster_user->nicklist_remove(this, nullptr);
    roster_user->nicklist_add(this, nullptr);
}

void weechat::account::sync_roster_nicklist()
{
    for (const auto& [jid, _] : roster)
        update_roster_nicklist_entry(jid);
}

void weechat::account::count_roster_nicklist_presence(int &online,
                                                      int &offline) const
{
    online = 0;
    offline = 0;

    for (const auto& [jid, item] : roster)
    {
        if (item.subscription == "none")
            continue;

        const weechat::user *roster_user = user::search(
            const_cast<account *>(this), jid.c_str());
        if (!roster_user)
            continue;

        if (roster_user->is_online)
            ++online;
        else
            ++offline;
    }
}

struct t_gui_buffer *weechat::account::create_buffer()
{
    buffer = weechat_buffer_new(fmt::format("account.{}", name).data(),
                                &input__data_cb, nullptr, nullptr,
                                &buffer__close_cb, nullptr, nullptr);
    if (!buffer)
        return nullptr;
    weechat::UiPort::for_buffer(buffer)->printf(fmt::format("xmpp: {}", name));

    if (!weechat_buffer_get_integer(buffer, "short_name_is_set"))
        weechat_buffer_set(buffer, "short_name", name.data());
    weechat_buffer_set(buffer, "localvar_set_type", "server");
    weechat_buffer_set(buffer, "localvar_set_account", name.data());
    weechat_buffer_set(buffer, "localvar_set_server", name.data());
    weechat_buffer_set(buffer, "localvar_set_charset_modifier",
                       fmt::format("account.{}", name).data());
    weechat_buffer_set(buffer, "title", name.data());

    weechat_buffer_set(buffer, "nicklist", "1");
    weechat_buffer_set(buffer, "nicklist_display_groups", "0");
    nicklist::ensure_account_groups(buffer);
    weechat_buffer_set_pointer(buffer, "nicklist_callback",
                               (void*)&buffer__nickcmp_cb);
    weechat_buffer_set_pointer(buffer, "nicklist_callback_pointer",
                               this);
    weechat_buffer_set_pointer(buffer, XMPP_BUFFER_ACCOUNT_PTR, this);

    return buffer;
}

void weechat::account::reset()
{
    if (connection)
    {
        if (xmpp_conn_is_connected(connection) || xmpp_conn_is_connecting(connection))
            xmpp_disconnect(connection);
    }

    is_connected = 0;
}

int weechat::account::connect()
{
    return connect(false);  // Not a manual connect
}

int weechat::account::connect([[maybe_unused]] bool manual)
{
    if (!buffer)
    {
        if (!create_buffer())
            return 0;
    }

    reset();

    last_top_level_ext_sent = last_top_level_ext::none;

    // Reset SM availability on any connect attempt, but honour per-server LMDB
    // caps learned from prior unsupported-stanza-type downgrades.
    sm_available = true;
    csi_available = true;
    {
        ::jid parsed(nullptr, std::string(option_jid.string()));
        if (!parsed.domain.empty())
        {
            const auto cached = stream_ext_cache_get(parsed.domain);
            if (cached.sm && !*cached.sm)
                sm_available = false;
            if (cached.csi && !*cached.csi)
                csi_available = false;
        }
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

        for (auto& [_, acc] : accounts)
        {
            // Drain completed uploads that finished while the main thread
            // was blocked (e.g. Python hook deadlocks). The upload worker
            // thread writes to a pipe, and weechat_hook_fd may not fire if
            // the event loop is stalled. Check directly from the timer.
            for (auto& [rd_fd, ctx] : acc.pending_uploads)
            {
                if (ctx->worker_done.exchange(false))
                {
                    char sig[1];
                    (void)::read(rd_fd, sig, sizeof(sig));
                    if (ctx->worker.joinable())
                        ctx->worker.join();
                    acc.upload_fd_cb(nullptr, &acc, rd_fd);
                }
            }

            if (acc.is_connected
                && (xmpp_conn_is_connecting(acc.connection)
                    || xmpp_conn_is_connected(acc.connection)))
                acc.connection.process(acc.context, 10);
            else if (acc.disconnected);
            else if (acc.reconnect_start > 0
                     && acc.reconnect_start < time(nullptr))
            {
                // Clear reconnect_start BEFORE calling connect() so that a
                // failed connect() does not cause an immediate re-fire on the
                // next timer tick. The next disconnect() call will set a new
                // (longer) reconnect_start via the exponential backoff.
                acc.reconnect_start = 0;
                acc.connect();
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
    for (auto& [_, channel] : channels)
    {
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
            if (auto channel_it = channels.find(jid); channel_it != channels.end())
            {
                auto& [_, ch] = *channel_it;
                ch.pgp.ids.emplace(keyid);
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
    std::ranges::for_each(path, [](char &c) { if (c == ':') c = '-'; });

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

    // Build Atom <entry> via fluent XEP-0277 builder.
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
        // Derive title from body: up to first newline or 60 chars (RFC 4287 §4.2.14).
        std::string_view bv(body);
        auto nl = bv.find('\n');
        body_excerpt = std::string(bv.substr(0, std::min(nl == std::string_view::npos
                                                         ? bv.size() : nl,
                                                         size_t{60})));
        title_text = body_excerpt;
    }

    const std::string bare_jid = ::jid(nullptr, jid()).bare;
    const std::string author_name = bare_jid.empty() ? jid() : bare_jid;
    const std::string author_uri = fmt::format("xmpp:{}", bare_jid);

    stanza::xep0277::entry entry_spec;
    entry_spec.title_text(title_text)
        .atom_id(atom_id)
        .published(ts_str)
        .updated(ts_str)
        .author(author_name, author_uri);

    // <content type='text'> — omitted for comments (body is in <title> per XEP-0277 §3.2)
    if (!is_comment)
        entry_spec.content_text(body);

    // <file-sharing xmlns='urn:xmpp:sfs:0'> for each embed (XEP-0447)
    for (const auto &emb : post.embeds)
    {
        if (!emb.uploaded()) continue;

        auto file_spec = stanza::xep0447::file()
            .name(emb.filename);
        if (!emb.mime.empty())
            file_spec.media_type(emb.mime);
        if (emb.size > 0)
            file_spec.size(emb.size);
        for (const auto& [algo, b64] : emb.hashes)
            file_spec.add_hash(stanza::xep0447::hash(algo, b64));
        if (emb.width > 0)
            file_spec.width(emb.width);
        if (emb.height > 0)
            file_spec.height(emb.height);
        if (!emb.file_date.empty())
            file_spec.date(emb.file_date);

        auto sources_spec = stanza::xep0447::sources()
            .add(stanza::xep0447::url_data(emb.get_url));

        entry_spec.file_sharing(stanza::xep0447::file_sharing()
            .disposition(emb.disposition())
            .file(file_spec)
            .sources(sources_spec));
    }

    // <thr:in-reply-to> for replies
    if (!reply_to_id.empty())
    {
        constexpr std::string_view kCommentsPfx2 = "urn:xmpp:microblog:0:comments/";
        const std::string reply_ref_node =
            (pub_node.starts_with(kCommentsPfx2))
            ? "urn:xmpp:microblog:0"
            : pub_node;
        const std::string reply_feed_key = fmt::format("{}/{}", pub_service, reply_ref_node);
        const std::string reply_xmpp_uri = fmt::format(
            "xmpp:{}?;node={};item={}", pub_service, reply_ref_node, reply_to_id);
        const std::string reply_atom_id = feed_atom_id_get(reply_feed_key, reply_to_id);
        entry_spec.in_reply_to(
            reply_atom_id.empty() ? reply_xmpp_uri : reply_atom_id,
            reply_xmpp_uri);
    }

    // Determine target service/node
    constexpr std::string_view k_comments_pfx = "urn:xmpp:microblog:0:comments/";
    const bool is_comments_node = (pub_node.starts_with(k_comments_pfx));
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
        entry_spec.link("replies", comments_xmpp_uri, "comments");
    }

    entry_spec.generator("Xepher", "https://github.com/ekollof/xepher",
                         weechat::plugin_version());

    auto pubsub_el = stanza::xep0060::pubsub()
        .publish(stanza::xep0060::publish(target_node)
            .item(stanza::xep0060::item().id(item_uuid).payload(entry_spec)));

    // publish-options (only when access_open)
    if (access_open)
    {
        auto form = stanza::xep0004::form("submit")
            .add_hidden("FORM_TYPE",
                        "http://jabber.org/protocol/pubsub#publish-options")
            .add_text("pubsub#access_model", "open")
            // XEP-0472 §5.1.1 Base profile MUST requirements for social feed nodes
            .add_text("pubsub#type",          "urn:xmpp:microblog:0")
            .add_text("pubsub#persist_items", "true")
            .add_text("pubsub#max_items",     "max")
            .add_text("pubsub#notify_retract","true");

        pubsub_el.publish_options(stanza::xep0060::publish_options()
            .child_spec(form));
    }

    const std::string uid = stanza::uuid(context);
    auto iq_s = stanza::iq()
        .type("set")
        .id(uid)
        .from(this->jid().c_str())
        .to(target_service)
        .pubsub(pubsub_el);

    connection.send(iq_s.build(context).get());

    pubsub_publish_ids[uid] = {target_service, target_node, item_uuid, post.buffer};

    if (post.is_reply)
        weechat::UiPort::for_buffer(post.buffer)->printf_network(
            fmt::format("Posted reply to {} on {}/{}",
                        reply_to_id, target_service, target_node));
    else
        weechat::UiPort::for_buffer(post.buffer)->printf_network(
            fmt::format("Posted to {}/{} (id: {})",
                        pub_service, pub_node, item_uuid));
}