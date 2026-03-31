// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

// Template tag embedding for /feed post.
//
// Supported tags in the post body:
//   {{ embed "file.jpg" }}               — inline image  (Markdown ![...](url))
//   {{ embed "file.jpg" alt="sunset" }}  — inline image with alt text
//   {{ attach "doc.pdf" }}               — download link (Markdown [...](url))
//   {{ attach "doc.pdf" alt="Paper" }}   — download link with alt text
//   {{ video "clip.mp4" }}               — inline video  (treated as embed)
//   {{ video "clip.mp4" alt="demo" }}    — inline video with alt text
//
// Each tag is resolved by uploading the referenced file via XEP-0363 HTTP
// Upload and substituting the tag with Markdown once the GET URL is known.
// The two-phase async approach:
//   Phase 1: parse tags, validate files exist, kick off first upload
//   Phase 2: each upload completion triggers the next; when all done, publish

#include <string>
#include <vector>
#include <cstdint>

namespace xepher {

struct embed_tag
{
    enum class kind { embed, attach, video };

    kind        type     = kind::embed;
    std::string filename;       // original filename from the tag
    std::string filepath;       // resolved absolute path
    std::string alt;            // alt text (empty if not provided)

    // Filled in after upload completes:
    std::string get_url;
    std::string mime;
    uint64_t    size    = 0;
    int         width   = 0;
    int         height  = 0;
    std::string sha256_b64;

    bool uploaded() const { return !get_url.empty(); }

    // XEP-0447 disposition value
    const char *disposition() const
    {
        return (type == kind::attach) ? "attachment" : "inline";
    }

    // Markdown replacement string (only valid after upload)
    std::string markdown() const
    {
        const std::string display = alt.empty() ? filename : alt;
        if (type == kind::attach)
            return "[" + display + "](" + get_url + ")";
        else
            return "![" + display + "](" + get_url + ")";
    }

    // The full original tag text as it appeared in the body (used for substitution)
    std::string tag_text; // e.g. `{{ embed "file.jpg" }}`
};

// Pending feed post waiting for embed uploads to complete.
// Stored in account::pending_feed_posts keyed by the current upload request IQ id.
struct pending_feed_post
{
    std::string account_name;

    // Publish target
    std::string service;
    std::string node;

    // Whether this is a reply
    bool        is_reply            = false;
    std::string reply_to_id;
    std::string reply_target_service;
    std::string reply_target_node;

    // Post metadata
    std::string title;              // may be empty
    bool        access_open        = false;
    std::string item_uuid;          // pre-generated UUID
    std::string atom_id;            // pre-generated tag: URI
    std::string timestamp;          // ISO-8601 UTC, e.g. "2026-03-31T12:00:00Z"

    // Body with {{ }} tags still intact (used for draft saving on failure)
    std::string raw_body_template;

    // Embed tags extracted from the template, in order of appearance
    std::vector<embed_tag> embeds;

    // How many have completed uploading so far
    size_t uploads_done = 0;

    // The WeeChat buffer to report progress/errors to
    struct t_gui_buffer *buffer = nullptr;
};

// -------------------------------------------------------------------------
// parse_embed_tags()
//
// Scans `body` for {{ embed|attach|video "filename" [alt="text"] }} tags.
// Returns one embed_tag per match, with tag_text, type, filename, and alt
// filled in. filepath is NOT resolved here (caller does that).
// -------------------------------------------------------------------------
std::vector<embed_tag> parse_embed_tags(const std::string &body);

// -------------------------------------------------------------------------
// render_body()
//
// Substitutes every embed_tag's tag_text with its markdown() replacement
// in `tmpl`. All tags must have been uploaded (get_url non-empty).
// -------------------------------------------------------------------------
std::string render_body(const std::string &tmpl,
                        const std::vector<embed_tag> &embeds);

} // namespace xepher
