// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

// Compatibility wrappers for libstrophe APIs added in 0.10.0
// (xmpp_stanza_new_from_string, xmpp_stanza_add_child_ex). Older packages
// (e.g. some Slackware builds) still ship pre-0.10 headers/libraries.
//
// CMake defines XEPHER_HAVE_XMPP_STANZA_NEW_FROM_STRING and
// XEPHER_HAVE_XMPP_STANZA_ADD_CHILD_EX when the symbols are available.

#include <string>
#include <strophe.h>

#include "strophe_compat_config.hh"

#if !XEPHER_HAVE_XMPP_STANZA_NEW_FROM_STRING
#include <libxml/parser.h>
#include <libxml/tree.h>
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
