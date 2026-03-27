// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "atom.hh"
#include "xhtml.hh"

#include <strings.h>  // strcasecmp (POSIX)
#include <strophe.h>

// Helper: read text content of a direct child element by tag name.
// If the element has type='xhtml', render via xhtml_to_weechat().
// If the element has type='html', strip tags via html_strip_to_plain().
static std::string atom_text_child(xmpp_ctx_t *ctx, xmpp_stanza_t *parent, const char *tag)
{
    if (!parent) return {};
    xmpp_stanza_t *el = xmpp_stanza_get_child_by_name(parent, tag);
    if (!el) return {};
    const char *type_attr = xmpp_stanza_get_attribute(el, "type");
    if (type_attr && strcasecmp(type_attr, "xhtml") == 0)
        return xhtml_to_weechat(el);
    char *t = xmpp_stanza_get_text(el);
    if (!t) return {};
    std::string s(t);
    xmpp_free(ctx, t);
    if (type_attr && strcasecmp(type_attr, "html") == 0)
        return html_strip_to_plain(s);
    return s;
}

atom_entry parse_atom_entry(xmpp_ctx_t *ctx, xmpp_stanza_t *entry,
                            const char *publisher)
{
    atom_entry e;
    if (!entry) return e;

    e.title   = atom_text_child(ctx, entry, "title");
    e.summary = atom_text_child(ctx, entry, "summary");
    e.pubdate = atom_text_child(ctx, entry, "published");
    e.updated = atom_text_child(ctx, entry, "updated");
    e.item_id = atom_text_child(ctx, entry, "id");

    if (e.pubdate.empty())
        e.pubdate = e.updated;

    // <content> — RFC 4287 §4.1.3.
    // Movim and other clients often publish both type='text' and type='xhtml'.
    // Prefer plain text when available; fall back to XHTML (rendered) or HTML
    // (tag-stripped).  Iterate all <content> children to find the best one.
    {
        std::string text_content, xhtml_content, html_content;
        bool found_text = false, found_xhtml = false, found_html = false;

        for (xmpp_stanza_t *child = xmpp_stanza_get_children(entry);
             child; child = xmpp_stanza_get_next(child))
        {
            const char *child_name = xmpp_stanza_get_name(child);
            if (!child_name || strcasecmp(child_name, "content") != 0)
                continue;

            const char *type_attr = xmpp_stanza_get_attribute(child, "type");

            if (!type_attr || strcasecmp(type_attr, "text") == 0)
            {
                if (!found_text)
                {
                    char *t = xmpp_stanza_get_text(child);
                    if (t) { text_content = t; xmpp_free(ctx, t); found_text = true; }
                }
            }
            else if (strcasecmp(type_attr, "xhtml") == 0)
            {
                if (!found_xhtml)
                {
                    // xhtml_to_weechat() renders the stanza tree with WeeChat
                    // colour/attribute codes for bold, italic, links, etc.
                    xhtml_content = xhtml_to_weechat(child);
                    found_xhtml = !xhtml_content.empty();
                    if (found_xhtml) e.content_is_xhtml = true;
                }
            }
            else if (strcasecmp(type_attr, "html") == 0)
            {
                if (!found_html)
                {
                    char *t = xmpp_stanza_get_text(child);
                    if (t)
                    {
                        html_content = html_strip_to_plain(std::string(t));
                        xmpp_free(ctx, t);
                        found_html = !html_content.empty();
                    }
                }
            }
        }

        // Preference order: plain text > XHTML rendered > HTML stripped
        if (found_text)
            e.content = std::move(text_content);
        else if (found_xhtml)
            e.content = std::move(xhtml_content);
        else if (found_html)
            e.content = std::move(html_content);
    }

    // <author><name>…</name></author>
    {
        xmpp_stanza_t *author_el = xmpp_stanza_get_child_by_name(entry, "author");
        if (author_el)
        {
            xmpp_stanza_t *name_el = xmpp_stanza_get_child_by_name(author_el, "name");
            if (name_el)
            {
                char *t = xmpp_stanza_get_text(name_el);
                if (t)
                {
                    e.author = t;
                    xmpp_free(ctx, t);
                }
            }

            xmpp_stanza_t *uri_el = xmpp_stanza_get_child_by_name(author_el, "uri");
            if (uri_el)
            {
                char *t = xmpp_stanza_get_text(uri_el);
                if (t)
                {
                    e.author_uri = t;
                    xmpp_free(ctx, t);
                }
            }
        }
    }

    if (e.author.empty() && !e.author_uri.empty())
    {
        if (e.author_uri.size() >= 5 && e.author_uri.compare(0, 5, "xmpp:") == 0)
            e.author = e.author_uri.substr(5);
        else
            e.author = e.author_uri;
    }

    if (e.author.empty() && publisher && *publisher)
        e.author = publisher;

    // Iterate children for <link> and <thr:in-reply-to>
    for (xmpp_stanza_t *child = xmpp_stanza_get_children(entry);
         child; child = xmpp_stanza_get_next(child))
    {
        const char *child_name = xmpp_stanza_get_name(child);
        if (!child_name) continue;

        if (strcasecmp(child_name, "link") == 0)
        {
            const char *rel  = xmpp_stanza_get_attribute(child, "rel");
            const char *href = xmpp_stanza_get_attribute(child, "href");
            if (!href) continue;

            if (!rel || strcasecmp(rel, "alternate") == 0)
            {
                if (e.link.empty())
                    e.link = href;
            }
            else if (strcasecmp(rel, "via") == 0)
            {
                if (e.via_link.empty())
                    e.via_link = href;
            }
            else if (strcasecmp(rel, "replies") == 0)
            {
                if (e.replies_link.empty())
                    e.replies_link = href;
            }
            else if (strcasecmp(rel, "enclosure") == 0)
            {
                e.enclosures.emplace_back(href);
            }
        }
        else if (strcasecmp(child_name, "in-reply-to") == 0 ||
                 strcasecmp(child_name, "thr:in-reply-to") == 0)
        {
            // Prefer href (full XMPP URI) but fall back to ref (item ID).
            if (e.reply_to.empty())
            {
                const char *href = xmpp_stanza_get_attribute(child, "href");
                if (href)
                    e.reply_to = href;
                else
                {
                    const char *ref = xmpp_stanza_get_attribute(child, "ref");
                    if (ref) e.reply_to = ref;
                }
            }
        }
        else if (strcasecmp(child_name, "category") == 0)
        {
            const char *term = xmpp_stanza_get_attribute(child, "term");
            if (term && *term)
                e.categories.emplace_back(term);
        }
        else if (strcasecmp(child_name, "geoloc") == 0)
        {
            std::string lat = atom_text_child(ctx, child, "lat");
            std::string lon = atom_text_child(ctx, child, "lon");
            std::string country = atom_text_child(ctx, child, "country");
            std::string region = atom_text_child(ctx, child, "region");
            std::string locality = atom_text_child(ctx, child, "locality");

            if (!lat.empty() || !lon.empty())
                e.geoloc = lat + (lat.empty() || lon.empty() ? "" : ", ") + lon;
            if (!locality.empty())
                e.geoloc += (e.geoloc.empty() ? "" : " - ") + locality;
            else if (!region.empty())
                e.geoloc += (e.geoloc.empty() ? "" : " - ") + region;
            if (!country.empty())
                e.geoloc += (e.geoloc.empty() ? "" : ", ") + country;
        }
    }

    return e;
}

