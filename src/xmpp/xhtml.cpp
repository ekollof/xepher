// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// XEP-0071: XHTML-IM shared rendering utilities.
// Extracted from connection/presence_handler.inl so that atom.cpp (and any
// other translation unit) can render XHTML stanza content and strip raw HTML.

#include "xhtml.hh"

#include <array>
#include <cctype>
#include <cstring>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include <strophe.h>
#include <weechat/weechat-plugin.h>

#include "../plugin.hh"

// ---------------------------------------------------------------------------
// css_color_to_weechat
// ---------------------------------------------------------------------------

std::string css_color_to_weechat(const std::string &css)
{
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 20> k_color_table = {{
        {"black",    "black"},
        {"white",    "white"},
        {"red",      "red"},
        {"green",    "green"},
        {"blue",     "blue"},
        {"yellow",   "yellow"},
        {"cyan",     "cyan"},
        {"magenta",  "magenta"},
        {"orange",   "214"},
        {"gray",     "gray"},
        {"grey",     "gray"},
        {"darkgray", "darkgray"},
        {"darkgrey", "darkgray"},
        {"purple",   "magenta"},
        {"pink",     "213"},
        {"brown",    "130"},
        {"lime",     "46"},
        {"teal",     "30"},
        {"navy",     "18"},
        {"silver",   "250"},
    }};

    std::string lc = css;
    for (auto &c : lc) c = (char)tolower((unsigned char)c);
    auto s = lc.find_first_not_of(" \t");
    auto e = lc.find_last_not_of(" \t");
    if (s == std::string::npos) return "";
    lc = lc.substr(s, e - s + 1);

    auto it = std::ranges::find_if(k_color_table,
        [&](const auto &entry) { return entry.first == lc; });
    if (it != k_color_table.end()) return std::string(it->second);

    if (lc.size() >= 4 && lc[0] == '#')
    {
        unsigned r = 0, g = 0, b = 0;
        if (lc.size() == 7)
        {
            r = std::stoul(lc.substr(1,2), nullptr, 16);
            g = std::stoul(lc.substr(3,2), nullptr, 16);
            b = std::stoul(lc.substr(5,2), nullptr, 16);
        }
        else if (lc.size() == 4)
        {
            r = std::stoul(lc.substr(1,1), nullptr, 16) * 17;
            g = std::stoul(lc.substr(2,1), nullptr, 16) * 17;
            b = std::stoul(lc.substr(3,1), nullptr, 16) * 17;
        }
        if (r > 200 && g < 100 && b < 100) return "red";
        if (r < 100 && g > 150 && b < 100) return "green";
        if (r < 100 && g < 100 && b > 150) return "blue";
        if (r > 150 && g > 150 && b < 100) return "yellow";
        if (r < 100 && g > 150 && b > 150) return "cyan";
        if (r > 150 && g < 100 && b > 150) return "magenta";
        if (r > 180 && g > 120 && b < 80)  return "214";
        if (r > 180 && g > 180 && b > 180) return "white";
        if (r < 80  && g < 80  && b < 80)  return "black";
        if (r > 100 && g > 100 && b > 100) return "gray";
    }
    return "";
}

// ---------------------------------------------------------------------------
// css_style_to_weechat
// ---------------------------------------------------------------------------

std::pair<std::string, std::string> css_style_to_weechat(const char *style)
{
    if (!style) return {"", ""};

    std::string open, close;
    std::string s(style);

    size_t pos = 0;
    while (pos < s.size())
    {
        size_t semi = s.find(';', pos);
        std::string decl = s.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
        pos = (semi == std::string::npos) ? s.size() : semi + 1;

        size_t colon = decl.find(':');
        if (colon == std::string::npos) continue;
        std::string prop = decl.substr(0, colon);
        std::string val  = decl.substr(colon + 1);

        auto trim = [](std::string &str) {
            auto a = str.find_first_not_of(" \t");
            if (a == std::string::npos) { str.clear(); return; }
            auto b = str.find_last_not_of(" \t");
            str = str.substr(a, b - a + 1);
            for (auto &c : str) c = (char)tolower((unsigned char)c);
        };
        trim(prop);
        trim(val);

        if (prop == "color")
        {
            std::string wc = css_color_to_weechat(val);
            if (!wc.empty())
            {
                open  += weechat_color(wc.c_str());
                close  = std::string(weechat_color("resetcolor")) + close;
            }
        }
        else if (prop == "font-weight" && val == "bold")
        {
            open  += weechat_color("bold");
            close  = std::string(weechat_color("-bold")) + close;
        }
        else if (prop == "font-style" && val == "italic")
        {
            open  += weechat_color("italic");
            close  = std::string(weechat_color("-italic")) + close;
        }
        else if (prop == "text-decoration" && val == "underline")
        {
            open  += weechat_color("underline");
            close  = std::string(weechat_color("-underline")) + close;
        }
        // background-color: intentionally ignored
    }
    return {open, close};
}

