// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <algorithm>
#include <charconv>
#include <cmath>
#include <expected>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <ctime>
#include <fmt/chrono.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strophe.h>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "weechat/runtime_port.hh"
#include "util.hh"
#include "xmpp/stanza_view.hh"

XMPP_TEST_EXPORT int char_cmp(const void *p1, const void *p2)
{
    return *(const char *)p1 == *(const char *)p2;
}

XMPP_TEST_EXPORT std::expected<std::uint32_t, std::string>
parse_uint32(std::string_view value)
{
    if (value.empty())
        return std::unexpected("empty");
    std::uint32_t parsed = 0;
    const auto *begin = value.data();
    const auto *end = value.data() + value.size();
    const auto [ptr, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc {} || ptr != end)
        return std::unexpected("invalid uint32");
    return parsed;
}

XMPP_TEST_EXPORT std::optional<sm_reconnect_endpoint>
parse_sm_location(std::string_view location)
{
    if (location.empty())
        return std::nullopt;

    std::string_view host;
    std::string_view port_str;

    if (location.starts_with('['))
    {
        const auto close = location.find(']');
        if (close == std::string_view::npos || close + 1 >= location.size()
            || location[close + 1] != ':')
            return std::nullopt;
        host = location.substr(1, close - 1);
        port_str = location.substr(close + 2);
    }
    else
    {
        const auto colon = location.rfind(':');
        if (colon == std::string_view::npos || colon + 1 >= location.size())
            return std::nullopt;
        host = location.substr(0, colon);
        port_str = location.substr(colon + 1);
    }

    if (host.empty())
        return std::nullopt;

    const auto port = parse_uint32(port_str);
    if (!port || *port == 0 || *port > 65535U)
        return std::nullopt;

    return sm_reconnect_endpoint{std::string(host), static_cast<std::uint16_t>(*port)};
}

XMPP_TEST_EXPORT std::expected<std::int64_t, std::string>
parse_int64(std::string_view value)
{
    if (value.empty())
        return std::unexpected("empty");
    std::int64_t parsed = 0;
    const auto *begin = value.data();
    const auto *end = value.data() + value.size();
    const auto [ptr, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc {} || ptr != end)
        return std::unexpected("invalid int64");
    return parsed;
}

XMPP_TEST_EXPORT std::string format_local_timestamp(std::time_t t)
{
    std::tm lt {};
    if (!localtime_r(&t, &lt))
        return {};
    return fmt::format("{:%Y-%m-%d %H:%M}", lt);
}

XMPP_TEST_EXPORT std::string format_utc_timestamp(std::time_t t)
{
    return fmt::format("{:%Y-%m-%dT%H:%M:%SZ}", fmt::gmtime(t));
}

XMPP_TEST_EXPORT std::string unescape(std::string_view str)
{
    std::string result;
    result.reserve(str.size());
    std::size_t i = 0;
    while (i < str.size())
    {
        if (i + 3 < str.size() && str[i] == '&' && str[i + 1] == '#')
        {
            std::size_t j = i + 2;
            while (j < str.size() && str[j] >= '0' && str[j] <= '9')
                ++j;
            if (j < str.size() && str[j] == ';' && j > i + 2)
            {
                if (auto val = parse_uint32(str.substr(i + 2, j - (i + 2))); val && *val <= 0xffU)
                    result += static_cast<char>(*val);
                i = j + 1;
                continue;
            }
        }
        result += str[i++];
    }
    return result;
}

