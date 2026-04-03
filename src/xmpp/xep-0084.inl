// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <strophe.h>

// stanza::* builder types are available via the including translation unit.

namespace weechat::xep0084
{
    // XEP-0084: User Avatar (PEP-based)
    inline constexpr const char *METADATA_NS = "urn:xmpp:avatar:metadata";
    inline constexpr const char *DATA_NS     = "urn:xmpp:avatar:data";

    // Request avatar data from PubSub.
    // Returns a caller-owned xmpp_stanza_t* (call xmpp_stanza_release when done).
    inline xmpp_stanza_t *request_avatar_data(xmpp_ctx_t *context,
                                              const char *to,
                                              const char *id)
    {
        auto sp = stanza::iq()
            .type("get")
            .id(id)
            .to(to)
            .pubsub(
                stanza::xep0060::pubsub().items(
                    stanza::xep0060::items(DATA_NS)
                )
            )
            .build(context);

        xmpp_stanza_clone(sp.get());
        return sp.get();
    }

    // Publish avatar image data to urn:xmpp:avatar:data PEP node.
    // base64_data: base64-encoded raw image bytes (no newlines).
    // hash_id:     hex SHA-1 of the raw image bytes (used as item id).
    // Returns a caller-owned xmpp_stanza_t* (call xmpp_stanza_release when done).
    inline xmpp_stanza_t *publish_avatar_data(xmpp_ctx_t *context,
                                              const char *base64_data,
                                              const char *hash_id)
    {
        // <data xmlns='urn:xmpp:avatar:data'>BASE64</data>
        struct data_payload : virtual public stanza::spec {
            data_payload(const char *b64) : spec("data") {
                xmlns<urn::xmpp::avatar::data>();
                text(b64);
            }
        };

        data_payload dp(base64_data);
        auto sp = stanza::iq()
            .type("set")
            .id(stanza::uuid(context))
            .pubsub(
                stanza::xep0060::pubsub().publish(
                    stanza::xep0060::publish(DATA_NS)
                        .item(stanza::xep0060::item()
                                  .id(hash_id)
                                  .payload(dp))
                )
            )
            .build(context);

        xmpp_stanza_clone(sp.get());
        return sp.get();
    }

    // Publish avatar metadata to urn:xmpp:avatar:metadata PEP node.
    // hash_id:    hex SHA-1 (used as item id and info id attribute).
    // mime_type:  e.g. "image/png" or "image/jpeg".
    // bytes:      raw file size in bytes.
    // width/height: image dimensions (may be 0 if unknown).
    // Returns a caller-owned xmpp_stanza_t* (call xmpp_stanza_release when done).
    inline xmpp_stanza_t *publish_avatar_metadata(xmpp_ctx_t *context,
                                                  const char *hash_id,
                                                  const char *mime_type,
                                                  size_t bytes,
                                                  uint32_t width,
                                                  uint32_t height)
    {
        struct info_spec : virtual public stanza::spec {
            info_spec(const char *id, const char *type,
                      size_t sz, uint32_t w, uint32_t h) : spec("info")
            {
                attr("id",    id);
                attr("type",  type);
                attr("bytes", std::to_string(sz));
                if (w > 0) attr("width",  std::to_string(w));
                if (h > 0) attr("height", std::to_string(h));
            }
        };

        struct metadata_payload : virtual public stanza::spec {
            metadata_payload(const char *id, const char *type,
                             size_t sz, uint32_t w, uint32_t h)
                : spec("metadata")
            {
                xmlns<urn::xmpp::avatar::metadata>();
                info_spec inf(id, type, sz, w, h);
                child(inf);
            }
        };

        metadata_payload mp(hash_id, mime_type, bytes, width, height);
        auto sp = stanza::iq()
            .type("set")
            .id(stanza::uuid(context))
            .pubsub(
                stanza::xep0060::pubsub().publish(
                    stanza::xep0060::publish(METADATA_NS)
                        .item(stanza::xep0060::item()
                                  .id(hash_id)
                                  .payload(mp))
                )
            )
            .build(context);

        xmpp_stanza_clone(sp.get());
        return sp.get();
    }
}