atom_feed parse_atom_feed(xmpp_ctx_t *ctx, xmpp_stanza_t *feed)
{
    atom_feed f;
    if (!feed) return f;

    f.title    = atom_text_child(ctx, feed, "title");
    f.subtitle = atom_text_child(ctx, feed, "subtitle");
    f.updated  = atom_text_child(ctx, feed, "updated");
    f.feed_id  = atom_text_child(ctx, feed, "id");

    xmpp_stanza_t *author_el = xmpp_stanza_get_child_by_name(feed, "author");
    if (author_el)
    {
        xmpp_stanza_t *name_el = xmpp_stanza_get_child_by_name(author_el, "name");
        if (name_el)
        {
            char *t = xmpp_stanza_get_text(name_el);
            if (t)
            {
                f.author = t;
                xmpp_free(ctx, t);
            }
        }

        xmpp_stanza_t *uri_el = xmpp_stanza_get_child_by_name(author_el, "uri");
        if (uri_el)
        {
            char *t = xmpp_stanza_get_text(uri_el);
            if (t)
            {
                f.author_uri = t;
                xmpp_free(ctx, t);
            }
        }
    }

    if (f.author.empty() && !f.author_uri.empty())
    {
        if (f.author_uri.size() >= 5 && f.author_uri.compare(0, 5, "xmpp:") == 0)
            f.author = f.author_uri.substr(5);
        else
            f.author = f.author_uri;
    }

    return f;
}
