// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "node.hh"
#pragma GCC visibility push(default)
#include "ns.hh"
#pragma GCC visibility pop

namespace stanza {

    /* XEP-0384: OMEMO Encryption – stanza builder types */
    struct xep0384 {

        // ── OMEMO:2 (urn:xmpp:omemo:2) ──────────────────────────────────────

        // <key rid='…' [kex='true']>BASE64</key>
        struct key : virtual public spec {
            key(std::string_view rid, std::string_view b64, bool kex = false)
                : spec("key")
            {
                attr("rid", rid);
                if (kex) attr("kex", "true");
                text(b64);
            }
        };

        // <keys jid='…'> … </keys>
        struct keys : virtual public spec {
            explicit keys(std::string_view jid) : spec("keys") {
                attr("jid", jid);
            }

            keys& add_key(xep0384::key k) { child(k); return *this; }
        };

        // <header sid='…'> … </header>
        struct header : virtual public spec {
            explicit header(std::string_view sid) : spec("header") {
                attr("sid", sid);
            }

            header& add_keys(xep0384::keys k) { child(k); return *this; }
        };

        // <payload>BASE64</payload>
        struct payload : virtual public spec {
            explicit payload(std::string_view b64) : spec("payload") {
                text(b64);
            }
        };

        // <encrypted xmlns='urn:xmpp:omemo:2'> … </encrypted>
        struct encrypted : virtual public spec {
            encrypted() : spec("encrypted") {
                xmlns<urn::xmpp::omemo::_2>();
            }

            encrypted& add_header(xep0384::header h) { child(h); return *this; }
            encrypted& add_payload(xep0384::payload p) { child(p); return *this; }
        };

        // ── OMEMO:2 bundle elements (XEP-0384 §4.3) ──────────────────────────

        // <spk id='…'>BASE64</spk>
        struct spk : virtual public spec {
            spk(std::string_view id, std::string_view b64) : spec("spk") {
                attr("id", id);
                xmlns<urn::xmpp::omemo::_2>();
                text(b64);
            }
        };

        // <spks>BASE64</spks>
        struct spks : virtual public spec {
            explicit spks(std::string_view b64) : spec("spks") {
                xmlns<urn::xmpp::omemo::_2>();
                text(b64);
            }
        };

        // <ik>BASE64</ik>
        struct ik : virtual public spec {
            explicit ik(std::string_view b64) : spec("ik") {
                xmlns<urn::xmpp::omemo::_2>();
                text(b64);
            }
        };

        // <pk id='…'>BASE64</pk>
        struct pk : virtual public spec {
            pk(std::string_view id, std::string_view b64) : spec("pk") {
                attr("id", id);
                xmlns<urn::xmpp::omemo::_2>();
                text(b64);
            }
        };

        // <prekeys> … </prekeys>
        struct prekeys : virtual public spec {
            prekeys() : spec("prekeys") {
                xmlns<urn::xmpp::omemo::_2>();
            }

            prekeys& add_pk(xep0384::pk p) { child(p); return *this; }
        };

        // <bundle xmlns='urn:xmpp:omemo:2'> <spk/> <spks/> <ik/> <prekeys/> </bundle>
        struct bundle : virtual public spec {
            bundle() : spec("bundle") {
                xmlns<urn::xmpp::omemo::_2>();
            }

            bundle& add_spk(xep0384::spk s)     { child(s); return *this; }
            bundle& add_spks(xep0384::spks s)    { child(s); return *this; }
            bundle& add_ik(xep0384::ik i)        { child(i); return *this; }
            bundle& add_prekeys(xep0384::prekeys p) { child(p); return *this; }
        };

        // ── Legacy axolotl (eu.siacs.conversations.axolotl) ─────────────────

        // <key rid='…' [prekey='true']>BASE64</key>
        struct axolotl_key : virtual public spec {
            axolotl_key(std::string_view rid, std::string_view b64, bool prekey = false)
                : spec("key")
            {
                attr("rid", rid);
                if (prekey) attr("prekey", "true");
                text(b64);
            }
        };

        // <keys jid='…'> … </keys>  (same element name, legacy namespace)
        struct axolotl_keys : virtual public spec {
            explicit axolotl_keys(std::string_view jid) : spec("keys") {
                attr("jid", jid);
            }

            axolotl_keys& add_key(xep0384::axolotl_key k) { child(k); return *this; }
        };

        // <iv>BASE64</iv>
        struct axolotl_iv : virtual public spec {
            explicit axolotl_iv(std::string_view b64) : spec("iv") {
                text(b64);
            }
        };

