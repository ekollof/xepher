// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace xmpp {

struct disco_identity {
    std::string category;
    std::string type;
    std::string name;
};

struct discovered_component {
    std::string jid;
    std::vector<disco_identity> identities;
    std::vector<std::string> features;
};

struct server_capabilities {
    std::string domain;
    std::vector<disco_identity> domain_identities;
    std::vector<std::string> domain_features;
    std::vector<discovered_component> components;

    bool sm_available = true;
    bool csi_available = true;
    bool sm_enabled = false;
    bool sm_cached_disabled = false;
    bool csi_cached_disabled = false;

    bool carbons_advertised = false;
    bool upload_discovered = false;
    std::string upload_jid;
    std::size_t upload_max_bytes = 0;
    std::vector<std::string> pubsub_services;
    std::vector<std::string> pubsub_mam_services;
};

}  // namespace xmpp