// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <string>
#include <strophe.h>
#include <vector>

// Parsed representation of an Atom <entry> element (RFC 4287 / XEP-0277 / XEP-0472).
struct atom_entry
{
    std::string title;          // <title>
    std::string summary;        // <summary>
    std::string content;        // <content> body text (XHTML stripped to plain text if needed)
    bool        content_is_xhtml = false; // true when <content type='xhtml'>
    std::string pubdate;        // <published>
    std::string updated;        // <updated>
    std::string link;           // <link rel='alternate' href='...'/>
    std::string via_link;       // <link rel='via' href='...'/>  (XEP-0472 boost/repeat)
    std::string replies_link;   // <link rel='replies' href='...'/>  (XEP-0277 comments)
    std::string author;         // <author><name>…</name></author>
    std::string author_uri;     // <author><uri>…</uri></author>
    std::string reply_to;       // <thr:in-reply-to ref|href='…'>
    std::string item_id;        // atom <id> element text
    std::vector<std::string> categories; // <category term='...'>
    std::vector<std::string> enclosures; // <link rel='enclosure' href='...'>
    std::string geoloc;         // compact XEP-0080 summary

    // Convenience: returns the best available body text.
    // XEP-0277/RFC 4287 precedence: <content> beats <summary>.
    const std::string &body() const
    {
        if (!content.empty()) return content;
        if (!summary.empty()) return summary;
        return content; // empty
    }

    bool empty() const
    {
        return title.empty() && summary.empty() && content.empty();
    }
};

struct atom_feed
{
    std::string title;      // <feed><title>
    std::string subtitle;   // <feed><subtitle>
    std::string updated;    // <feed><updated>
    std::string author;     // <feed><author><name>
    std::string author_uri; // <feed><author><uri>
    std::string feed_id;    // <feed><id>

    bool empty() const
    {
        return title.empty() && subtitle.empty() && updated.empty()
            && author.empty() && author_uri.empty() && feed_id.empty();
    }
};

// Parse an Atom <entry> stanza into an atom_entry.
// Returns a default-constructed (empty) atom_entry when entry is nullptr.
atom_entry parse_atom_entry(xmpp_ctx_t *ctx, xmpp_stanza_t *entry,
                            const char *publisher = nullptr);

atom_feed parse_atom_feed(xmpp_ctx_t *ctx, xmpp_stanza_t *feed);
