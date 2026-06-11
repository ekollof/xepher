#!/usr/bin/env python3
"""Regenerate src/message_emoji_shortcodes.inl from github/gemoji."""

from __future__ import annotations

import json
import urllib.request
from pathlib import Path

GEMoji_URL = "https://raw.githubusercontent.com/github/gemoji/master/db/emoji.json"
OUT_PATH = Path(__file__).resolve().parents[1] / "src" / "message_emoji_shortcodes.inl"


def main() -> None:
    with urllib.request.urlopen(GEMoji_URL) as response:
        data = json.load(response)

    entries: list[tuple[str, str]] = []
    seen: set[str] = set()
    for item in data:
        emoji = item["emoji"]
        for alias in item.get("aliases", []):
            if alias in seen:
                continue
            seen.add(alias)
            entries.append((alias, emoji))

    entries.sort(key=lambda pair: pair[0])

    rows = []
    for alias, emoji in entries:
        esc = emoji.replace("\\", "\\\\").replace('"', '\\"')
        rows.append(f'    {{"{alias}", "{esc}"}},')

    content = f"""// Generated from github/gemoji db/emoji.json — do not edit by hand.
// Regenerate: python3 tools/generate_emoji_shortcodes.py

#pragma once

#include <string_view>
#include <unordered_map>

#include "util.hh"

namespace {{

struct emoji_shortcode_entry {{
    std::string_view code;
    std::string_view emoji;
}};

static constexpr emoji_shortcode_entry k_emoji_shortcodes[] = {{
{chr(10).join(rows)}
}};

[[nodiscard]] inline const auto &emoji_shortcode_map()
{{
    static const std::unordered_map<std::string_view, std::string_view,
                                     transparent_string_hash, std::equal_to<>> map = [] {{
        std::unordered_map<std::string_view, std::string_view,
                            transparent_string_hash, std::equal_to<>> out;
        out.reserve({len(entries)});
        for (const auto &[code, emoji] : k_emoji_shortcodes)
            out.emplace(code, emoji);
        return out;
    }}();
    return map;
}}

}} // namespace
"""

    OUT_PATH.write_text(content, encoding="utf-8")
    print(f"wrote {OUT_PATH} ({len(entries)} aliases)")


if __name__ == "__main__":
    main()