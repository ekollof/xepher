// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

struct t_gui_buffer;

namespace weechat {

class account;

struct icat_preview_request {
    struct t_gui_buffer *buffer = nullptr;
    std::string source;
    size_t width = 0;
    size_t height = 0;
    bool mam_replay = false;
    std::string channel_jid;
    std::string stable_id;
    std::string mime;
};

// Invoke /icat after the message line is printed. During MAM replay, resolves
// HTTP(S) URLs to a local cache file synchronously before calling icat.
void invoke_icat_preview(const icat_preview_request &req, account &acct);

// Blocking download of an image URL into the account image cache directory.
[[nodiscard]] std::expected<std::string, std::string>
download_image_to_cache_sync(account &acct, std::string_view url);

}  // namespace weechat