// XEP-0393: Message Styling
// Converts XEP-0393 markup to WeeChat color codes
XMPP_TEST_EXPORT std::string apply_xep393_styling(std::string_view text)
{
    if (text.empty()) return std::string(text);
    
    std::string result;
    result.reserve(text.size() * 2); // Reserve extra space for color codes
    
    size_t i = 0;
    bool in_preblock = false;
    
    while (i < text.length())
    {
        // Handle preformatted blocks (```)
        if (!in_preblock && i + 2 < text.length() && 
            text[i] == '`' && text[i+1] == '`' && text[i+2] == '`')
        {
            in_preblock = true;
            result += weechat::RuntimePort::default_runtime().color("gray");
            i += 3;
            continue;
        }
        else if (in_preblock && i + 2 < text.length() && 
                 text[i] == '`' && text[i+1] == '`' && text[i+2] == '`')
        {
            in_preblock = false;
            result += weechat::RuntimePort::default_runtime().color("resetcolor");
            i += 3;
            continue;
        }
        
        // Inside preformatted block, no styling
        if (in_preblock)
        {
            result += text[i++];
            continue;
        }
        
        // Block quote (> at start of line)
        if ((i == 0 || text[i-1] == '\n') && text[i] == '>' && 
            (i + 1 >= text.length() || text[i+1] == ' '))
        {
            result += weechat::RuntimePort::default_runtime().color("green");
            result += '>';
            i++;
            // Continue until end of line
            while (i < text.length() && text[i] != '\n')
            {
                result += text[i++];
            }
            result += weechat::RuntimePort::default_runtime().color("resetcolor");
            if (i < text.length())
                result += text[i++]; // Add the newline
            continue;
        }
        
        char ch = text[i];
        
        // Check for inline styling markers
        // Rules: must have whitespace or punctuation before, non-whitespace after
        bool can_start = (i == 0 || isspace(text[i-1]) || ispunct(text[i-1]));
        
        if (can_start && i + 1 < text.length())
        {
            // *strong* (bold)
            if (ch == '*' && !isspace(text[i+1]))
            {
                size_t end = i + 1;
                // XEP-0393: spans MUST NOT cross line boundaries
                while (end < text.length() && text[end] != '*' && text[end] != '\n')
                    end++;
                
                if (end < text.length() && text[end] == '*' && !isspace(text[end-1]) &&
                    (end + 1 >= text.length() || isspace(text[end+1]) || ispunct(text[end+1])))
                {
                    result += weechat::RuntimePort::default_runtime().color("bold");
                    i++;
                    while (i < end)
                        result += text[i++];
                    result += weechat::RuntimePort::default_runtime().color("-bold");
                    i++; // Skip closing *
                    continue;
                }
            }
            
            // _emphasis_ (italic/underline)
            else if (ch == '_' && !isspace(text[i+1]))
            {
                size_t end = i + 1;
                // XEP-0393: spans MUST NOT cross line boundaries
                while (end < text.length() && text[end] != '_' && text[end] != '\n')
                    end++;
                
                if (end < text.length() && text[end] == '_' && !isspace(text[end-1]) &&
                    (end + 1 >= text.length() || isspace(text[end+1]) || ispunct(text[end+1])))
                {
                    result += weechat::RuntimePort::default_runtime().color("underline");
                    i++;
                    while (i < end)
                        result += text[i++];
                    result += weechat::RuntimePort::default_runtime().color("-underline");
                    i++; // Skip closing _
                    continue;
                }
            }
            
            // `monospace` (inline code)
            else if (ch == '`' && !isspace(text[i+1]))
            {
                size_t end = i + 1;
                // XEP-0393: spans MUST NOT cross line boundaries
                while (end < text.length() && text[end] != '`' && text[end] != '\n')
                    end++;
                
                if (end < text.length() && text[end] == '`' && !isspace(text[end-1]) &&
                    (end + 1 >= text.length() || isspace(text[end+1]) || ispunct(text[end+1])))
                {
                    result += weechat::RuntimePort::default_runtime().color("gray");
                    i++;
                    while (i < end)
                        result += text[i++];
                    result += weechat::RuntimePort::default_runtime().color("resetcolor");
                    i++; // Skip closing `
                    continue;
                }
            }
            
            // ~strikethrough~ (XEP-0393 §6.2.4 — single tilde U+007E)
            else if (ch == '~' && i + 1 < text.length() && !isspace(text[i+1]))
            {
                size_t end = i + 1;
                // XEP-0393: spans MUST NOT cross line boundaries; look for closing ~
                while (end < text.length() &&
                       text[end] != '~' &&
                       text[end] != '\n')
                    end++;

                if (end < text.length() && text[end] == '~' &&
                    end > i + 1 &&          // at least one char between tildes
                    !isspace(text[end-1]) &&
                    (end + 1 >= text.length() || isspace(text[end+1]) || ispunct(text[end+1])))
                {
                    result += weechat::RuntimePort::default_runtime().color("red");
                    i += 1; // Skip opening ~
                    while (i < end)
                        result += text[i++];
                    result += weechat::RuntimePort::default_runtime().color("resetcolor");
                    i += 1; // Skip closing ~
                    continue;
                }
            }
        }
        
        // No styling matched, just copy character
        result += text[i++];
    }
    
    return result;
}

