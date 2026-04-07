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
#include <algorithm>
#include <optional>
#include <memory>
#include <array>
#include <fmt/core.h>
#include <fmt/chrono.h>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "xmpp/stanza.hh"
#include "xmpp/node.hh"
#include "xmpp/xep-0054.inl"
#include "xmpp/xep-0292.inl"
#include "account.hh"
#include "user.hh"
#include "avatar.hh"
#include "channel.hh"
#include "buffer.hh"
#include "message.hh"
#include "command.hh"
#include "sexp/driver.hh"
#include "ui/picker.hh"

#define MAM_DEFAULT_DAYS 2
#define STR(X) #X

#include "command/rooms.inl"
