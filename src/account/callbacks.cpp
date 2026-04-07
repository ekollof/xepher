// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <strophe.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <algorithm>
#include <fmt/core.h>
#include <fmt/ranges.h>
#include <libxml/xmlwriter.h>
#include <libxml/xmlerror.h>
#include <libxml/parser.h>
#include <weechat/weechat-plugin.h>
#include <filesystem>
#include <lmdb++.h>

#include "plugin.hh"
#include "xmpp/node.hh"
#include "xmpp/stanza.hh"
#include "config.hh"
#include "input.hh"
#include "omemo.hh"
#include "account.hh"
#include "connection.hh"
#include "user.hh"
#include "channel.hh"
#include "buffer.hh"
#include "debug.hh"

#include "account/callbacks.inl"
