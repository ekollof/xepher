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

## Completed Work (as of latest session)

### 1. `std::string_view` Campaign (Largely Complete)

- Converted the vast majority of read-only `const std::string&` parameters to `std::string_view` across:
  - Utility functions (`util.cpp/hh`)
  - OMEMO layer (many internal helpers and lambdas)
  - Connection and command code
  - Picker callbacks and local spec structs
  - Stanza-building helpers

- Updated all call sites and necessary internal logic (with `std::string(...)` conversions only where storage or C APIs required it).

**Result**: Dramatic reduction in unnecessary string copies for read-only data.

### 2. `std::span` Campaign (Very Advanced)

- Introduced `std::span` for owned byte buffers in:
  - OMEMO base64 layer (`base64_encode` / `base64_encode_raw`)
  - Main GCM crypto functions (`crypto_encrypt`, `crypto_decrypt`, `axolotl_omemo_encrypt/decrypt`)
  - Key/IV/authtag arrays
  - Transport key encryption (`encrypt_axolotl_transport_key`)
  - PGP `gpgme_data_new_from_mem` paths
  - File path buffers (`rooms.inl`)
  - Various internal working buffers

- Pattern established: When we own a `std::vector` / `std::array`, create a local `std::span` and pass `.data()/.size()` through it.

**Result**: Much better compliance with the explicit recommendation to use `std::span` instead of pointer+size pairs.

### 3. `std::ranges` Adoption (Strong Start)

- Upgraded dozens of classical algorithms:
  - `std::sort` / `std::stable_sort` → `std::ranges::sort` / `stable_sort`
  - `std::transform` → `std::ranges::transform`
  - `std::any_of` / `std::find_if` / `std::find` → `std::ranges::` equivalents
  - `std::copy_if` → `std::ranges::copy_if`
  - `std::unique` + erase → `std::ranges::unique` + erase
  - Manual join / comma-list loops → `std::ranges::for_each`
  - `std::search` → `std::ranges::search`
  - `std::copy_n` → `std::ranges::copy_n`

- Added `#include <ranges>` in key translation units (`api.cpp`, etc.).

**Result**: Clear, consistent use of the `std::ranges` namespace in many hot paths.

### 4. Modern Container & String APIs

- Upgraded many `.count(key) != 0` → `.contains(key)` (C++20)
- Upgraded several `str.find(...) != npos` → `str.contains(...)` (C++23)
- These are small but very visible modernizations.

## Current Status (as of this document)

- **Builds cleanly** with `make`
- **All tests pass** (27/27 cases, 286/286 assertions)
- A large body of modernization work is complete but **not yet committed** (as of the session when this doc was written).
- The easy-to-medium wins in the three main recommended areas (`string_view`, `span`, `ranges` algorithms) are substantially done.

**Files touched**: 20+ across `src/` (utility, connection, command, OMEMO, PGP, XMPP helpers, etc.).

## Remaining Opportunities

### High Value / Relatively Easy
- **More `std::span`** on remaining owned buffers passed to C APIs (gcrypt, libsignal, OpenSSL EVP/BIO, etc.). Many direct `.data(), .size()` calls remain.
- **Deeper `std::ranges` views** (`std::views::filter`, `transform`, `take`, `enumerate`, `reverse`, `join`, etc.).
- **More `std::ranges::for_each`** on manual index-based or complex loops.
- **First real `std::expected` usage** (the biggest remaining zero-adoption item from the original audit).

### Medium / More Invasive
- Full `std::ranges` algorithms on more complex manual loops (some loops have early `continue` / state that makes pure algorithms harder).
- Using `std::ranges::to` for container conversions (C++23).
- `std::expected` propagation through larger call chains.

### Lower Priority / Riskier
- Changing C callback signatures (libsignal, gcrypt provider functions) to take spans — would require significant wrapper work.
- Large-scale loop refactors just for aesthetics.

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