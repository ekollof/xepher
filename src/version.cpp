// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "version.hh"

#define XMPP_STR(X) #X
#define XMPP_XSTR(X) XMPP_STR(X)

namespace {

#ifdef GIT_COMMIT
constexpr const char k_commit[] = XMPP_XSTR(GIT_COMMIT);
constexpr const char k_version[] = "0.5.0@" XMPP_XSTR(GIT_COMMIT);
#else
constexpr const char k_commit[] = "unknown";
constexpr const char k_version[] = "0.5.0";
#endif

} // namespace

namespace weechat {

auto plugin_commit() noexcept -> const char *
{
    return k_commit;
}

auto plugin_version() noexcept -> const char *
{
    return k_version;
}

} // namespace weechat
