// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <algorithm>
#include <cctype>
#include <ranges>
#include <string>
#include <string_view>

#include "atom.hh"
#include "node.hh"
#include "stanza_view.hh"
#include "test_export.hh"
#include "util.hh"
#include "xhtml.hh"

namespace
{

[[nodiscard]] bool name_iequals(std::string_view a, std::string_view b)
{
    return a.size() == b.size()
        && std::ranges::equal(a, b, [](const char ca, const char cb) {
               return std::tolower(static_cast<unsigned char>(ca))
                   == std::tolower(static_cast<unsigned char>(cb));
           });
}

[[nodiscard]] bool attr_iequals(std::optional<std::string_view> value, std::string_view expected)
{
    return value && name_iequals(*value, expected);
}

// Read text content of a direct child element by tag name.
// If the element has type='xhtml', render via xhtml_to_weechat().
// If the element has type='html', strip tags via html_strip_to_plain().
[[nodiscard]] std::string atom_text_child(const xmpp::StanzaView parent, const std::string_view tag)
{
    if (!parent.valid())
        return {};

    const xmpp::StanzaView el = parent.child(tag);
    if (!el.valid())
        return {};

    if (attr_iequals(el.attr("type"), "xhtml"))
        return xhtml_to_weechat(el);

    std::string s = el.text();
    if (s.empty())
        return {};

    if (attr_iequals(el.attr("type"), "html"))
        return html_strip_to_plain(s);
    return s;
}

void resolve_author_from_uri(std::string &author, const std::string &author_uri)
{
    if (!author.empty() || author_uri.empty())
        return;

    if (author_uri.size() >= 5 && author_uri.starts_with("xmpp:"))
        author = author_uri.substr(5);
    else
        author = author_uri;
}

} // namespace