// XEP-0394: Message Markup (receive-only)
// Applies <markup xmlns='urn:xmpp:markup:0'> to `plain_text`, returning a
// WeeChat colour-coded string.  Returns empty string if no <markup> child
// exists in `stanza`.
XMPP_TEST_EXPORT std::string apply_xep394_markup(
    const xmpp::StanzaView stanza,
    const std::string_view plain_text)
{
    if (!stanza.valid() || plain_text.empty())
        return {};

    const xmpp::StanzaView markup_elem = stanza.child("markup", "urn:xmpp:markup:0");
    if (!markup_elem.valid())
        return {};

    // Build a table mapping unicode-codepoint index → UTF-8 byte offset.
    // XEP-0394 start/end are in unicode codepoints; we need byte positions.
    std::vector<std::size_t> cp_to_byte; // cp_to_byte[cp] = byte offset
    cp_to_byte.reserve(plain_text.size() + 1);
    {
        std::size_t byte = 0;
        while (byte <= plain_text.size())
        {
            cp_to_byte.push_back(byte);
            if (byte == plain_text.size()) break;
            // Advance one UTF-8 codepoint
            unsigned char c = static_cast<unsigned char>(plain_text[byte]);
            std::size_t len = 1;
            if      ((c & 0x80) == 0x00) len = 1;
            else if ((c & 0xE0) == 0xC0) len = 2;
            else if ((c & 0xF0) == 0xE0) len = 3;
            else if ((c & 0xF8) == 0xF0) len = 4;
            byte += len;
        }
    }
    std::size_t total_cps [[maybe_unused]] = cp_to_byte.size() - 1; // last entry is end-sentinel

    auto cp_byte = [&](long cp) -> std::size_t {
        if (cp < 0) return 0;
        auto idx = static_cast<std::size_t>(cp);
        if (idx >= cp_to_byte.size()) return plain_text.size();
        return cp_to_byte[idx];
    };

    // Events: at a given byte offset, insert a WeeChat color string.
    // We sort by offset then by priority (open before close at same position).
    struct Event {
        std::size_t byte_off;
        int         priority; // lower = applied first
        std::string code;
    };
    std::vector<Event> events;

    // Helper to get start/end codepoint attributes
    const auto get_long_attr = [](const xmpp::StanzaView el,
                                  const std::string_view attr,
                                  const long fallback) -> long {
        const std::string v = el.attr_string(attr);
        if (v.empty())
            return fallback;
        if (auto val = parse_int64(v); val)
            return static_cast<long>(*val);
        return fallback;
    };

    for (const xmpp::StanzaView child : markup_elem)
    {
        const std::string_view child_name_sv = child.name();
        if (child_name_sv.empty())
            continue;

        if (child_name_sv == "span")
        {
            const long start = get_long_attr(child, "start", -1);
            const long end   = get_long_attr(child, "end", -1);
            if (start < 0 || end <= start) continue;

            // Determine which style this span applies (first child wins)
            std::string open_code, close_code;
            for (const xmpp::StanzaView sp : child)
            {
                const std::string_view sp_name = sp.name();
                if (sp_name.empty()) continue;
                if (sp_name == "emphasis") {
                    open_code  = weechat::RuntimePort::default_runtime().color("italic");
                    close_code = weechat::RuntimePort::default_runtime().color("-italic");
                } else if (sp_name == "strong") {
                    open_code  = weechat::RuntimePort::default_runtime().color("bold");
                    close_code = weechat::RuntimePort::default_runtime().color("-bold");
                } else if (sp_name == "code") {
                    open_code  = weechat::RuntimePort::default_runtime().color("cyan");
                    close_code = weechat::RuntimePort::default_runtime().color("resetcolor");
                } else if (sp_name == "deleted") {
                    open_code  = weechat::RuntimePort::default_runtime().color("8");   // dark grey ≈ strikethrough hint
                    close_code = weechat::RuntimePort::default_runtime().color("resetcolor");
                }
                if (!open_code.empty()) break;
            }
            if (open_code.empty()) continue;

            events.push_back({cp_byte(start), 0, std::move(open_code)});
            events.push_back({cp_byte(end),   1, std::move(close_code)});
        }
        else if (child_name_sv == "bcode")
        {
            long start = get_long_attr(child, "start", -1);
            long end   = get_long_attr(child, "end",   -1);
            if (start < 0 || end <= start) continue;
            events.push_back({cp_byte(start), 0, weechat::RuntimePort::default_runtime().color("gray")});
            events.push_back({cp_byte(end),   1, weechat::RuntimePort::default_runtime().color("resetcolor")});
        }
        else if (child_name_sv == "bquote")
        {
            long start = get_long_attr(child, "start", -1);
            long end   = get_long_attr(child, "end",   -1);
            if (start < 0 || end <= start) continue;
            // Insert green color at start of each line within [start, end)
            events.push_back({cp_byte(start), 0, weechat::RuntimePort::default_runtime().color("green")});
            // Also emit green after every newline within the range
            std::size_t sb = cp_byte(start);
            std::size_t eb = cp_byte(end);
            for (std::size_t b = sb; b < eb && b < plain_text.size(); ++b)
            {
                if (plain_text[b] == '\n' && b + 1 < eb)
                    events.push_back({b + 1, 0, weechat::RuntimePort::default_runtime().color("green")});
            }
            events.push_back({cp_byte(end), 1, weechat::RuntimePort::default_runtime().color("resetcolor")});
        }
        else if (child_name_sv == "list")
        {
            // Each <li start="N"/> inserts a bullet marker at that codepoint.
            for (const xmpp::StanzaView li : child)
            {
                if (li.name() != "li") continue;
                const long li_start = get_long_attr(li, "start", -1);
                if (li_start < 0) continue;
                // Insert "• " before the list item start
                events.push_back({cp_byte(li_start), -1, "• "});
            }
        }
    }

    if (events.empty()) return std::string(plain_text); // markup present but no actionable elements

    // Sort events: by byte offset, then by priority
    std::ranges::sort(events, [](const Event &a, const Event &b) {
        if (a.byte_off != b.byte_off) return a.byte_off < b.byte_off;
        return a.priority < b.priority;
    });

    // Build output
    std::string result;
    result.reserve(plain_text.size() + events.size() * 8);
    std::size_t pos = 0;
    for (const auto &ev : events)
    {
        if (ev.byte_off > pos && ev.byte_off <= plain_text.size())
        {
            result += plain_text.substr(pos, ev.byte_off - pos);
            pos = ev.byte_off;
        }
        result += ev.code;
    }
    if (pos < plain_text.size())
        result += plain_text.substr(pos);

    return result;
}

