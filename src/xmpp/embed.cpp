// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "embed.hh"

#include <string_view>
#include <algorithm>

namespace xepher {

// -------------------------------------------------------------------------
// parse_embed_tags()
//
// Hand-rolled parser for {{ embed|attach|video "filename" [alt="text"] }}.
// No regex dependency — straightforward state-machine scan.
// -------------------------------------------------------------------------
std::vector<embed_tag> parse_embed_tags(const std::string &body)
{
    std::vector<embed_tag> result;
    const std::string_view sv(body);
    std::size_t pos = 0;

    while (pos < sv.size())
    {
        // Find next opening {{
        auto open = sv.find("{{", pos);
        if (open == std::string_view::npos)
            break;

        // Find matching }}
        auto close = sv.find("}}", open + 2);
        if (close == std::string_view::npos)
            break;

        // Extract the inner content (trim whitespace)
        std::string_view inner = sv.substr(open + 2, close - open - 2);
        // ltrim
        while (!inner.empty() && (inner.front() == ' ' || inner.front() == '\t'))
            inner.remove_prefix(1);
        // rtrim
        while (!inner.empty() && (inner.back() == ' ' || inner.back() == '\t'))
            inner.remove_suffix(1);

        // Identify keyword
        embed_tag tag;
        if (inner.substr(0, 5) == "embed")
        {
            tag.type  = embed_tag::kind::embed;
            inner     = inner.substr(5);
        }
        else if (inner.substr(0, 6) == "attach")
        {
            tag.type  = embed_tag::kind::attach;
            inner     = inner.substr(6);
        }
        else if (inner.substr(0, 5) == "video")
        {
            tag.type  = embed_tag::kind::video;
            inner     = inner.substr(5);
        }
        else
        {
            // Unknown keyword — skip this tag
            pos = close + 2;
            continue;
        }

        // ltrim after keyword
        while (!inner.empty() && (inner.front() == ' ' || inner.front() == '\t'))
            inner.remove_prefix(1);

        // Parse quoted filename
        if (inner.empty() || inner.front() != '"')
        {
            pos = close + 2;
            continue;
        }
        inner.remove_prefix(1); // consume opening "
        auto fname_end = inner.find('"');
        if (fname_end == std::string_view::npos)
        {
            pos = close + 2;
            continue;
        }
        tag.filename = std::string(inner.substr(0, fname_end));
        inner        = inner.substr(fname_end + 1);

        // Parse optional alt="..."
        // ltrim
        while (!inner.empty() && (inner.front() == ' ' || inner.front() == '\t'))
            inner.remove_prefix(1);
        if (inner.substr(0, 4) == "alt=")
        {
            inner = inner.substr(4);
            if (!inner.empty() && inner.front() == '"')
            {
                inner.remove_prefix(1);
                auto alt_end = inner.find('"');
                if (alt_end != std::string_view::npos)
                    tag.alt = std::string(inner.substr(0, alt_end));
            }
        }

        // Record the full original tag text for substitution
        tag.tag_text = std::string(sv.substr(open, close + 2 - open));

        result.push_back(std::move(tag));
        pos = close + 2;
    }

    return result;
}

// -------------------------------------------------------------------------
// render_body()
// -------------------------------------------------------------------------
std::string render_body(const std::string &tmpl,
                        const std::vector<embed_tag> &embeds)
{
    std::string result = tmpl;
    // Replace in reverse order of position so that earlier offsets remain valid
    // after each substitution.  Build a sorted (by position) list first.
    struct subst { std::size_t pos; std::string tag; std::string repl; };
    std::vector<subst> subs;
    subs.reserve(embeds.size());
    for (const auto &e : embeds)
    {
        auto p = result.find(e.tag_text);
        if (p != std::string::npos)
            subs.push_back({p, e.tag_text, e.markdown()});
    }
    // Sort descending by position so we can replace right-to-left
    std::sort(subs.begin(), subs.end(),
              [](const subst &a, const subst &b){ return a.pos > b.pos; });
    for (const auto &s : subs)
        result.replace(s.pos, s.tag.size(), s.repl);
    return result;
}

} // namespace xepher
