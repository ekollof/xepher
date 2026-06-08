// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "iq_omemo_pubsub.hh"

#include <cstdint>
#include <fmt/core.h>

namespace xmpp {

namespace {

inline constexpr std::string_view k_pubsub_ns = "http://jabber.org/protocol/pubsub";
inline constexpr std::string_view k_ietf_stanza_ns =
    "urn:ietf:params:xml:ns:xmpp-stanzas";

}  // namespace

bool is_legacy_devicelist_pubsub_node(const std::string_view node)
{
    return node == k_legacy_devicelist_node;
}

bool is_legacy_bundle_pubsub_node(const std::string_view node)
{
    return node.starts_with(k_legacy_bundle_node_prefix);
}

bool iq_has_legacy_devicelist_pubsub_error(const StanzaView iq)
{
    const StanzaView pubsub = iq.child("pubsub", k_pubsub_ns);
    if (!pubsub.valid())
        return false;

    const StanzaView items = pubsub.child("items");
    if (!items.valid())
        return false;

    const std::string node = items.attr_string("node");
    return is_legacy_devicelist_pubsub_node(node);
}

bool iq_error_has_item_not_found(const StanzaView iq)
{
    const StanzaView err = iq.child("error");
    return err.valid() && err.child("item-not-found", k_ietf_stanza_ns).valid();
}

std::string omemo_precondition_retry_node_from_publish_id(
    const std::string_view iq_id,
    const std::uint32_t device_id)
{
    if (iq_id == "omemo-legacy-bundle")
        return fmt::format("{}{}", k_legacy_bundle_node_prefix, device_id);
    if (iq_id == "announce-legacy1")
        return std::string(k_legacy_devicelist_node);
    return {};
}

}  // namespace xmpp