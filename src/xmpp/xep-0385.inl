// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "node.hh"
#pragma GCC visibility push(default)
#include "ns.hh"
#pragma GCC visibility pop

namespace stanza {

    /* Stateless Inline Media Sharing (XEP-0385) */
    struct xep0385 {

        // <hash xmlns='urn:xmpp:hashes:2' algo='...'>b64val</hash>
        struct hash : virtual public spec {
            hash(std::string_view algo, std::string_view val) : spec("hash") {
                xmlns<urn::xmpp::hashes::_2>();
                attr("algo", algo);
                text(val);
            }
        };

        // <file xmlns='urn:xmpp:jingle:apps:file-transfer:5'>...</file>
        struct file : virtual public spec {
            file() : spec("file") {
                xmlns<urn::xmpp::jingle::apps::file_transfer::_5>();
            }

            file& media_type(std::string_view v) {
                struct mt : virtual public spec { mt(std::string_view vv) : spec("media-type") { text(vv); } } mt_el(v);
                child(mt_el); return *this;
            }
            file& name(std::string_view v) {
                struct nm : virtual public spec { nm(std::string_view vv) : spec("name") { text(vv); } } nm_el(v);
                child(nm_el); return *this;
            }
            file& size(std::string_view v) {
                struct sz : virtual public spec { sz(std::string_view vv) : spec("size") { text(vv); } } sz_el(v);
                child(sz_el); return *this;
            }
            file& size(size_t n) { return size(std::to_string(n)); }
            file& width(std::string_view v) {
                struct w : virtual public spec { w(std::string_view vv) : spec("width") { text(vv); } } w_el(v);
                child(w_el); return *this;
            }
            file& width(size_t n) { return width(std::to_string(n)); }
            file& height(std::string_view v) {
                struct h : virtual public spec { h(std::string_view vv) : spec("height") { text(vv); } } h_el(v);
                child(h_el); return *this;
            }
            file& height(size_t n) { return height(std::to_string(n)); }
            file& date(std::string_view v) {
                struct d : virtual public spec { d(std::string_view vv) : spec("date") { text(vv); } } d_el(v);
                child(d_el); return *this;
            }
            file& add_hash(const xep0385::hash& h) { child(const_cast<xep0385::hash&>(h)); return *this; }
            file& hash_sha256(std::string_view b64val) {
                return add_hash(xep0385::hash("sha-256", b64val));
            }
        };

        // <sources><reference type='data' uri='...'/></sources>
        struct sources : virtual public spec {
            sources() : spec("sources") {}
            sources& add_source(std::string_view uri) {
                struct ref : virtual public spec {
                    ref(std::string_view u) : spec("reference") {
                        attr("type", "data");
                        attr("uri", u);
                    }
                };
                ref r(uri);
                child(r);
                return *this;
            }
        };

        // <media-sharing xmlns='urn:xmpp:sims:1'>...</media-sharing>
        struct media_sharing : virtual public spec {
            media_sharing() : spec("media-sharing") {
                xmlns<urn::xmpp::sims::_1>();
            }
            media_sharing& file(xep0385::file f) { child(f); return *this; }
            media_sharing& sources(xep0385::sources s) { child(s); return *this; }
        };

        // <reference type='data' begin='...' end='...'>...</reference>
        struct reference : virtual public spec {
            reference(std::string_view begin, std::string_view end)
                : spec("reference")
            {
                xmlns<urn::xmpp::reference::_0>();
                attr("type", "data");
                attr("begin", begin);
                attr("end", end);
            }
            reference& uri(std::string_view u) { attr("uri", u); return *this; }
            reference& media_sharing(xep0385::media_sharing m) { child(m); return *this; }
        };

        // stanza::message mixin
        struct message : virtual public spec {
            message() : spec("message") {}
            message& sims_reference(xep0385::reference r) { child(r); return *this; }
        };
    };

}
