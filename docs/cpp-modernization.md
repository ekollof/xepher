# C++23 Modernization Effort

> **Status**: Active (as of 2026-06)
> This document captures the plan, progress, and remaining work for adopting modern C++23 features as recommended in the project's agent instructions.

## Background

This work originated from a full compliance audit against the rules in:

- [AGENTS.md](../AGENTS.md)
- [.github/copilot-instructions.md](../.github/copilot-instructions.md)

The audit (documented in the private session plan) identified that while the project claims C++23 as its standard, adoption of the specific "C++23 Memory Safety Features to Leverage" was only partial:

- Heavy use of `std::string_view`, `std::optional`, `std::make_unique`
- Almost no use of `std::span`
- Almost no use of `std::ranges::`
- Zero use of `std::expected`

The instructions explicitly call out these features as things agents should leverage.

## Goals

1. **Systematically adopt** the features recommended in the copilot instructions:
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

### 6. Modern Container & String APIs ✅

- `.count(key) != 0` → `.contains(key)` (C++20)
- `str.find(...) != npos` → `str.contains(...)` (C++23)

## Current Status (2026-06-01)

- **Builds cleanly** with `make`
- **All tests pass** (27/27 cases, 286/286 assertions)
- All phases from the original plan are now complete.
- The codebase has zero remaining classical `std::algorithm` calls in `.cpp`/`.inl` files.
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

## Remaining Opportunities

### Lower Priority
- **Deeper `std::views` usage** on remaining manual loops where value is clear
  (many loops involve C APIs that don't benefit from views: LMDB cursors,
  libstrophe XML iteration, WeeChat hook callbacks)
- **More `std::expected`** in error-returning functions — the pattern is
  established, can be adopted incrementally
- **Large-scale loop refactors** — not worth the churn for marginal value

## How to Pick This Up Later

1. Read this document + the original `.github/copilot-instructions.md` section on C++23 features.
2. Build the project (`make`) to get a clean baseline.
3. Look at recent changes (or the uncommitted diff if available) for the established patterns:
   - `std::string_view` for read-only strings
   - Local `std::span<T>` variables for owned buffers
   - `std::ranges::X` instead of `std::X`
4. Good places to continue:
   - `src/omemo/internal_crypto.inl` and friends (more spans)
   - Any file with manual `for (size_t i = 0; ...)` loops over containers
   - Error handling paths (for `std::expected`)
5. Always run `make` after changes. Prefer small, focused commits.

## Related Documents

- [AGENTS.md](../AGENTS.md)
- [.github/copilot-instructions.md](../.github/copilot-instructions.md) (especially the "C++23 Memory Safety Features to Leverage" section)
- The original private session plan (contains the full audit findings and detailed step-by-step history)

---

*This document was generated from an interactive modernization session in June 2026. It is intended to make the work resumable by future agents or contributors.*