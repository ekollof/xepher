// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

// Compatibility wrappers for libstrophe APIs missing on older releases
// (e.g. Slackware 15 ships ~0.8.x without flags/keepalive/verbosity).
//
// CMake probes symbols and writes strophe_compat_config.hh (0/1 macros).

#include <string>
#include <strophe.h>

#include "strophe_compat_config.hh"

#if !XEPHER_HAVE_XMPP_STANZA_NEW_FROM_STRING
#include <libxml/parser.h>
#include <libxml/tree.h>
#endif

// ── Connection flag macros (0.9+) ──────────────────────────────────────────
#if XEPHER_DEFINE_XMPP_CONN_FLAG_MACROS
#ifndef XMPP_CONN_FLAG_DISABLE_TLS
#define XMPP_CONN_FLAG_DISABLE_TLS (1UL << 0)
#endif
#ifndef XMPP_CONN_FLAG_MANDATORY_TLS
#define XMPP_CONN_FLAG_MANDATORY_TLS (1UL << 1)
#endif
#ifndef XMPP_CONN_FLAG_LEGACY_SSL
#define XMPP_CONN_FLAG_LEGACY_SSL (1UL << 2)
#endif
#ifndef XMPP_CONN_FLAG_TRUST_TLS
#define XMPP_CONN_FLAG_TRUST_TLS (1UL << 3)
#endif
#ifndef XMPP_CONN_FLAG_LEGACY_AUTH
#define XMPP_CONN_FLAG_LEGACY_AUTH (1UL << 4)
#endif
#endif

#if XEPHER_DEFINE_XMPP_CONN_FLAG_DISABLE_SM
#ifndef XMPP_CONN_FLAG_DISABLE_SM
// Harmless on pre-SM libstrophe (no built-in SM to disable).
#define XMPP_CONN_FLAG_DISABLE_SM (1UL << 5)
#endif
#endif

// ── Optional C API shims (declared only when the system header lacks them) ─
// Provided as static inline so strophe.hh decltype() and call sites that use
// the real C names still compile against ancient headers.

#if !XEPHER_HAVE_XMPP_CTX_SET_VERBOSITY
static inline void xmpp_ctx_set_verbosity(xmpp_ctx_t * /*ctx*/, int /*level*/)
{}
#endif

#if !XEPHER_HAVE_XMPP_CONN_GET_FLAGS
static inline long xmpp_conn_get_flags(const xmpp_conn_t * /*conn*/)
{
    return 0;
}
#endif

#if !XEPHER_HAVE_XMPP_CONN_SET_FLAGS
static inline int xmpp_conn_set_flags(xmpp_conn_t *conn, long flags)
{
    // 0.8.x only had xmpp_conn_disable_tls — map DISABLE_TLS when possible.
#if XEPHER_HAVE_XMPP_CONN_DISABLE_TLS
    if (conn && (flags & XMPP_CONN_FLAG_DISABLE_TLS))
        xmpp_conn_disable_tls(conn);
#else
    (void)conn;
#endif
    (void)flags;
    return 0;
}
#endif

#if !XEPHER_HAVE_XMPP_CONN_SET_KEEPALIVE
static inline void xmpp_conn_set_keepalive(xmpp_conn_t * /*conn*/,
                                          int /*timeout*/,
                                          int /*interval*/)
{}
#endif

#if !XEPHER_HAVE_XMPP_CONN_SET_CERTFAIL_HANDLER
// Minimal stubs so call sites can still #if around real TLS cert inspection.
#ifndef XEPHER_STROPHE_TLS_CERT_STUBS
#define XEPHER_STROPHE_TLS_CERT_STUBS
struct _xmpp_tlscert_t;
typedef struct _xmpp_tlscert_t xmpp_tlscert_t;
typedef int (*xmpp_certfail_handler)(const xmpp_tlscert_t *cert,
                                     const char *errormsg);
#endif
static inline void xmpp_conn_set_certfail_handler(
    xmpp_conn_t * /*conn*/, xmpp_certfail_handler /*h*/)
{}
#endif

