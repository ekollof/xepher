// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "message_reply.hh"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fmt/core.h>
#include <memory>
#include <ranges>
#include <weechat/weechat-plugin.h>

#include "color.hh"
#include "plugin.hh"
#include "weechat/runtime_port.hh"
#include "xmpp/message_fallback.hh"

namespace xmpp {

namespace {

constexpr std::array k_og_preview_glyphs{
    std::string_view{"\xE2\x94\x8C"},  // ┌
    std::string_view{"\xE2\x94\x82"},  // │
    std::string_view{"\xE2\x94\x94"},  // └
    std::string_view{"\xE2\x94\x90"},  // ┐
};

constexpr std::string_view k_reply_arrow = "\xE2\x86\xAA";  // ↪

[[nodiscard]] std::string trim_leading_ws(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos)
        return {};
    return std::string(text.substr(first));
}

[[nodiscard]] std::string truncate_at_first_newline(std::string_view text)
{
    if (const auto nl = text.find('\n'); nl != std::string_view::npos)
        return std::string(text.substr(0, nl));
    return std::string(text);
}

}  // namespace

std::optional<MessageReply> parse_message_reply(StanzaView msg)
{
    const StanzaView reply = msg.child("reply", k_reply_ns);
    if (!reply.valid())
        return std::nullopt;

    const std::string target_id = reply.attr_string("id");
    if (target_id.empty())
        return std::nullopt;

    MessageReply parsed;
    parsed.target_id = target_id;
    return parsed;
}

bool is_og_preview_continuation_line(std::string_view plain)
{
    return std::ranges::any_of(k_og_preview_glyphs,
                               [&](std::string_view glyph) { return plain.starts_with(glyph); });
}

std::string strip_leading_reply_chain(std::string_view text)
{
    std::string_view clean_text = text;
    auto pos = clean_text.find(k_reply_arrow);
    while (pos != std::string_view::npos)
    {
        std::string_view after = clean_text.substr(pos + k_reply_arrow.size());
        after.remove_prefix(after.find_first_not_of(' '));
        if (const auto further = after.find(k_reply_arrow); further != std::string_view::npos)
        {
            clean_text = after.substr(further);
            pos = 0;
        }
        else
        {
            clean_text = after;
            break;
        }
    }
    return std::string(clean_text);
}

bool should_truncate_reply_excerpt(std::string_view text)
{
    int newline_count = 0;
    int line_len = 0;
    return std::ranges::any_of(text, [&](char c) {
        if (c == '\n')
        {
            ++newline_count;
            line_len = 0;
        }
        else
            ++line_len;
        return newline_count > 5 || line_len > 200;
    });
}

std::string build_reply_excerpt(std::string_view clean_text)
{
    if (should_truncate_reply_excerpt(clean_text) && clean_text.size() > 40)
        return fmt::format("{}...", clean_text.substr(0, 40));
    return std::string(clean_text);
}

std::optional<std::string> nick_from_line_tags(std::span<const std::string_view> tags)
{
    const auto it = std::ranges::find_if(tags, [](std::string_view tag) {
        return tag.starts_with("nick_");
    });
    if (it == tags.end())
        return std::nullopt;
    return std::string(it->substr(5));
}

std::string extract_line_body_text(std::string_view raw)
{
    if (raw.empty())
        return {};

    if (raw[0] == '\t')
        raw = raw.substr(1);

    std::string owned = truncate_at_first_newline(raw);
    std::unique_ptr<char, decltype(&free)> guard(
        weechat_string_remove_color(owned.c_str(), nullptr), &free);
    std::string plain = guard ? guard.get() : owned;
    plain = trim_leading_ws(plain);
    if (plain.empty() || is_og_preview_continuation_line(plain))
        return {};
    return plain;
}

std::string format_reply_quote_body(std::string_view quote_nick, std::string_view excerpt)
{
    const auto dim = weechat::RuntimePort::default_runtime().xmpp_color("darkgray");
    const auto cyan = weechat::RuntimePort::default_runtime().xmpp_color("cyan");
    const auto reset = weechat::RuntimePort::default_runtime().xmpp_color("resetcolor");
    if (!quote_nick.empty())
    {
        return fmt::format("{}│ {}{}{}: {}{}{}",
                           dim, cyan, quote_nick, dim, dim, excerpt, reset);
    }
    return fmt::format("{}│ {}{}{}", dim, dim, excerpt, reset);
}

}  // namespace xmpp