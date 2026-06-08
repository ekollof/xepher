// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <optional>
#include <charconv>
#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>
#include <list>
#include <array>
#include <algorithm>
#include <ranges>
#include <span>
#include <expected>
#include <sstream>
#include <iomanip>
#include <time.h>
#include <stdint.h>
#include <sys/utsname.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <fmt/core.h>
#include <fmt/chrono.h>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "color.hh"
#include "xmpp/node.hh"
#include "xmpp/stanza.hh"
#include "xmpp/atom.hh"
#include "config.hh"
#include "account.hh"
#include "user.hh"
#include "channel.hh"
#include "buffer.hh"
#include "connection.hh"
#include "omemo.hh"
#include "pgp.hh"
#include "util.hh"
#include "avatar.hh"
#include "debug.hh"
#include "message.hh"
#include "connection/internal.hh"
#include "xmpp/chat_state.hh"
#include "xmpp/message_ack.hh"
#include "xmpp/stanza_view.hh"
#include "weechat/line_store.hh"
#include "xmpp/xhtml.hh"
#include "xmpp/xep-0054.inl"
#include "xmpp/xep-0084.inl"
#include "xmpp/xep-0172.inl"
#include "xmpp/xep-0292.inl"

namespace {

struct file_metadata {
    std::string name;
    std::string mime;
    std::string size_raw;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

[[nodiscard]] auto parse_file_metadata(xmpp_stanza_t *file_elem) -> file_metadata
{
    file_metadata meta;
    if (!file_elem)
        return meta;

    if (auto *elem = xmpp_stanza_get_child_by_name(file_elem, "name"))
        meta.name = stanza_element_text(elem);
    if (auto *elem = xmpp_stanza_get_child_by_name(file_elem, "media-type"))
        meta.mime = stanza_element_text(elem);
    if (auto *elem = xmpp_stanza_get_child_by_name(file_elem, "size"))
        meta.size_raw = stanza_element_text(elem);
    if (auto *elem = xmpp_stanza_get_child_by_name(file_elem, "width"))
        if (auto w = parse_uint32(stanza_element_text(elem)); w)
            meta.width = *w;
    if (auto *elem = xmpp_stanza_get_child_by_name(file_elem, "height"))
        if (auto h = parse_uint32(stanza_element_text(elem)); h)
            meta.height = *h;
    return meta;
}

[[nodiscard]] auto format_byte_size(std::string_view size_str) -> std::string
{
    if (size_str.empty())
        return {};
    if (auto sz = parse_int64(size_str); sz)
    {
        const auto n = *sz;
        if (n >= 1024 * 1024)
            return fmt::format("{:.1f} MB", n / 1048576.0);
        if (n >= 1024)
            return fmt::format("{:.1f} KB", n / 1024.0);
        return fmt::format("{} B", n);
    }
    return std::string(size_str);
}

[[nodiscard]] auto format_file_share_suffix(std::string_view name,
                                            std::string_view mime,
                                            std::string_view size_raw,
                                            std::string_view url) -> std::string
{
    std::string suffix = std::string("\n") + weechat_color("cyan") + "[File: ";
    if (!name.empty())
        suffix += name;
    else
        suffix += url;

    if (!mime.empty() || !size_raw.empty())
    {
        suffix += " (";
        if (!mime.empty())
            suffix += mime;
        if (!mime.empty() && !size_raw.empty())
            suffix += ", ";
        if (!size_raw.empty())
            suffix += format_byte_size(size_raw);
        suffix += ")";
    }
    suffix += " " + std::string(url);
    suffix += "]" + std::string(weechat_color("resetcolor"));
    return suffix;
}

[[nodiscard]] auto format_encrypted_file_suffix(std::string_view name,
                                                std::string_view size_raw) -> std::string
{
    std::string suffix = std::string("\n") + weechat_color("cyan") + "[Encrypted file: ";
    suffix += name.empty() ? "(unnamed)" : std::string(name);
    if (!size_raw.empty())
        suffix += " (" + format_byte_size(size_raw) + ")";
    suffix += " — downloading…]" + std::string(weechat_color("resetcolor"));
    return suffix;
}

} // namespace

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "connection/message_handler.inl"
#pragma GCC diagnostic pop
