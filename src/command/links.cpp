// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <strophe.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <iostream>
#include <sstream>
#include <utility>
#include <optional>
#include <memory>
#include <array>
#include <unordered_set>
#include <algorithm>
#include <ranges>
#include <span>
#include <expected>
#include <fmt/core.h>
#include <fmt/chrono.h>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "weechat/runtime_port.hh"
#include "account.hh"
#include "channel.hh"
#include "buffer.hh"
#include "command.hh"
#include "connection/internal.hh"
#include "weechat/ui_port.hh"

#include "command/links.inl"