        // <header sid='…'> <keys …/> <iv>…</iv> </header>
        struct axolotl_header : virtual public spec {
            explicit axolotl_header(std::string_view sid) : spec("header") {
                attr("sid", sid);
            }

            // New-style: <header><keys jid='…'><key …/></keys></header>
            axolotl_header& add_keys(xep0384::axolotl_keys k) { child(k); return *this; }
            // Legacy flat layout: <header><key …/></header> (Conversations/older Gajim compat)
            axolotl_header& add_key(xep0384::axolotl_key k) { child(k); return *this; }
            axolotl_header& add_iv(xep0384::axolotl_iv iv) { child(iv); return *this; }
        };

        // <payload>BASE64</payload>  (same element name as OMEMO:2)
        struct axolotl_payload : virtual public spec {
            explicit axolotl_payload(std::string_view b64) : spec("payload") {
                text(b64);
            }
        };

        // <encrypted xmlns='eu.siacs.conversations.axolotl'> … </encrypted>
        struct axolotl_encrypted : virtual public spec {
            axolotl_encrypted() : spec("encrypted") {
                xmlns<eu::siacs::conversations::axolotl>();
            }

            axolotl_encrypted& add_header(xep0384::axolotl_header h) { child(h); return *this; }
            axolotl_encrypted& add_payload(xep0384::axolotl_payload p) { child(p); return *this; }
        };

        // ── Legacy axolotl bundle elements ───────────────────────────────────

        // <signedPreKeyPublic signedPreKeyId='…'>BASE64</signedPreKeyPublic>
        struct axolotl_spk : virtual public spec {
            axolotl_spk(std::string_view id, std::string_view b64) : spec("signedPreKeyPublic") {
                attr("signedPreKeyId", id);
                text(b64);
            }
        };

        // <signedPreKeySignature>BASE64</signedPreKeySignature>
        struct axolotl_spks : virtual public spec {
            explicit axolotl_spks(std::string_view b64) : spec("signedPreKeySignature") {
                text(b64);
            }
        };

        // <identityKey>BASE64</identityKey>
        struct axolotl_ik : virtual public spec {
            explicit axolotl_ik(std::string_view b64) : spec("identityKey") {
                text(b64);
            }
        };

        // <preKeyPublic preKeyId='…'>BASE64</preKeyPublic>
        struct axolotl_pk : virtual public spec {
            axolotl_pk(std::string_view id, std::string_view b64) : spec("preKeyPublic") {
                attr("preKeyId", id);
                text(b64);
            }
        };

        // <prekeys> … </prekeys>  (legacy namespace)
        struct axolotl_prekeys : virtual public spec {
            axolotl_prekeys() : spec("prekeys") {
                xmlns<eu::siacs::conversations::axolotl>();
            }

            axolotl_prekeys& add_pk(xep0384::axolotl_pk p) { child(p); return *this; }
        };

        // <bundle xmlns='eu.siacs.conversations.axolotl'> … </bundle>
        struct axolotl_bundle : virtual public spec {
            axolotl_bundle() : spec("bundle") {
                xmlns<eu::siacs::conversations::axolotl>();
            }

            axolotl_bundle& add_spk(xep0384::axolotl_spk s)       { child(s); return *this; }
            axolotl_bundle& add_spks(xep0384::axolotl_spks s)      { child(s); return *this; }
            axolotl_bundle& add_ik(xep0384::axolotl_ik i)          { child(i); return *this; }
            axolotl_bundle& add_prekeys(xep0384::axolotl_prekeys p) { child(p); return *this; }
        };

        // ── XEP-0334 store hint (used in OMEMO message wrapper) ──────────────

        // <store xmlns='urn:xmpp:hints'/>
        struct store_hint : virtual public spec {
            store_hint() : spec("store") {
                xmlns<urn::xmpp::hints>();
            }
        };

        // ── xdata helpers (used in publish-options) ──────────────────────────

        // <value>text</value>
        struct xdata_value : virtual public spec {
            explicit xdata_value(std::string_view val) : spec("value") {
                text(val);
            }
        };

        // <field var='…' [type='…']><value>…</value></field>
        struct xdata_field : virtual public spec {
            xdata_field(std::string_view var, std::string_view val,
                        std::string_view type = "") : spec("field")
            {
                attr("var", var);
                if (!type.empty()) attr("type", type);
                xdata_value v(val);
                child(v);
            }
        };

        // <x xmlns='jabber:x:data' type='submit'> fields </x>
        struct xdata_form : virtual public spec {
            xdata_form() : spec("x") {
                attr("xmlns", "jabber:x:data");
                attr("type", "submit");
            }

            xdata_form& add_field(xdata_field f) { child(f); return *this; }
        };
    };

}