// Markdown renderer for Atom feed plain-text content.
//
// Handles a barebones subset of Markdown and maps it to WeeChat colour/
// attribute codes so that plain-text microblog posts authored in Markdown
// (Movim, Libervia, etc.) are displayed with visual formatting in WeeChat.
//
// Block-level constructs are identified at the start of each line.
// Inline spans are processed left-to-right within non-code lines.
// Images are matched before links because the pattern is longer.
// No support for nested lists, tables, footnotes, or raw HTML pass-through.
std::string apply_markdown_to_weechat(std::string_view text)
{
    if (text.empty()) return std::string(text);

    // Split input into lines (preserving trailing newline awareness).
    std::vector<std::string> lines;
    {
        std::istringstream ss{std::string(text)};
        std::string line;
        while (std::getline(ss, line))
            lines.push_back(line);
    }

    std::string result;
    result.reserve(text.size() * 2);

    const char *bold       = weechat::RuntimePort::default_runtime().color("bold");
    const char *bold_off   = weechat::RuntimePort::default_runtime().color("-bold");
    const char *italic     = weechat::RuntimePort::default_runtime().color("italic");
    const char *italic_off = weechat::RuntimePort::default_runtime().color("-italic");
    const char *gray       = weechat::RuntimePort::default_runtime().color("gray");
    const char *darkgray   = weechat::RuntimePort::default_runtime().color("darkgray");
    const char *green      = weechat::RuntimePort::default_runtime().color("green");
    const char *blue       = weechat::RuntimePort::default_runtime().color("blue");
    const char *dim        = weechat::RuntimePort::default_runtime().color("darkgray");
    const char *rst        = weechat::RuntimePort::default_runtime().color("resetcolor");

    // Helper: apply inline spans to a single line of text.
    // Processes: images, links, **bold**/__bold__, *italic*/_italic_,
    // `code`, ~~strikethrough~~.
    // `in_code_block` suppresses inline processing inside fenced blocks.
    auto apply_inline = [&](std::string_view ln) -> std::string
    {
        std::string out;
        out.reserve(ln.size() * 2);
        size_t i = 0;
        const size_t n = ln.size();

        while (i < n)
        {
            // ![alt](url) — image (must come before link check)
            if (i + 1 < n && ln[i] == '!' && ln[i+1] == '[')
            {
                size_t alt_start = i + 2;
                size_t alt_end   = ln.find(']', alt_start);
                if (alt_end != std::string::npos && alt_end + 1 < n && ln[alt_end+1] == '(')
                {
                    size_t url_start = alt_end + 2;
                    size_t url_end   = ln.find(')', url_start);
                    if (url_end != std::string::npos)
                    {
                        std::string alt = std::string(ln.substr(alt_start, alt_end - alt_start));
                        out += dim;
                        out += alt.empty() ? "[Image]" : "[Image: " + alt + "]";
                        out += rst;
                        i = url_end + 1;
                        continue;
                    }
                }
            }

            // [text](url) — link
            if (ln[i] == '[')
            {
                size_t text_start = i + 1;
                size_t text_end   = ln.find(']', text_start);
                if (text_end != std::string::npos && text_end + 1 < n && ln[text_end+1] == '(')
                {
                    size_t url_start = text_end + 2;
                    size_t url_end   = ln.find(')', url_start);
                    if (url_end != std::string::npos)
                    {
                        std::string link_text = std::string(ln.substr(text_start, text_end - text_start));
                        std::string url       = std::string(ln.substr(url_start, url_end - url_start));
                        out += link_text;
                        out += " (";
                        out += blue;
                        out += url;
                        out += rst;
                        out += ")";
                        i = url_end + 1;
                        continue;
                    }
                }
            }

            // **bold** or __bold__
            if (i + 1 < n && ((ln[i] == '*' && ln[i+1] == '*') ||
                               (ln[i] == '_' && ln[i+1] == '_')))
            {
                char marker = ln[i];
                size_t end = ln.find(std::string{marker, marker}, i + 2);
                if (end != std::string::npos)
                {
                    out += bold;
                    out += ln.substr(i + 2, end - (i + 2));
                    out += bold_off;
                    i = end + 2;
                    continue;
                }
            }

            // ~~strikethrough~~
            if (i + 1 < n && ln[i] == '~' && ln[i+1] == '~')
            {
                size_t end = ln.find("~~", i + 2);
                if (end != std::string::npos)
                {
                    out += darkgray;
                    out += ln.substr(i + 2, end - (i + 2));
                    out += rst;
                    i = end + 2;
                    continue;
                }
            }

            // `inline code`
            if (ln[i] == '`' && (i + 1 >= n || ln[i+1] != '`'))
            {
                size_t end = ln.find('`', i + 1);
                if (end != std::string::npos)
                {
                    out += gray;
                    out += ln.substr(i + 1, end - (i + 1));
                    out += rst;
                    i = end + 1;
                    continue;
                }
            }

            // *italic* or _italic_ (single marker, not already consumed as **)
            if ((ln[i] == '*' || ln[i] == '_') &&
                (i == 0 || isspace((unsigned char)ln[i-1]) || ispunct((unsigned char)ln[i-1])) &&
                i + 1 < n && !isspace((unsigned char)ln[i+1]))
            {
                char marker = ln[i];
                size_t end = i + 1;
                while (end < n && ln[end] != marker && ln[end] != '\n')
                    ++end;
                if (end < n && ln[end] == marker && !isspace((unsigned char)ln[end-1]) &&
                    (end + 1 >= n || isspace((unsigned char)ln[end+1]) || ispunct((unsigned char)ln[end+1])))
                {
                    out += italic;
                    out += ln.substr(i + 1, end - (i + 1));
                    out += italic_off;
                    i = end + 1;
                    continue;
                }
            }

            out += ln[i++];
        }
        return out;
    };

    bool in_code_block = false;

    for (const auto &ln : lines)
    {

        // --- Fenced code block delimiter (``` at start of line) ---
        if (ln.size() >= 3 && ln[0] == '`' && ln[1] == '`' && ln[2] == '`')
        {
            in_code_block = !in_code_block;
            if (in_code_block)
                result += gray;
            else
                result += rst;
            // Optionally show the language hint (characters after the ```)
            // but don't render it prominently — just append dimly.
            std::string lang = ln.substr(3);
            // Trim whitespace
            auto s = lang.find_first_not_of(" \t");
            if (s != std::string::npos)
            {
                lang = lang.substr(s);
                result += dim;
                result += "[" + lang + "]";
                result += (in_code_block ? gray : rst);
            }
            result += '\n';
            continue;
        }

        // Inside a fenced code block — pass through verbatim (already gray).
        if (in_code_block)
        {
            result += ln;
            result += '\n';
            continue;
        }

        // --- Horizontal rule: ---, ***, or ___ (3 or more, whole line) ---
        {
            bool is_hr = false;
            if (ln.size() >= 3)
            {
                char c = ln[0];
                if (c == '-' || c == '*' || c == '_')
                {
                    is_hr = true;
                    for (char ch : ln)
                        if (ch != c && ch != ' ') { is_hr = false; break; }
                }
            }
            if (is_hr)
            {
                result += dim;
                result += "────────────────────────────────────────";
                result += rst;
                result += '\n';
                continue;
            }
        }

        // --- Headings: # H1, ## H2, ### H3 ---
        {
            size_t hashes = 0;
            while (hashes < ln.size() && ln[hashes] == '#')
                ++hashes;
            if (hashes >= 1 && hashes <= 6 &&
                hashes < ln.size() && ln[hashes] == ' ')
            {
                std::string heading = ln.substr(hashes + 1);
                result += bold;
                result += apply_inline(heading);
                result += bold_off;
                result += '\n';
                continue;
            }
        }

        // --- Blockquote: > text ---
        if (!ln.empty() && ln[0] == '>' &&
            (ln.size() == 1 || ln[1] == ' ' || ln[1] == '>'))
        {
            // Strip the leading "> " and recurse for nested quotes
            std::string inner = (ln.size() > 2 && ln[1] == ' ') ? ln.substr(2)
                              : (ln.size() > 1)                  ? ln.substr(1)
                              : "";
            result += green;
            result += "> ";
            result += rst;
            result += apply_inline(inner);
            result += '\n';
            continue;
        }

        // --- Unordered list: - item or * item (not *** which is handled above) ---
        if (ln.size() >= 2 &&
            (ln[0] == '-' || ln[0] == '*') &&
            ln[1] == ' ')
        {
            result += "• ";
            result += apply_inline(ln.substr(2));
            result += '\n';
            continue;
        }

        // --- Ordered list: 1. item ---
        {
            size_t j = 0;
            while (j < ln.size() && isdigit((unsigned char)ln[j]))
                ++j;
            if (j > 0 && j < ln.size() && ln[j] == '.' &&
                j + 1 < ln.size() && ln[j+1] == ' ')
            {
                result += ln.substr(0, j);   // keep the number
                result += ". ";
                result += apply_inline(ln.substr(j + 2));
                result += '\n';
                continue;
            }
        }

        // --- Plain line: apply inline spans ---
        result += apply_inline(ln);
        result += '\n';
    }

    // Trim trailing newline added by the loop (the original text may or may
    // not have ended with one; we normalise to match the input).
    if (!result.empty() && result.back() == '\n' &&
        (text.empty() || text.back() != '\n'))
        result.pop_back();

    return result;
}

