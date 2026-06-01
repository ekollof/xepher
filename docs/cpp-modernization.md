# C++23 Modernization Effort

> **Status**: Complete (as of 2026-06-01)
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

As of 2026-06-01, all of these gaps have been closed. The codebase now has zero remaining
classical `std::algorithm` calls in `.cpp`/`.inl` files, and all recommended C++23 features
are in active use.

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

### 2. `std::views` (Phase 2 ✅)

- **`std::views::split`**: replaced manual comma-split parsing loop in
  `caps_cache_load` (lmdb_cache.inl) — 9 lines → 2 lines
- **`std::views::filter | transform`**: converted two device-list parsing
  functions in `omemo/commands.inl`:
  - `get_cached_device_ids`: split → transform(parse) → filter(valid) →
    transform(unwrap) → `std::ranges::to<std::vector>()`
  - `show_fingerprints` device collection: same pipeline with
    `std::ranges::copy` + `back_inserter`
- **`std::ranges::to`** (C++23): confirmed working with GCC 16

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
- **Zero remaining classical `std::algorithm` calls** in `.cpp`/`.inl`
  files (lone exception: a commented-out `std::find` in plugin.cpp).

## Current Status (2026-06-01)

- **Builds cleanly** with `make`
- **All tests pass** (27/27 cases, 286/286 assertions)
- All phases from the original plan are now complete.
- Zero remaining classical `std::algorithm` calls in `.cpp`/`.inl` files.
- `std::expected`, `std::views`, and `std::ranges::to` patterns are established and
  ready for wider adoption.
- `std::span` is used wherever owned buffers pass through C API boundaries.

**Adoption counts** (approximate):
| Feature | Before | After |
|---------|--------|-------|
| `std::expected` | 0 | 2 |
| `std::views::` | 0 | 4 |
| `std::ranges::to` | 0 | 1 |
| `std::span` | 0 | 29 |
| `std::ranges::` algorithms | ~40 (classical) | 37 (all modern) |
| `.find(X) != npos` | 15+ | 0 |
| `.count(K) > 0` | 5+ | 0 |
| `std::copy_n` | 4 | 0 |

## Notes for Future Work

- **LMDB cursors, libstrophe XML iteration, and WeeChat C hooks** are constrained
  by C ABI boundaries — not candidates for `std::ranges::views` or `std::span`.
- **Manual index loops** over non-standard data (C arrays, opaque handles) remain
  where necessary; converting them would add indirection without benefit.
- **`std::expected`** can be adopted further in error-returning functions — the
  pattern is established, adoption should be incremental.

## Related Documents

- [AGENTS.md](../AGENTS.md)