// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

// Runtime plugin version strings. GIT_COMMIT is compiled only into version.cpp
// so a new commit does not bust ccache for every translation unit.

namespace weechat {

// Git describe string, or "unknown" when built without VCS metadata.
[[nodiscard]] auto plugin_commit() noexcept -> const char *;

// Full version for display / XEP-0092 / Atom generator, e.g. "0.5.0@v0.11.0-1-gabcdef".
[[nodiscard]] auto plugin_version() noexcept -> const char *;

} // namespace weechat
