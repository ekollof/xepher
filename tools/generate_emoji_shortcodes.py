#!/usr/bin/env python3
"""Regenerate src/message_emoji_shortcodes.inl from vendored github/gemoji data."""

from __future__ import annotations

import argparse
import json
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GEMoji_URL = "https://raw.githubusercontent.com/github/gemoji/master/db/emoji.json"
JSON_PATH = ROOT / "tools" / "gemoji" / "emoji.json"
OUT_PATH = ROOT / "src" / "message_emoji_shortcodes.inl"


def load_emoji_data(path: Path) -> list[dict]:
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def fetch_upstream_json(path: Path) -> None:
    with urllib.request.urlopen(GEMoji_URL) as response:
        data = json.load(response)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"fetched {path}")


def generate_inl(data: list[dict]) -> str:
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

    return f"""// Generated from tools/gemoji/emoji.json — do not edit by hand.
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


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--fetch",
        action="store_true",
        help="download tools/gemoji/emoji.json from upstream before generating",
    )
    args = parser.parse_args()

    if args.fetch:
        fetch_upstream_json(JSON_PATH)

    if not JSON_PATH.is_file():
        raise SystemExit(f"missing {JSON_PATH} (run with --fetch or add the file)")

    content = generate_inl(load_emoji_data(JSON_PATH))
    OUT_PATH.write_text(content, encoding="utf-8")
    alias_count = content.count('    {"')
    print(f"wrote {OUT_PATH} ({alias_count} aliases)")


if __name__ == "__main__":
    main()