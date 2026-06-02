# C++23 Modernization Effort

> **Status**: Views modernization extended (as of 2026-06-02); prior phases complete
> This document captures the plan, progress, and remaining work for adopting modern C++23 features as recommended in the project's agent instructions.

## Background

This work originated from a full compliance audit against the rules in:

- [AGENTS.md](../AGENTS.md)

The audit identified that while the project claimed C++23 as its standard, adoption of the
specific "C++23 Memory Safety Features to Leverage" was only partial:

- Heavy use of `std::string_view`, `std::optional`, `std::make_unique`
- Almost no use of `std::span`
- Almost no use of `std::ranges::`
- Zero use of `std::expected`

As of 2026-06-01, the initial gaps (span, ranges algos, expected, views) were closed, with zero remaining
classical `std::algorithm` calls in `.cpp`/`.inl` files. Views adoption was further extended in subsequent
work (see below). All recommended C++23 features are in active use.

## Goals

1. **Systematically adopt** the features recommended in the agent instructions:
   - `std::string_view` for read-only string parameters (instead of `const std::string&`)
   - `std::span` for array/byte buffer views (instead of raw pointer + size)
   - `std::ranges` algorithms and views (instead of manual loops or classical `<algorithm>`)
   - `std::expected` for error handling (instead of exceptions or out-parameters)

2. Improve code safety, clarity, and modernity while following the project's "surgical/minimal changes" philosophy.

3. Leave the codebase in a state where future agents can easily continue the work.

## Completed Work (as of 2026-06-01)

### 1. `std::expected` (Phase 1 ✅)

- Converted two functions from losing error information to carrying
  diagnostic strings via `std::expected<T, std::string>`:
  - `esfs_b64_decode` (helpers.cpp) — now reports specific BIO failures
  - `pkcs7_unpad` (internal_crypto.inl) — now distinguishes empty input,
    invalid padding byte, and inconsistent padding bytes
- Pattern: error type is `std::string`, `std::unexpected(...)` for errors,
  `.error()` to read the message, `->` to access the value.
- Includes added to `helpers.cpp` and `api.cpp`.

### 2. `std::views` (Phase 2 ✅, extended 2026-06-02)

- **`std::views::split`**: replaced manual comma-split parsing loop in
  `caps_cache_load` (lmdb_cache.inl) — 9 lines → 2 lines
- **`std::views::split`**: modernized the central `split(std::string_view, char)` helper
  in `omemo/internal_prelude.inl` (used extensively for OMEMO devicelist ";"-separated
  strings and other parsing) from ~20-line manual char-by-char loop + current buffer
  to a 6-line views pipeline. All existing call sites (in session_flow, codec, commands,
  internal_crypto, etc.) benefit with no API change.
- **`std::views::split`**: replaced manual tab-split parsing loop (9 lines) in
  `og_cache_lookup` (lmdb_cache.inl) with views pipeline (matching caps style).
- **`std::views::filter | transform`**: converted (and added one more) device-list
  parsing in `omemo/commands.inl` (e.g. `show_devices`, `get_cached...`,
  `show_fingerprints`): split → transform(parse) → filter(valid) → transform →
  copy/to vector (pure collection cases).
- **`std::views::take` + `std::ranges::for_each`**: replaced manual prefix hash loop
  (first 64 bytes) in `avatar::render_unicode_blocks` (avatar.cpp).
- **`std::ranges::for_each`** (on range): updated groups comma-listing in
  `command/roster.inl`, meta list in iq_handler, and values list in message_handler
  (with color seps) to use ranges for_each over the container (avoiding index loops).
- Added one more `std::expected` conversion: `parse_uint32` / `parse_int64` now return
  `std::expected<T, std::string>` (with "invalid ..." on from_chars fail) instead of
  optional; call sites (including in views pipelines) required zero or minimal edits
  thanks to compatible API (bool ctx, * , .value_or(default) all work). Updated
  2 parse decls + impl; ~30 call sites unchanged in behavior.
- **`std::ranges::to`** (C++23): confirmed working with GCC 16
- Added `#include <ranges>` to `avatar.cpp` and `command/roster.cpp` (the .inl files
  for roster and the omemo/lmdb wrappers already pulled it via their .cpp entry points).

### 3. `std::string_view` Campaign ✅

