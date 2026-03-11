// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#ifdef __cplusplus
#include <cstdlib>
#include <string>
#include <strophe.h>
#endif

// Stack-allocated RAII string wrapper replacing the old malloc-based t_string.
// with_noop / with_free / with_xmpp_free construct the value; callers pass
// by value (cheap move) and functions consume via .value.c_str().
struct t_string
{
    std::string value;
};

static inline t_string with_noop(const char *const v)
{ return {v ? v : ""}; }

static inline t_string with_free(char *v)
{
    t_string s{v ? v : ""};
    free(v);
    return s;
}

static inline t_string with_xmpp_free(char *v, xmpp_ctx_t *ctx)
{
    t_string s{v ? v : ""};
    xmpp_free(ctx, v);
    return s;
}

static inline void stanza__set_text(xmpp_ctx_t *context, xmpp_stanza_t *parent,
                                    t_string value)
{
    xmpp_stanza_t *text = xmpp_stanza_new(context);
    xmpp_stanza_set_text(text, value.value.c_str());
    xmpp_stanza_add_child(parent, text);
    xmpp_stanza_release(text);
}

xmpp_stanza_t *stanza__presence(xmpp_ctx_t *context, xmpp_stanza_t *base,
                                xmpp_stanza_t **children, const char *ns,
                                const char *from, const char *to, const char *type);

xmpp_stanza_t *stanza__iq(xmpp_ctx_t *context, xmpp_stanza_t *base,
                          xmpp_stanza_t **children, const char *ns, const char *id,
                          const char *from, const char *to, const char *type);

xmpp_stanza_t *stanza__iq_pubsub(xmpp_ctx_t *context, xmpp_stanza_t *base,
                                 xmpp_stanza_t **children, t_string ns);

xmpp_stanza_t *stanza__iq_pubsub_items(xmpp_ctx_t *context, xmpp_stanza_t *base, const char *node);

xmpp_stanza_t *stanza__iq_pubsub_items_item(xmpp_ctx_t *context, xmpp_stanza_t *base,
                                            t_string id);

xmpp_stanza_t *stanza__iq_pubsub_subscribe(xmpp_ctx_t *context, xmpp_stanza_t *base,
                                           t_string node, t_string jid);

xmpp_stanza_t *stanza__iq_pubsub_publish(xmpp_ctx_t *context, xmpp_stanza_t *base,
                                         xmpp_stanza_t **children, t_string node);

xmpp_stanza_t *stanza__iq_pubsub_publish_item(xmpp_ctx_t *context, xmpp_stanza_t *base,
                                              xmpp_stanza_t **children, t_string id);

xmpp_stanza_t *stanza__iq_pubsub_publish_item_list(xmpp_ctx_t *context, xmpp_stanza_t *base,
                                                   xmpp_stanza_t **children, t_string ns);

xmpp_stanza_t *stanza__iq_pubsub_publish_item_list_device(xmpp_ctx_t *context, xmpp_stanza_t *base,
                                                          t_string id, t_string label);

xmpp_stanza_t *stanza__iq_pubsub_publish_item_bundle(xmpp_ctx_t *context, xmpp_stanza_t *base,
                                                     xmpp_stanza_t **children, t_string ns);

xmpp_stanza_t *stanza__iq_pubsub_publish_item_bundle_signedPreKeyPublic(
    xmpp_ctx_t *context, xmpp_stanza_t *base, xmpp_stanza_t **children, t_string signedPreKeyId);

xmpp_stanza_t *stanza__iq_pubsub_publish_item_bundle_signedPreKeySignature(
    xmpp_ctx_t *context, xmpp_stanza_t *base, xmpp_stanza_t **children);

xmpp_stanza_t *stanza__iq_pubsub_publish_item_bundle_identityKey(
    xmpp_ctx_t *context, xmpp_stanza_t *base, xmpp_stanza_t **children);

xmpp_stanza_t *stanza__iq_pubsub_publish_item_bundle_prekeys(
    xmpp_ctx_t *context, xmpp_stanza_t *base, xmpp_stanza_t **children);

xmpp_stanza_t *stanza__iq_pubsub_publish_item_bundle_prekeys_preKeyPublic(
    xmpp_ctx_t *context, xmpp_stanza_t *base, xmpp_stanza_t **children, t_string preKeyId);

xmpp_stanza_t *stanza__iq_enable(xmpp_ctx_t *context, xmpp_stanza_t *base,
                                 t_string ns);

xmpp_stanza_t *stanza__iq_ping(xmpp_ctx_t *context, xmpp_stanza_t *base,
                               t_string ns);

xmpp_stanza_t *stanza__iq_query(xmpp_ctx_t *context, xmpp_stanza_t *base,
                                t_string ns, t_string node);
