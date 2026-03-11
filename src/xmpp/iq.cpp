// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <cstdlib>
#include <strophe.h>

#include "stanza.hh"

xmpp_stanza_t *stanza__iq(xmpp_ctx_t *context, xmpp_stanza_t *base,
                          xmpp_stanza_t **children, const char *ns, const char *id,
                          const char *from, const char *to, const char *type)
{
    xmpp_stanza_t *parent = base ? base : xmpp_iq_new(context, type, id);
    xmpp_stanza_t **child = children;

    if (ns)
        xmpp_stanza_set_ns(parent, ns);

    if (base && id)
        xmpp_stanza_set_id(parent, id);

    if (from)
        xmpp_stanza_set_from(parent, from);

    if (to)
        xmpp_stanza_set_to(parent, to);

    if (base && type)
        xmpp_stanza_set_attribute(parent, "type", type);

    while (*child)
    {
        xmpp_stanza_add_child(parent, *child);
        xmpp_stanza_release(*child);

        ++child;
    }

    return parent;
}

xmpp_stanza_t *stanza__iq_pubsub(xmpp_ctx_t *context, xmpp_stanza_t *base,
                                 xmpp_stanza_t **children, t_string ns)
{
    xmpp_stanza_t *parent = base;
    xmpp_stanza_t **child = children;

    if (!parent)
    {
        parent = xmpp_stanza_new(context);
        xmpp_stanza_set_name(parent, "pubsub");
    }

    if (!ns.value.empty())
        xmpp_stanza_set_ns(parent, ns.value.c_str());

    while (*child)
    {
        xmpp_stanza_add_child(parent, *child);
        xmpp_stanza_release(*child);

        ++child;
    }

    return parent;
}

xmpp_stanza_t *stanza__iq_pubsub_items(xmpp_ctx_t *context, xmpp_stanza_t *base, const char *node)
{
    xmpp_stanza_t *parent = base;

    if (!parent)
    {
        parent = xmpp_stanza_new(context);
        xmpp_stanza_set_name(parent, "items");
    }

    if (node)
        xmpp_stanza_set_attribute(parent, "node", node);

    return parent;
}

xmpp_stanza_t *stanza__iq_pubsub_items_item(xmpp_ctx_t *context, xmpp_stanza_t *base,
                                            t_string id)
{
    xmpp_stanza_t *parent = base;

    if (!parent)
    {
        parent = xmpp_stanza_new(context);
        xmpp_stanza_set_name(parent, "item");
    }

    if (!id.value.empty())
        xmpp_stanza_set_id(parent, id.value.c_str());

    return parent;
}

xmpp_stanza_t *stanza__iq_pubsub_subscribe(xmpp_ctx_t *context, xmpp_stanza_t *base,
                                           t_string node, t_string jid)
{
    xmpp_stanza_t *parent = base;

    if (!parent)
    {
        parent = xmpp_stanza_new(context);
        xmpp_stanza_set_name(parent, "subscribe");
    }

    if (!node.value.empty())
        xmpp_stanza_set_attribute(parent, "node", node.value.c_str());

    if (!jid.value.empty())
        xmpp_stanza_set_attribute(parent, "jid", jid.value.c_str());

    return parent;
}

xmpp_stanza_t *stanza__iq_pubsub_publish(xmpp_ctx_t *context, xmpp_stanza_t *base,
                                         xmpp_stanza_t **children, t_string node)
{
    xmpp_stanza_t *parent = base;
    xmpp_stanza_t **child = children;

    if (!parent)
    {
        parent = xmpp_stanza_new(context);
        xmpp_stanza_set_name(parent, "publish");
    }

    if (!node.value.empty())
        xmpp_stanza_set_attribute(parent, "node", node.value.c_str());

    while (*child)
    {
        xmpp_stanza_add_child(parent, *child);
        xmpp_stanza_release(*child);

        ++child;
    }

    return parent;
}

xmpp_stanza_t *stanza__iq_pubsub_publish_item(xmpp_ctx_t *context, xmpp_stanza_t *base,
                                              xmpp_stanza_t **children, t_string id)
{
    xmpp_stanza_t *parent = base;
    xmpp_stanza_t **child = children;

    if (!parent)
    {
        parent = xmpp_stanza_new(context);
        xmpp_stanza_set_name(parent, "item");
    }

    if (!id.value.empty())
        xmpp_stanza_set_id(parent, id.value.c_str());

    while (*child)
    {
        xmpp_stanza_add_child(parent, *child);
        xmpp_stanza_release(*child);

        ++child;
    }

    return parent;
}

xmpp_stanza_t *stanza__iq_pubsub_publish_item_list(xmpp_ctx_t *context, xmpp_stanza_t *base,
                                                    xmpp_stanza_t **children, t_string ns)
{
    xmpp_stanza_t *parent = base;
    xmpp_stanza_t **child = children;

    if (!parent)
    {
        parent = xmpp_stanza_new(context);
        xmpp_stanza_set_name(parent, "devices");
    }

    if (!ns.value.empty())
        xmpp_stanza_set_ns(parent, ns.value.c_str());

    while (*child)
    {
        xmpp_stanza_add_child(parent, *child);
        xmpp_stanza_release(*child++);
    }

    return parent;
}

xmpp_stanza_t *stanza__iq_pubsub_publish_item_list_device(xmpp_ctx_t *context, xmpp_stanza_t *base,
                                                          t_string id, t_string label)
{
    xmpp_stanza_t *parent = base;

    if (!parent)
    {
        parent = xmpp_stanza_new(context);
        xmpp_stanza_set_name(parent, "device");
    }

    if (!id.value.empty())
        xmpp_stanza_set_id(parent, id.value.c_str());

    if (!label.value.empty())
        xmpp_stanza_set_attribute(parent, "label", label.value.c_str());

    return parent;
}