// ---------------------------------------------------------------------------
// xhtml_to_weechat
// ---------------------------------------------------------------------------

std::string xhtml_to_weechat(xmpp_stanza_t *stanza, bool in_blockquote)
{
    std::string result;
    for (xmpp_stanza_t *child = xmpp_stanza_get_children(stanza);
         child; child = xmpp_stanza_get_next(child))
    {
        if (xmpp_stanza_is_text(child))
        {
            const char *raw = xmpp_stanza_get_text_ptr(child);
            if (!raw) continue;
            if (in_blockquote)
            {
                std::string txt(raw);
                std::string line;
                for (char c : txt)
                {
                    if (c == '\n')
                    {
                        result += weechat_color("green");
                        result += "> ";
                        result += weechat_color("resetcolor");
                        result += line;
                        result += '\n';
                        line.clear();
                    }
                    else line += c;
                }
                if (!line.empty())
                {
                    result += weechat_color("green");
                    result += "> ";
                    result += weechat_color("resetcolor");
                    result += line;
                }
            }
            else
            {
                result += raw;
            }
            continue;
        }

        const char *name = xmpp_stanza_get_name(child);
        if (!name) continue;

        std::string_view name_sv(name);

        bool is_block = (name_sv == "p"
                      || name_sv == "div"
                      || name_sv == "li");
        bool is_br         = (name_sv == "br");
        bool is_blockquote = (name_sv == "blockquote");
        bool is_pre        = (name_sv == "pre");

        if (is_br)
        {
            result += '\n';
            if (in_blockquote)
            {
                result += weechat_color("green");
                result += "> ";
                result += weechat_color("resetcolor");
            }
            continue;
        }

        if (is_blockquote)
        {
            if (!result.empty() && result.back() != '\n') result += '\n';
            std::string inner = xhtml_to_weechat(child, true);
            result += weechat_color("green");
            result += "> ";
            result += weechat_color("resetcolor");
            result += inner;
            if (!result.empty() && result.back() != '\n') result += '\n';
            continue;
        }

        if (is_pre)
        {
            if (!result.empty() && result.back() != '\n') result += '\n';
            result += weechat_color("gray");
            result += xhtml_to_weechat(child, in_blockquote);
            result += weechat_color("resetcolor");
            if (!result.empty() && result.back() != '\n') result += '\n';
            continue;
        }

        if (is_block)
        {
            if (!result.empty() && result.back() != '\n') result += '\n';
            result += xhtml_to_weechat(child, in_blockquote);
            if (!result.empty() && result.back() != '\n') result += '\n';
            continue;
        }

        if (name_sv == "b" || name_sv == "strong")
        {
            result += weechat_color("bold");
            result += xhtml_to_weechat(child, in_blockquote);
            result += weechat_color("-bold");
            continue;
        }

        if (name_sv == "i" || name_sv == "em")
        {
            result += weechat_color("italic");
            result += xhtml_to_weechat(child, in_blockquote);
            result += weechat_color("-italic");
            continue;
        }

        if (name_sv == "u")
        {
            result += weechat_color("underline");
            result += xhtml_to_weechat(child, in_blockquote);
            result += weechat_color("-underline");
            continue;
        }

        if (name_sv == "del" || name_sv == "s" || name_sv == "strike")
        {
            result += weechat_color("darkgray");
            result += xhtml_to_weechat(child, in_blockquote);
            result += weechat_color("resetcolor");
            continue;
        }

        if (name_sv == "code" || name_sv == "tt")
        {
            result += weechat_color("gray");
            result += xhtml_to_weechat(child, in_blockquote);
            result += weechat_color("resetcolor");
            continue;
        }

        if (name_sv == "span")
        {
            const char *style_attr = xmpp_stanza_get_attribute(child, "style");
            auto [open, close] = css_style_to_weechat(style_attr);
            result += open;
            result += xhtml_to_weechat(child, in_blockquote);
            result += close;
            continue;
        }

        if (name_sv == "a")
        {
            const char *href = xmpp_stanza_get_attribute(child, "href");
            std::string link_text = xhtml_to_weechat(child, in_blockquote);
            if (href && *href)
            {
                auto sanitize_url = [](const char *url) -> std::string {
                    std::string out;
                    for (const char *p = url; *p; ++p) {
                        unsigned char c = static_cast<unsigned char>(*p);
                        if (c >= 0x20 || c == 0x09 || c == 0x0A)
                            out += static_cast<char>(c);
                    }
                    return out;
                };
                std::string safe_href = sanitize_url(href);
                if (link_text == safe_href)
                {
                    result += weechat_color("blue");
                    result += safe_href;
                    result += weechat_color("resetcolor");
                }
                else
                {
                    result += link_text;
                    result += ' ';
                    result += weechat_color("blue");
                    result += '(';
                    result += safe_href;
                    result += ')';
                    result += weechat_color("resetcolor");
                }
            }
            else
            {
                result += link_text;
            }
            continue;
        }

        if (name_sv == "img")
        {
            const char *alt = xmpp_stanza_get_attribute(child, "alt");
            result += weechat_color("darkgray");
            result += '[';
            result += (alt && *alt) ? alt : "image";
            result += ']';
            result += weechat_color("resetcolor");
            continue;
        }

        // Fallback: recurse without special formatting (body, html, unknown tags)
        result += xhtml_to_weechat(child, in_blockquote);
    }
    return result;
}

