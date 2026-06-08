// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "node.hh"

namespace stanza {

    /* Microblogging (XEP-0277) + Atom (RFC 4287) + boost/repeat (XEP-0472)
     * Builders for <entry> and common Atom children used in PEP publish paths.
     */

    struct xep0277 {

        inline static constexpr std::string_view k_atom_ns =
            "http://www.w3.org/2005/Atom";
        inline static constexpr std::string_view k_thr_ns =
            "http://purl.org/syndication/thread/1.0";

        struct entry : virtual public spec {
            entry() : spec("entry") {
                attr("xmlns", k_atom_ns);
            }

            entry& title_text(std::string_view text) {
                struct title_el : virtual public spec {
                    explicit title_el(std::string_view t) : spec("title") {
                        attr("type", "text");
                        text(t);
                    }
                };
                child(title_el{text});
                return *this;
            }

            entry& atom_id(std::string_view id) {
                struct id_el : virtual public spec {
                    explicit id_el(std::string_view v) : spec("id") { text(v); }
                };
                child(id_el{id});
                return *this;
            }

            entry& published(std::string_view ts) {
                struct pub_el : virtual public spec {
                    explicit pub_el(std::string_view v) : spec("published") { text(v); }
                };
                child(pub_el{ts});
                return *this;
            }

            entry& updated(std::string_view ts) {
                struct upd_el : virtual public spec {
                    explicit upd_el(std::string_view v) : spec("updated") { text(v); }
                };
                child(upd_el{ts});
                return *this;
            }

            entry& author(std::string_view name, std::string_view uri) {
                struct author_el : virtual public spec {
                    author_el(std::string_view n, std::string_view u) : spec("author") {
                        struct name_el : virtual public spec {
                            explicit name_el(std::string_view v) : spec("name") { text(v); }
                        };
                        struct uri_el : virtual public spec {
                            explicit uri_el(std::string_view v) : spec("uri") { text(v); }
                        };
                        child(name_el{n});
                        child(uri_el{u});
                    }
                };
                child(author_el{name, uri});
                return *this;
            }

            entry& content_text(std::string_view text) {
                struct content_el : virtual public spec {
                    explicit content_el(std::string_view t) : spec("content") {
                        attr("type", "text");
                        text(t);
                    }
                };
                child(content_el{text});
                return *this;
            }

            entry& link(std::string_view rel, std::string_view href,
                        std::optional<std::string_view> title = std::nullopt,
                        std::optional<std::string_view> ref = std::nullopt) {
                struct link_el : virtual public spec {
                    link_el(std::string_view r, std::string_view h,
                            std::optional<std::string_view> t,
                            std::optional<std::string_view> rf)
                        : spec("link")
                    {
                        attr("rel", r);
                        attr("href", h);
                        if (t) attr("title", *t);
                        if (rf) attr("ref", *rf);
                    }
                };
                child(link_el{rel, href, title, ref});
                return *this;
            }

            entry& in_reply_to(std::string_view ref, std::string_view href) {
                struct reply_el : virtual public spec {
                    reply_el(std::string_view r, std::string_view h) : spec("thr:in-reply-to") {
                        attr("xmlns:thr", k_thr_ns);
                        attr("ref", r);
                        attr("href", h);
                    }
                };
                child(reply_el{ref, href});
                return *this;
            }

            entry& generator(std::string_view name, std::string_view uri,
                             std::string_view version) {
                struct gen_el : virtual public spec {
                    gen_el(std::string_view n, std::string_view u, std::string_view v)
                        : spec("generator")
                    {
                        attr("uri", u);
                        attr("version", v);
                        text(n);
                    }
                };
                child(gen_el{name, uri, version});
                return *this;
            }

            entry& file_sharing(xep0447::file_sharing fs) {
                child(std::move(fs));
                return *this;
            }
        };
    };

} // namespace stanza