XMPP_TEST_EXPORT bool is_image_mime_type(std::string_view mime)
{
    return mime.starts_with("image/");
}

XMPP_TEST_EXPORT std::pair<size_t, size_t> read_image_dimensions(const char *path)
{
    auto deleter = [](FILE *f) { if (f) fclose(f); };
    std::unique_ptr<FILE, decltype(deleter)> file(fopen(path, "rb"), deleter);
    if (!file)
        return {0, 0};

    unsigned char hdr[24] = {};
    size_t hdr_read = fread(hdr, 1, sizeof(hdr), file.get());

    // PNG: IHDR chunk starts at byte 8
    if (hdr_read >= 24
        && hdr[0] == 0x89 && hdr[1] == 'P' && hdr[2] == 'N' && hdr[3] == 'G'
        && hdr[4] == 0x0D && hdr[5] == 0x0A && hdr[6] == 0x1A && hdr[7] == 0x0A)
    {
        size_t w = (static_cast<size_t>(hdr[16]) << 24)
                 | (static_cast<size_t>(hdr[17]) << 16)
                 | (static_cast<size_t>(hdr[18]) <<  8)
                 |  static_cast<size_t>(hdr[19]);
        size_t h = (static_cast<size_t>(hdr[20]) << 24)
                 | (static_cast<size_t>(hdr[21]) << 16)
                 | (static_cast<size_t>(hdr[22]) <<  8)
                 |  static_cast<size_t>(hdr[23]);
        return {w, h};
    }

    // JPEG: scan for SOF0/SOF1/SOF2 markers (0xFF 0xC0-0xC2)
    if (hdr_read >= 3 && hdr[0] == 0xFF && hdr[1] == 0xD8 && hdr[2] == 0xFF)
    {
        unsigned char buf[65536];
        fseek(file.get(), 2, SEEK_SET);
        size_t total = fread(buf, 1, sizeof(buf), file.get());
        for (size_t i = 0; i + 8 < total; ++i)
        {
            if (buf[i] == 0xFF && (buf[i+1] == 0xC0 || buf[i+1] == 0xC1 || buf[i+1] == 0xC2))
            {
                size_t h = (static_cast<size_t>(buf[i+5]) << 8) | static_cast<size_t>(buf[i+6]);
                size_t w = (static_cast<size_t>(buf[i+7]) << 8) | static_cast<size_t>(buf[i+8]);
                return {w, h};
            }
        }
    }

    return {0, 0};
}

std::string icat_dimension_args(size_t pixel_width, size_t pixel_height)
{
    constexpr size_t default_columns = 40;
    constexpr size_t max_rows = 20;

    size_t rows;
    if (pixel_width == 0 || pixel_height == 0)
    {
        // No dimensions known — use a sensible default so -print_immediately works
        rows = 10;
    }
    else
    {
        rows = std::max(size_t(1),
            static_cast<size_t>(std::round(
                static_cast<double>(default_columns) * pixel_height / pixel_width)));
        if (rows > max_rows)
            rows = max_rows;
    }

    return fmt::format(" -columns {} -rows {}", default_columns, rows);
}
