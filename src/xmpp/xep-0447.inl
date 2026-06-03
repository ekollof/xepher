// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "node.hh"
#pragma GCC visibility push(default)
#include "ns.hh"
#pragma GCC visibility pop

namespace stanza {

    /* Stateless File Sharing (XEP-0447) + Encrypted SFS (XEP-0448)
     * Builders for <file-sharing>, <file> (per XEP-0446), <encrypted>, etc.
     * Used for rich upload previews (SFS primary + SIMS/OOB legacy).
     * Follows patterns from xep-0428.inl / xep-0060.inl etc.
     */

    struct xep0447 {

        // Reusable <hash xmlns='urn:xmpp:hashes:2' algo='...'>b64val</hash>
        struct hash : virtual public spec {
            hash(std::string_view algo, std::string_view val) : spec("hash") {
                xmlns<urn::xmpp::hashes::_2>();
                attr("algo", algo);
                text(val);
            }
        };

        // <file xmlns='urn:xmpp:file:metadata:0'> per XEP-0446 (core for SFS)
        // Supports the elements we emit: media-type, name, size, width/height (images),
        // one or more hash.
        struct file : virtual public spec {
            file() : spec("file") {
                xmlns<urn::xmpp::file::metadata>();
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

            file& add_hash(const hash& h) { child(const_cast<hash&>(h)); return *this; }
            file& hash_sha256(std::string_view b64val) {
                return add_hash( xep0447::hash("sha-256", b64val) );
            }
        };

        // <url-data xmlns='http://jabber.org/protocol/url-data' target='...' />
        struct url_data : virtual public spec {
            url_data(std::string_view target) : spec("url-data") {
                attr("xmlns", "http://jabber.org/protocol/url-data");
                attr("target", target);
            }
        };

        // Simple <sources> container (used for plain and inner for ESFS).
        struct sources : virtual public spec {
            sources() : spec("sources") {}
            sources& add(spec& ch) { child(ch); return *this; }
        };

        // <encrypted xmlns='urn:xmpp:esfs:0' cipher='...'> for XEP-0448 ESFS
        // (placed inside <sources> of a file-sharing)
        // Contains key/iv (b64), optional cipher hash(es), and inner <sources> (for the cipher bytes url).
        struct encrypted : virtual public spec {
            encrypted() : spec("encrypted") {
                xmlns<urn::xmpp::esfs::_0>();
                attr("cipher", "urn:xmpp:ciphers:aes-256-gcm-nopadding:0");
            }

            encrypted& key(std::string_view b64) {
                struct k : virtual public spec { k(std::string_view vv) : spec("key") { text(vv); } } k_el(b64);
                child(k_el); return *this;
            }
            encrypted& iv(std::string_view b64) {
                struct i : virtual public spec { i(std::string_view vv) : spec("iv") { text(vv); } } i_el(b64);
                child(i_el); return *this;
            }

            encrypted& add_hash(const xep0447::hash& h) { child(const_cast<xep0447::hash&>(h)); return *this; }
            encrypted& cipher_hash_sha256(std::string_view b64val) {
                return add_hash( xep0447::hash("sha-256", b64val) );
            }

            // inner sources (typically containing url-data for the ciphertext)
            encrypted& sources(xep0447::sources s) { child(s); return *this; }
        };

        // <file-sharing xmlns='urn:xmpp:sfs:0' disposition='inline'> ... </file-sharing>
        // Primary per XEP-0447. Contains <file> + <sources> (which may hold <encrypted> for ESFS or direct url-data).
        struct file_sharing : virtual public spec {
            file_sharing() : spec("file-sharing") {
                xmlns<urn::xmpp::sfs::_0>();
                attr("disposition", "inline");
            }

            file_sharing& file(xep0447::file f) { child(f); return *this; }

            // sources container (typed)
            file_sharing& sources(xep0447::sources s) { child(s); return *this; }
        };

        // stanza::message mixin so we can do stanza::message().file_sharing( ... )
        struct message : virtual public spec {
            message() : spec("message") {}

            message& file_sharing(xep0447::file_sharing fs) { child(fs); return *this; }
        };
    };

}  // namespace stanza