- Converted read-only `const std::string&` parameters to `std::string_view`
  across utility functions, OMEMO layer, connection/command code, picker
  callbacks, and stanza-building helpers.

### 4. `std::span` Campaign ✅

- Adopted for owned byte buffers in OMEMO base64/crypto layers, PGP
  `gpgme_data_new_from_mem` paths, file path buffers, and OpenSSL BIO paths.
- Pattern: create local `std::span<T>` from owned `std::vector<T>` / `std::array<T>`.

### 5. `std::ranges` Algorithm Migration ✅

- All classical algorithm calls in `.cpp`/`.inl` files upgraded to
  `std::ranges::` equivalents (sort, stable_sort, transform, any_of,
  find_if, find, copy_if, unique, for_each, search, copy_n, contains).

### 6. Final C++23 API Sweep ✅

- **`.find(X) != npos` → `.contains(X)`**: 7 locations (helpers.cpp,
  message_handler.inl, codec.inl, channel.cpp)
- **`.count(K) > 0` → `.contains(K)`**: 4 locations (codec.inl,
  session_flow.inl, channel.cpp — map/set/.unordered_map)
- **`std::copy_n` → `std::ranges::copy`/`copy_n`**: 4 locations in
  internal_crypto.inl
- **Structured bindings**: modernized several map iterations and find results
  (e.g. `for (auto& [_, acc] : accounts)`, `auto& [_, acc] = *it`, similar in
  channel::members, disconnect loops, timer callbacks, save_pgp etc.).
- **Zero remaining classical `std::algorithm` calls** in `.cpp`/`.inl`
  files (lone exception: a commented-out `std::find` in plugin.cpp).

## Current Status (2026-06-01)

- **Builds cleanly** with `make`
- **All tests pass** (27/27 cases, 286/286 assertions)
- Initial phases from the original plan are complete; `std::views` adoption and other
  C++23 (structured bindings, more expected) being incrementally extended as surgical
  opportunities arise in list/string processing and error paths (e.g. more maps in buffer/config/completion, og cache).
- Zero remaining classical `std::algorithm` calls in `.cpp`/`.inl` files.
- `std::expected`, `std::views`, and `std::ranges::to` patterns are established and
  ready for wider adoption.
- `std::span` is used wherever owned buffers pass through C API boundaries.

**Adoption counts** (approximate):
| Feature | Before | After |
|---------|--------|-------|
| `std::expected` | 0 | 6 (b64, pkcs7, 2x parse_*, load_tofu_trust, og_cache_lookup) |
| `std::views::` | 0 | 8+ (split x3, join_with attempt, take, filter/transform pipelines x3+, etc.) |
| `std::ranges::to` | 0 | 1+ |
| `std::span` | 0 | 29 |
| `std::ranges::` algorithms | ~40 (classical) | 37 (all modern) |
| Structured bindings in for/find | few | more (accounts, channels, members, buffer lookups, config, completion, commands, lambdas in channel etc. across 15+ sites) |
| `.find(X) != npos` | 15+ | 0 |
| `.count(K) > 0` | 5+ | 0 |
| `std::copy_n` | 4 | 0 |

## Notes for Future Work

- **LMDB cursors, libstrophe XML iteration, and WeeChat C hooks** are constrained
  by C ABI boundaries — not candidates for `std::ranges::views` or `std::span`.
- **Manual index loops** over non-standard data (C arrays, opaque handles) remain
  where necessary; converting them would add indirection without benefit.
- **`std::expected`** can be adopted further in error-returning functions — the
  pattern is established, adoption should be incremental (parse_* , load_tofu_trust, og_cache_lookup done; more in
  decode/load paths possible without rippling too far, e.g. avatar cache, other lmdb lookups).
- **Further `std::views`** opportunities exist for other list comprehensions, string
  tokenization, or filtering side-effect loops (e.g. device lists, MUC members, etc.).
  Continue the surgical pattern: prefer views pipelines over manual loops or eager
  intermediate vectors where the result is consumed once. `join_with` + `to<string>`
  is powerful for joins but may need fallback to for_each on complex seps or older
  pipe compatibility.
- **Structured bindings** (`auto [k, v]`) and `if` init + structured can be applied
  more to map/set iterations and find() results for clarity.

## Related Documents

- [AGENTS.md](../AGENTS.md)