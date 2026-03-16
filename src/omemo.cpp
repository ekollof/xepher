// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <algorithm>
#include <cassert>
#include <array>
#include <charconv>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/core.h>
#include <gcrypt.h>
#include <key_helper.h>
#include <lmdb++.h>
#include <signal_protocol.h>
#include <omemo/protocol.h>
#include <omemo/session_builder.h>
#include <omemo/session_cipher.h>
#include <session_record.h>
#include <session_pre_key.h>
#include <strophe.h>
#include <weechat/weechat-plugin.h>

#include "account.hh"
#include "gcrypt.hh"
#include "omemo.hh"
#include "plugin.hh"
#include "strophe.hh"
#include "xmpp/ns.hh"
#include "xmpp/stanza.hh"

#ifndef NDEBUG
#define OMEMO_ASSERT(condition, message)                                                     \
    do {                                                                                     \
        if (!(condition)) {                                                                  \
            std::fprintf(stderr, "OMEMO assertion failed: %s (%s:%d): %s\n",             \
                         #condition, __FILE__, __LINE__, message);                           \
            assert(condition);                                                               \
        }                                                                                    \
    } while (0)
#else
#define OMEMO_ASSERT(condition, message) do { (void) sizeof(condition); } while (0)
#endif

namespace {

#include "omemo/internal_namespace.inl"

} // namespace

const char *OMEMO_ADVICE = "[OMEMO encrypted message (XEP-0384)]";


#include "omemo/api.inl"
