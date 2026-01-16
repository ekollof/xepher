// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <regex>
#include <string>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strophe.h>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "util.hh"

int char_cmp(const void *p1, const void *p2)
{
    return *(const char *)p1 == *(const char *)p2;
}

char *exec(const char *command)
{
    // use hook_process instead!
    char buffer[128];
    char **result = weechat_string_dyn_alloc(256);

    // Open pipe to file
    FILE* pipe = popen(command, "r");
    if (!pipe) {
        return (char*)strdup("popen failed!");
    }

    // read till end of process:
    while (!feof(pipe)) {

        // use buffer to read and add to result
        if (fgets(buffer, 128, pipe) != NULL)
            weechat_string_dyn_concat(result, buffer, -1);
    }

    pclose(pipe);
    weechat_string_dyn_free(result, 0);
    return *result;
}

char *stanza_xml(xmpp_stanza_t *stanza)
{
    char *result;
    size_t len;
    xmpp_stanza_to_text(stanza, &result, &len);
    return result;
}

std::string unescape(const std::string& str)
{
    std::regex regex("\\&\\#(\\d+);");
    std::sregex_iterator begin(str.begin(), str.end(), regex), end;
    if (begin != end)
    {
        std::ostringstream output;
        do {
            std::smatch const& m = *begin;
            if (m[1].matched)
            {
                auto ch = static_cast<char>(std::stoul(m.str(1)));
                output << m.prefix() << ch;
            }
            else output << m.prefix() << m.str(0);
        } while (++begin != end);
        output << str.substr(str.size() - begin->position());
        return output.str();
    }
    return str;
}

// XEP-0393: Message Styling
// Converts XEP-0393 markup to WeeChat color codes
std::string apply_xep393_styling(const std::string& text)
{
    if (text.empty()) return text;
    
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
            result += weechat_color("gray");
            i += 3;
            continue;
        }
        else if (in_preblock && i + 2 < text.length() && 
                 text[i] == '`' && text[i+1] == '`' && text[i+2] == '`')
        {
            in_preblock = false;
            result += weechat_color("resetcolor");
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
            result += weechat_color("green");
            result += '>';
            i++;
            // Continue until end of line
            while (i < text.length() && text[i] != '\n')
            {
                result += text[i++];
            }
            result += weechat_color("resetcolor");
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
                while (end < text.length() && text[end] != '*')
                    end++;
                
                if (end < text.length() && !isspace(text[end-1]) &&
                    (end + 1 >= text.length() || isspace(text[end+1]) || ispunct(text[end+1])))
                {
                    result += weechat_color("bold");
                    i++;
                    while (i < end)
                        result += text[i++];
                    result += weechat_color("-bold");
                    i++; // Skip closing *
                    continue;
                }
            }
            
            // _emphasis_ (italic/underline)
            else if (ch == '_' && !isspace(text[i+1]))
            {
                size_t end = i + 1;
                while (end < text.length() && text[end] != '_')
                    end++;
                
                if (end < text.length() && !isspace(text[end-1]) &&
                    (end + 1 >= text.length() || isspace(text[end+1]) || ispunct(text[end+1])))
                {
                    result += weechat_color("underline");
                    i++;
                    while (i < end)
                        result += text[i++];
                    result += weechat_color("-underline");
                    i++; // Skip closing _
                    continue;
                }
            }
            
            // `monospace` (inline code)
            else if (ch == '`' && !isspace(text[i+1]))
            {
                size_t end = i + 1;
                while (end < text.length() && text[end] != '`')
                    end++;
                
                if (end < text.length() && !isspace(text[end-1]) &&
                    (end + 1 >= text.length() || isspace(text[end+1]) || ispunct(text[end+1])))
                {
                    result += weechat_color("gray");
                    i++;
                    while (i < end)
                        result += text[i++];
                    result += weechat_color("resetcolor");
                    i++; // Skip closing `
                    continue;
                }
            }
            
            // ~strikethrough~
            else if (ch == '~' && !isspace(text[i+1]))
            {
                size_t end = i + 1;
                while (end < text.length() && text[end] != '~')
                    end++;
                
                if (end < text.length() && !isspace(text[end-1]) &&
                    (end + 1 >= text.length() || isspace(text[end+1]) || ispunct(text[end+1])))
                {
                    result += weechat_color("red");
                    i++;
                    while (i < end)
                        result += text[i++];
                    result += weechat_color("resetcolor");
                    i++; // Skip closing ~
                    continue;
                }
            }
        }
        
        // No styling matched, just copy character
        result += text[i++];
    }
    
    return result;
}