// ---------------------------------------------------------------------------
// html_strip_to_plain
// ---------------------------------------------------------------------------
// Strip HTML tags from a raw HTML string and decode basic entities.
// This is a best-effort plain-text approximation for Atom <content type='html'>.

std::string html_strip_to_plain(const std::string &html)
{
    std::string out;
    out.reserve(html.size());

    bool in_tag    = false;
    bool in_script = false; // suppress <script>…</script> content entirely
    bool in_style  = false; // suppress <style>…</style> content entirely
    size_t i = 0;
    const size_t n = html.size();

    // Helper: case-insensitive prefix match at position pos inside html
    auto istartsw = [&](size_t pos, const char *prefix) {
        for (size_t k = 0; prefix[k]; ++k) {
            if (pos + k >= n) return false;
            if (tolower((unsigned char)html[pos+k]) != (unsigned char)tolower((unsigned char)prefix[k])) return false;
        }
        return true;
    };

    while (i < n)
    {
        char c = html[i];

        if (in_tag)
        {
            if (c == '>') in_tag = false;
            ++i;
            continue;
        }

        if (c == '<')
        {
            in_tag = true;
            // Check for block tags that should become newlines
            size_t tag_start = i + 1;
            bool is_close = (tag_start < n && html[tag_start] == '/');
            size_t name_start = is_close ? tag_start + 1 : tag_start;

            auto is_block_tag = [&](const char *t) {
                return istartsw(name_start, t);
            };

            // Suppress script/style content
            if (istartsw(name_start, "script"))  { in_script = true; }
            if (istartsw(name_start, "/script")) { in_script = false; ++i; in_tag = false; /* skip > */ while (i < n && html[i] != '>') ++i; if (i < n) ++i; continue; }
            if (istartsw(name_start, "style"))   { in_style = true; }
            if (istartsw(name_start, "/style"))  { in_style = false; ++i; in_tag = false; while (i < n && html[i] != '>') ++i; if (i < n) ++i; continue; }

            if (!in_script && !in_style)
            {
                // Insert newline before/after certain block tags
                if (is_block_tag("p")  || is_block_tag("div") || is_block_tag("br") ||
                    is_block_tag("li") || is_block_tag("h1")  || is_block_tag("h2") ||
                    is_block_tag("h3") || is_block_tag("h4")  || is_block_tag("h5") ||
                    is_block_tag("h6") || is_block_tag("tr")  || is_block_tag("blockquote") ||
                    is_block_tag("pre") || is_block_tag("/p") || is_block_tag("/div") ||
                    is_block_tag("/li") || is_block_tag("/h1") || is_block_tag("/h2") ||
                    is_block_tag("/h3") || is_block_tag("/h4") || is_block_tag("/h5") ||
                    is_block_tag("/h6") || is_block_tag("/tr"))
                {
                    if (!out.empty() && out.back() != '\n')
                        out += '\n';
                }
            }
            ++i;
            continue;
        }

        if (in_script || in_style) { ++i; continue; }

        // HTML entity decoding
        if (c == '&')
        {
            size_t semi = html.find(';', i + 1);
            if (semi != std::string::npos && semi - i <= 10)
            {
                std::string entity = html.substr(i + 1, semi - i - 1);
                // Lowercase for comparison
                std::string elc = entity;
                for (auto &ec : elc) ec = (char)tolower((unsigned char)ec);

                if      (elc == "amp")  { out += '&';  i = semi + 1; continue; }
                else if (elc == "lt")   { out += '<';  i = semi + 1; continue; }
                else if (elc == "gt")   { out += '>';  i = semi + 1; continue; }
                else if (elc == "quot") { out += '"';  i = semi + 1; continue; }
                else if (elc == "apos") { out += '\''; i = semi + 1; continue; }
                else if (elc == "nbsp") { out += ' ';  i = semi + 1; continue; }
                else if (elc == "mdash" || elc == "#8212") { out += "—"; i = semi + 1; continue; }
                else if (elc == "ndash" || elc == "#8211") { out += "–"; i = semi + 1; continue; }
                else if (elc == "hellip" || elc == "#8230") { out += "…"; i = semi + 1; continue; }
                else if (elc == "rsquo" || elc == "#8217") { out += "'"; i = semi + 1; continue; }
                else if (elc == "lsquo" || elc == "#8216") { out += "'"; i = semi + 1; continue; }
                else if (elc == "rdquo" || elc == "#8221") { out += "\""; i = semi + 1; continue; }
                else if (elc == "ldquo" || elc == "#8220") { out += "\""; i = semi + 1; continue; }
                else if (!entity.empty() && entity[0] == '#')
                {
                    // Numeric character reference: &#NNN; or &#xHH;
                    try {
                        unsigned long cp = 0;
                        if (entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X'))
                            cp = std::stoul(entity.substr(2), nullptr, 16);
                        else
                            cp = std::stoul(entity.substr(1));
                        // UTF-8 encode the code point
                        if (cp < 0x80) {
                            out += (char)cp;
                        } else if (cp < 0x800) {
                            out += (char)(0xC0 | (cp >> 6));
                            out += (char)(0x80 | (cp & 0x3F));
                        } else if (cp < 0x10000) {
                            out += (char)(0xE0 | (cp >> 12));
                            out += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out += (char)(0x80 | (cp & 0x3F));
                        } else if (cp < 0x110000) {
                            out += (char)(0xF0 | (cp >> 18));
                            out += (char)(0x80 | ((cp >> 12) & 0x3F));
                            out += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out += (char)(0x80 | (cp & 0x3F));
                        }
                        i = semi + 1;
                        continue;
                    } catch (...) { /* fall through to literal output */ }
                }
            }
        }

        out += c;
        ++i;
    }

    // Collapse runs of more than two consecutive newlines to two
    std::string result;
    result.reserve(out.size());
    int nl_count = 0;
    for (char c2 : out)
    {
        if (c2 == '\n') {
            ++nl_count;
            if (nl_count <= 2) result += c2;
        } else {
            nl_count = 0;
            result += c2;
        }
    }

    // Trim leading/trailing whitespace
    auto first = result.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) return {};
    auto last = result.find_last_not_of(" \t\n\r");
    return result.substr(first, last - first + 1);
}