xmpp_stanza_t *stanza__iq_pubsub_publish_item_bundle(xmpp_ctx_t *context, xmpp_stanza_t *base,
                                                     xmpp_stanza_t **children, t_string ns)
{
    xmpp_stanza_t *parent = base;
    xmpp_stanza_t **child = children;

    if (!parent)
    {
        parent = xmpp_stanza_new(context);
        xmpp_stanza_set_name(parent, "bundle");
    }

    if (!ns.value.empty())
        xmpp_stanza_set_ns(parent, ns.value.c_str());

    while (child && *child)
    {
        xmpp_stanza_add_child(parent, *child);
        xmpp_stanza_release(*child++);
    }

    return parent;
}

xmpp_stanza_t *stanza__iq_pubsub_publish_item_bundle_signedPreKeyPublic(
    xmpp_ctx_t *context, xmpp_stanza_t *base, xmpp_stanza_t **children, t_string signedPreKeyId)
{
    xmpp_stanza_t *parent = base;
    xmpp_stanza_t **child = children;

    if (!parent)
    {
        parent = xmpp_stanza_new(context);
        xmpp_stanza_set_name(parent, "spk");
    }

    if (!signedPreKeyId.value.empty())
        xmpp_stanza_set_attribute(parent, "id", signedPreKeyId.value.c_str());

    while (child && *child)
    {
        xmpp_stanza_add_child(parent, *child);
        xmpp_stanza_release(*child++);
    }

    return parent;
}

xmpp_stanza_t *stanza__iq_pubsub_publish_item_bundle_signedPreKeySignature(
    xmpp_ctx_t *context, xmpp_stanza_t *base, xmpp_stanza_t **children)
{
    xmpp_stanza_t *parent = base;
    xmpp_stanza_t **child = children;

    if (!parent)
    {
        parent = xmpp_stanza_new(context);
        xmpp_stanza_set_name(parent, "spks");
    }

    while (child && *child)
    {
        xmpp_stanza_add_child(parent, *child);
        xmpp_stanza_release(*child++);
    }

    return parent;
}

xmpp_stanza_t *stanza__iq_pubsub_publish_item_bundle_identityKey(
    xmpp_ctx_t *context, xmpp_stanza_t *base, xmpp_stanza_t **children)
{
    xmpp_stanza_t *parent = base;
    xmpp_stanza_t **child = children;

    if (!parent)
    {
        parent = xmpp_stanza_new(context);
        xmpp_stanza_set_name(parent, "ik");
    }

    while (child && *child)
    {
        xmpp_stanza_add_child(parent, *child);
        xmpp_stanza_release(*child++);
    }

    return parent;
}

xmpp_stanza_t *stanza__iq_pubsub_publish_item_bundle_prekeys(
    xmpp_ctx_t *context, xmpp_stanza_t *base, xmpp_stanza_t **children)
{
    xmpp_stanza_t *parent = base;
    xmpp_stanza_t **child = children;

    if (!parent)
    {
        parent = xmpp_stanza_new(context);
        xmpp_stanza_set_name(parent, "prekeys");
    }

    while (child && *child)
    {
        xmpp_stanza_add_child(parent, *child);
        xmpp_stanza_release(*child++);
    }

    return parent;
}

xmpp_stanza_t *stanza__iq_pubsub_publish_item_bundle_prekeys_preKeyPublic(
    xmpp_ctx_t *context, xmpp_stanza_t *base, xmpp_stanza_t **children, t_string preKeyId)
{
    xmpp_stanza_t *parent = base;
    xmpp_stanza_t **child = children;

    if (!parent)
    {
        parent = xmpp_stanza_new(context);
        xmpp_stanza_set_name(parent, "pk");
    }

    if (!preKeyId.value.empty())
        xmpp_stanza_set_attribute(parent, "id", preKeyId.value.c_str());

    while (child && *child)
    {
        xmpp_stanza_add_child(parent, *child);
        xmpp_stanza_release(*child++);
    }

    return parent;
}

xmpp_stanza_t *stanza__iq_enable(xmpp_ctx_t *context, xmpp_stanza_t *base,
                                 t_string ns)
{
    xmpp_stanza_t *parent = base;

    if (!parent)
    {
        parent = xmpp_stanza_new(context);
        xmpp_stanza_set_name(parent, "enable");
    }

    if (!ns.value.empty())
        xmpp_stanza_set_ns(parent, ns.value.c_str());

    return parent;
}

xmpp_stanza_t *stanza__iq_ping(xmpp_ctx_t *context, xmpp_stanza_t *base,
                               t_string ns)
{
    xmpp_stanza_t *parent = base;

    if (!parent)
    {
        parent = xmpp_stanza_new(context);
        xmpp_stanza_set_name(parent, "ping");
    }

    if (!ns.value.empty())
        xmpp_stanza_set_ns(parent, ns.value.c_str());

    return parent;
}

xmpp_stanza_t *stanza__iq_query(xmpp_ctx_t *context, xmpp_stanza_t *base,
                                t_string ns, t_string node)
{
    xmpp_stanza_t *parent = base;

    if (!parent)
    {
        parent = xmpp_stanza_new(context);
        xmpp_stanza_set_name(parent, "query");
    }

    if (!ns.value.empty())
        xmpp_stanza_set_ns(parent, ns.value.c_str());

    if (!node.value.empty())
        xmpp_stanza_set_attribute(parent, "node", node.value.c_str());

    return parent;
}