XMPP_TEST_EXPORT atom_entry parse_atom_entry(
    const xmpp::StanzaView entry,
    const std::string_view publisher)
{
    atom_entry e;
    if (!entry.valid())
        return e;

    e.title   = atom_text_child(entry, "title");
    e.summary = atom_text_child(entry, "summary");
    e.pubdate = atom_text_child(entry, "published");
    e.updated = atom_text_child(entry, "updated");
    e.item_id = atom_text_child(entry, "id");

    if (e.pubdate.empty())
        e.pubdate = e.updated;

    // <content> — RFC 4287 §4.1.3.
    {
        std::string text_content;
        std::string xhtml_content;
        std::string html_content;
        bool found_text = false;
        bool found_xhtml = false;
        bool found_html = false;

        for (const xmpp::StanzaView child : entry)
        {
            if (!name_iequals(child.name(), "content"))
                continue;

            const auto type_attr = child.attr("type");

            if (!type_attr || name_iequals(*type_attr, "text"))
            {
                if (!found_text)
                {
                    text_content = child.text();
                    found_text = !text_content.empty();
                }
            }
            else if (name_iequals(*type_attr, "xhtml"))
            {
                if (!found_xhtml)
                {
                    xhtml_content = xhtml_to_weechat(child);
                    found_xhtml = !xhtml_content.empty();
                    if (found_xhtml)
                        e.content_is_xhtml = true;
                }
            }
            else if (name_iequals(*type_attr, "html"))
            {
                if (!found_html)
                {
                    const std::string raw = child.text();
                    if (!raw.empty())
                    {
                        html_content = html_strip_to_plain(raw);
                        found_html = !html_content.empty();
                    }
                }
            }
        }

        if (found_text)
        {
            e.content      = std::move(text_content);
            e.content_type = "text";
        }
        else if (found_xhtml)
        {
            e.content      = std::move(xhtml_content);
            e.content_type = "xhtml";
        }
        else if (found_html)
        {
            e.content      = std::move(html_content);
            e.content_type = "html";
        }

        if (e.content_type == "text" || e.content_type.empty())
            e.content = apply_markdown_to_weechat(e.content);
    }

    for (const xmpp::StanzaView child : entry)
    {
        if (!name_iequals(child.name(), "author"))
            continue;

        if (e.author.empty())
        {
            const xmpp::StanzaView name_el = child.child("name");
            if (name_el.valid())
                e.author = name_el.text();
        }

        if (e.author.empty())
        {
            std::string s = child.text();
            if (s.find_first_not_of(" \t\r\n") != std::string::npos)
                e.author = std::move(s);
        }

        if (e.author_uri.empty())
        {
            const xmpp::StanzaView uri_el = child.child("uri");
            if (uri_el.valid())
                e.author_uri = uri_el.text();
        }

        if (!e.author.empty() && !e.author_uri.empty())
            break;
    }

    resolve_author_from_uri(e.author, e.author_uri);

    if (e.author.empty() && !publisher.empty())
        e.author = std::string(publisher);

    for (const xmpp::StanzaView child : entry)
    {
        const std::string_view child_name = child.name();
        if (child_name.empty())
            continue;

        if (name_iequals(child_name, "link"))
        {
            const std::string href = child.attr_string("href");
            if (href.empty())
                continue;

            const auto rel = child.attr("rel");
            if (!rel || name_iequals(*rel, "alternate"))
            {
                if (e.link.empty())
                    e.link = href;
            }
            else if (name_iequals(*rel, "via"))
            {
                if (e.via_link.empty())
                    e.via_link = href;
            }
            else if (name_iequals(*rel, "replies"))
            {
                if (e.replies_link.empty())
                {
                    e.replies_link = href;
                    std::string thr_count = child.attr_string("thr:count");
                    if (thr_count.empty())
                        thr_count = child.attr_string("count");
                    if (!thr_count.empty())
                    {
                        if (auto n = parse_int64(thr_count); n && *n >= 0)
                            e.comments_count = static_cast<int>(*n);
                    }
                    std::string thr_updated = child.attr_string("thr:updated");
                    if (thr_updated.empty())
                        thr_updated = child.attr_string("updated");
                    if (!thr_updated.empty())
                        e.comments_updated = std::move(thr_updated);
                }
            }
            else if (name_iequals(*rel, "enclosure"))
            {
                e.enclosures.emplace_back(href);
            }
        }
        else if (name_iequals(child_name, "in-reply-to")
                 || name_iequals(child_name, "thr:in-reply-to"))
        {
            if (e.reply_to.empty())
            {
                const std::string href = child.attr_string("href");
                if (!href.empty())
                    e.reply_to = href;
                else
                {
                    const std::string ref = child.attr_string("ref");
                    if (!ref.empty())
                        e.reply_to = ref;
                }
            }
        }
        else if (name_iequals(child_name, "category"))
        {
            const std::string term = child.attr_string("term");
            if (!term.empty())
                e.categories.emplace_back(term);
        }
        else if (name_iequals(child_name, "geoloc"))
        {
            const std::string lat = atom_text_child(child, "lat");
            const std::string lon = atom_text_child(child, "lon");
            const std::string country = atom_text_child(child, "country");
            const std::string region = atom_text_child(child, "region");
            const std::string locality = atom_text_child(child, "locality");

            if (!lat.empty() || !lon.empty())
                e.geoloc = lat + (lat.empty() || lon.empty() ? "" : ", ") + lon;
            if (!locality.empty())
                e.geoloc += (e.geoloc.empty() ? "" : " - ") + locality;
            else if (!region.empty())
                e.geoloc += (e.geoloc.empty() ? "" : " - ") + region;
            if (!country.empty())
                e.geoloc += (e.geoloc.empty() ? "" : ", ") + country;
        }
        else if (name_iequals(child_name, "file-sharing"))
        {
            sfs_attachment att;
            att.disposition = child.attr_string("disposition");

            const xmpp::StanzaView file_el = child.child("file");
            if (file_el.valid())
            {
                att.filename   = atom_text_child(file_el, "name");
                att.media_type = atom_text_child(file_el, "media-type");
                if (auto parsed = parse_int64(atom_text_child(file_el, "size"));
                    parsed && *parsed >= 0)
                {
                    att.size = static_cast<uint64_t>(*parsed);
                }
                if (auto w_parsed = parse_uint32(atom_text_child(file_el, "width")); w_parsed)
                {
                    att.width = static_cast<int>(*w_parsed);
                }
                if (auto h_parsed = parse_uint32(atom_text_child(file_el, "height")); h_parsed)
                {
                    att.height = static_cast<int>(*h_parsed);
                }

                for (const xmpp::StanzaView hc : file_el)
                {
                    if (!name_iequals(hc.name(), "hash"))
                        continue;
                    if (attr_iequals(hc.attr("algo"), "sha-256"))
                    {
                        att.sha256_b64 = hc.text();
                        break;
                    }
                }
            }

            const xmpp::StanzaView sources_el = child.child("sources");
            if (sources_el.valid())
            {
                for (const xmpp::StanzaView src : sources_el)
                {
                    if (!name_iequals(src.name(), "url-data"))
                        continue;
                    const std::string target = src.attr_string("target");
                    if (!target.empty())
                    {
                        att.url = target;
                        break;
                    }
                }
            }

            if (!att.url.empty())
                e.attachments.push_back(std::move(att));
        }
    }

    return e;
}

XMPP_TEST_EXPORT atom_feed parse_atom_feed(const xmpp::StanzaView feed)
{
    atom_feed f;
    if (!feed.valid())
        return f;

    f.title    = atom_text_child(feed, "title");
    f.subtitle = atom_text_child(feed, "subtitle");
    f.updated  = atom_text_child(feed, "updated");
    f.feed_id  = atom_text_child(feed, "id");

    const xmpp::StanzaView author_el = feed.child("author");
    if (author_el.valid())
    {
        const xmpp::StanzaView name_el = author_el.child("name");
        if (name_el.valid())
            f.author = name_el.text();

        const xmpp::StanzaView uri_el = author_el.child("uri");
        if (uri_el.valid())
            f.author_uri = uri_el.text();
    }

    resolve_author_from_uri(f.author, f.author_uri);

    return f;
}