#if !XEPHER_HAVE_XMPP_CONNECT_RAW
// IBR path checks XEPHER_HAVE_XMPP_CONNECT_RAW before calling.
#ifndef XMPP_EOK
#define XMPP_EOK 0
#endif
static inline int xmpp_connect_raw(xmpp_conn_t * /*conn*/,
                                   const char * /*domain*/,
                                   unsigned short /*port*/,
                                   xmpp_conn_handler /*cb*/,
                                   void * /*userdata*/)
{
    return -1;
}
#endif

namespace xepher::strophe {

#if XEPHER_HAVE_XMPP_STANZA_ADD_CHILD_EX

inline int add_child_ex(xmpp_stanza_t *parent, xmpp_stanza_t *child, int do_clone)
{
    return xmpp_stanza_add_child_ex(parent, child, do_clone);
}

#else

// Pre-0.10: only xmpp_stanza_add_child (always clones). When do_clone is
// false, transfer ownership by releasing the caller's reference after the
// parent takes its clone.
inline int add_child_ex(xmpp_stanza_t *parent, xmpp_stanza_t *child, int do_clone)
{
    if (!parent || !child)
        return -1;
    const int rc = xmpp_stanza_add_child(parent, child);
    if (rc == 0 && !do_clone)
        xmpp_stanza_release(child);
    return rc;
}

#endif

#if XEPHER_HAVE_XMPP_STANZA_NEW_FROM_STRING

inline xmpp_stanza_t *new_from_string(xmpp_ctx_t *ctx, const char *str)
{
    return xmpp_stanza_new_from_string(ctx, str);
}

#else

namespace detail {

inline xmpp_stanza_t *xml_node_to_stanza(xmpp_ctx_t *ctx, xmlNode *node)
{
    if (!ctx || !node)
        return nullptr;

    if (node->type == XML_TEXT_NODE || node->type == XML_CDATA_SECTION_NODE)
    {
        if (!node->content || !node->content[0])
            return nullptr;
        xmpp_stanza_t *text = xmpp_stanza_new(ctx);
        if (!text)
            return nullptr;
        xmpp_stanza_set_text(text, reinterpret_cast<const char *>(node->content));
        return text;
    }

    if (node->type != XML_ELEMENT_NODE)
        return nullptr;

    xmpp_stanza_t *stanza = xmpp_stanza_new(ctx);
    if (!stanza)
        return nullptr;

    xmpp_stanza_set_name(stanza, reinterpret_cast<const char *>(node->name));
    if (node->ns && node->ns->href)
        xmpp_stanza_set_ns(stanza, reinterpret_cast<const char *>(node->ns->href));

    for (xmlAttr *attr = node->properties; attr; attr = attr->next)
    {
        xmlChar *val = xmlNodeListGetString(node->doc, attr->children, 1);
        if (val)
        {
            xmpp_stanza_set_attribute(stanza,
                reinterpret_cast<const char *>(attr->name),
                reinterpret_cast<const char *>(val));
            xmlFree(val);
        }
    }

    for (xmlNode *child = node->children; child; child = child->next)
    {
        xmpp_stanza_t *child_st = xml_node_to_stanza(ctx, child);
        if (!child_st)
            continue;
        // Always clone into parent, then release our ref (works on all strophe).
        xmpp_stanza_add_child(stanza, child_st);
        xmpp_stanza_release(child_st);
    }

    return stanza;
}

}  // namespace detail

// Fallback for libstrophe < 0.10: parse with libxml2 (already a Xepher dep).
inline xmpp_stanza_t *new_from_string(xmpp_ctx_t *ctx, const char *str)
{
    if (!ctx || !str || !*str)
        return nullptr;

    xmlDoc *doc = xmlReadMemory(str, static_cast<int>(std::char_traits<char>::length(str)),
                                "stanza.xml", nullptr,
                                XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc)
        return nullptr;

    xmlNode *root = xmlDocGetRootElement(doc);
    xmpp_stanza_t *result = detail::xml_node_to_stanza(ctx, root);
    xmlFreeDoc(doc);
    return result;
}

#endif

}  // namespace xepher::strophe
