# Agent Instructions

All coding agents working in this repository **must** read and follow this entire file before making any changes.

**After any context compaction** (conversation summary, truncated history, or handoff to a new agent turn with only a recap): **re-read this entire `AGENTS.md` file before writing code or continuing the task.** Compaction drops detail from the thread — the repo rules here are authoritative, not the summary.

For OMEMO troubleshooting tasks, agents may use `tools/correlate_omemo_xml.sh --account <account>` as a convenience helper to correlate WeeChat log events with raw XML.

---

## Project Overview

This is a WeeChat plugin for XMPP/Jabber written in C++. It uses libstrophe for XMPP protocol handling and LMDB for local persistence (MAM cache, capabilities cache). This is a fork with bug fixes and feature enhancements.

## XEP Specifications

Canonical XEP specs for all implemented XEPs are stored in `docs/specs/xep-NNNN.txt` (fetched directly from `https://xmpp.org/extensions/xep-NNNN.html`).

**The XEP specifications must be *religiously* followed at all times to ensure compatibility.** This is non-negotiable. It guarantees correct interoperability with other clients (e.g. rich file previews via SFS/ESFS metadata in Conversations and similar, proper element nesting, size/hash reporting for plain vs. encrypted uploads, namespaces, and full protocol flows). Even small deviations "for convenience", legacy fallbacks, or perceived simplicity have caused real-world failures such as missing previews, corrupt metadata, service-unavailable errors in MUC, or incorrect links.

**When implementing or modifying support for any XEP:**
1. Fetch the canonical spec: `curl -s https://xmpp.org/extensions/xep-NNNN.html -o docs/specs/xep-NNNN.txt`
2. Verify stanza structure, namespaces, and protocol behavior against that spec before finalizing code. Cross-check all examples, MUST/RECOMMENDED/SHOULD requirements, and related XEPs (e.g. XEP-0446 file metadata, XEP-0300 hashes).
3. Commit the spec file alongside the implementation.

**Do not** modify the canonical spec files to match the code — the spec is authoritative.

## OMEMO Protocol Reference

- Primary OMEMO specification: `docs/specs/xep-0384.txt` (canonical, fetched from https://xmpp.org/extensions/xep-0384.html)
- For OMEMO changes, always verify stanza structure, namespaces, and pubsub node behavior against XEP-0384 before finalizing code.
- **Protocol**: `eu.siacs.conversations.axolotl` (legacy/Gajim-compatible) is the **sole** supported namespace.
  OMEMO:2 (`urn:xmpp:omemo:2`) must **not** be published, encoded, or treated as a primary path.
  Any incoming OMEMO:2 stanza may be silently ignored or answered with a key-not-found error.
- **Trust model**: Blind Trust Before Verification (BTBV) TOFU — mirrors Gajim's `get_default_trust()`.
  Trust levels: `UNTRUSTED=0`, `VERIFIED=1`, `UNDECIDED=2`, `BLIND=3` (stored in LMDB as `trust:{jid}:{device_id}`).
  Only `VERIFIED` and `BLIND` devices receive encrypted key material at encode time.
- **No ATM**: XEP-0450 Automatic Trust Management is removed. Do not add it back.
- **Reference implementation**: Gajim + python-omemo-dr at
  `/usr/lib/python3/dist-packages/gajim/common/modules/omemo.py`,
  `/usr/lib/python3/dist-packages/gajim/common/storage/omemo.py`,
  `/usr/lib/python3/dist-packages/omemo_dr/`
- **Current refactor plan**: See `TODO.md` in the repository root for the full phase-by-phase
  breakdown of the ongoing axolotl-only + BTBV refactor (branch `omemo-axolotl-btbv`).

## Build System

### Local development

- **Build command**: `make` — **parallel by default** (`-j$(nproc)`); use `make -j1` only when debugging ordering issues. Combine with ccache: `CXX="ccache c++" make`
- **Clean command**: `make clean` (avoid unless necessary; ccache makes rebuilds quick)
- **Output**: `xmpp.so` (WeeChat plugin)
- **Dependencies**: Managed via git submodules in `deps/`
- Always run `make` after code changes (109 doctests run automatically at the end)
- **Includes**: use `-Isrc` paths (`plugin.hh`, `xmpp/stanza_view.hh`) — never `../` relative includes in `src/`

### Distribution / CI builds

- **`PACKAGE_BUILD=1`**: all packaging scripts and specs pass this to `make`. It skips the dev-only `.source` ELF section (`objcopy --add-section`). Without it, tarball builds lacking `.git` can archive the entire build tree and produce gigabyte RPM/APK packages.
- **`.source` embed** (Linux dev builds only): after link, tracked sources are tarred into `xmpp.so` for the `make release` workflow. Runs only when `PACKAGE_BUILD` is unset, `git ls-files` succeeds, and the file list is non-empty.
- **`make clean`** removes `xmpp.so` and `obj/` (`clean.mk`) so container builds never reuse a host-built plugin.

### Packaging infrastructure

**Primary path — GitHub Actions** (`.github/workflows/packages.yml`):
- On `v*` tag push: builds all five distros **sequentially** in fresh Docker containers, uploads artifacts, and attaches them to the GitHub Release.
- `workflow_dispatch` with a version builds packages without creating a release.
- CI entry point: `packaging/github-build.sh <version>` (same script for local Docker verification).

**Local package verification** (requires Docker):
```sh
bash packaging/github-build.sh X.Y.Z              # all distros
bash packaging/github-build.sh X.Y.Z --fedora   # single distro
```

**Optional — persistent distrobox containers** (`packaging/distrobox-build.sh`):
- Reuses containers; stamps installed deps under `/opt/xepher-build/`. Uses the same `build-*-inside.sh` scripts as CI.
- Alpine and Void invoke `docker run` directly inside the script.

**Shared helpers** (`packaging/scripts/`):
- `prepare-source-tree.sh` — copy `/project` to a writable dir, `make clean`, seed `deps/diff/libdiff.a` while `.git` exists, set `safe.directory` for submodule steps.
- `build-{deb,rpm,arch,alpine,void}-inside.sh` — per-distro logic; all pass `PACKAGE_BUILD=1`.
- `docker-arch-wrapper.sh` — Arch `makepkg` runs as non-root `builder`; chowns `/output` for artifact copy.

Output lands in `packaging/build/` (`.deb`, `.rpm`, `.pkg.tar.zst`, `.apk`, `.xbps`).

## Coding Style & Practices

### General C++ Guidelines

- Minimize changes - make surgical, targeted fixes
- Use existing code style and patterns consistently
- **Write modern C++23 by default** (project standard via `-std=c++23`) in every new or touched `.cpp`/`.hh`/`.inl` — apply the Modernization Patterns below *as you implement*, not in a later sweep. Do not land raw loops, `const std::string&` read-only params, manual `find != npos`, or string `+` concatenation in new code when `ranges`/`string_view`/`fmt`/`expected`/`span` fit.
- Use `nullptr` not `NULL`
- RAII for resource management
- Keep functions focused and concise

### Naming Conventions

- Functions, variables, and most types: `snake_case` (e.g. `mam_cache_put_message`, `send_bookmarks`, `chat_type`).
- Private / implementation member variables: trailing underscore (e.g. `filter_`, `buf_`, `on_select_`).
- Namespaces: `weechat::`, `weechat::xmpp::`, `weechat::ui::`, `stanza::xep0384::`, etc.
- **Primary rule**: When adding or editing code, open a nearby file in the same directory/subsystem (e.g. `src/connection/*.cpp` or `src/command/*.inl`) and match the dominant local style and indentation exactly. Avoid global style changes or renames.

### C++23 Memory Safety Features to Leverage

- **`std::unique_ptr` / `std::shared_ptr`**: Already used extensively - prefer over raw pointers
- **`std::optional`**: Already used - better than null pointers for optional values
- **`std::string_view`**: Already used - safer than `const char*` for read-only strings
- **`std::span`**: Consider using for array views instead of pointer+size pairs
- **`std::expected`**: Use for error handling instead of exceptions (C++23)
- **Range-based algorithms**: `std::ranges::` for safer iteration
- **`std::make_unique` / `std::make_shared`**: Always prefer over `new`
- **Move semantics**: Use `std::move` to avoid unnecessary copies
- **Structured bindings**: `auto [key, value] = map.find(...)` for cleaner code

**Modernization Patterns** (non-negotiable for all new and modified code — not optional follow-up work. Established via surgical updates; agents **must** use these in new code, refactors, and list/string/error handling in `.cpp`/`.inl` files. Follow "surgical/minimal" rule — only apply where it simplifies without touching C ABI boundaries like LMDB cursors or WeeChat hook signatures. Use `StanzaView` for inbound stanza traversal (not raw libstrophe child iteration). Match local style exactly by opening a nearby file first. Always `#include <ranges>` / `<expected>` / `<span>` / `<algorithm>` (for `std::ranges::`) in the thin `.cpp` wrapper before `#include`ing the `.inl`. Use `fmt::format` for string assembly; `-Isrc` includes (`plugin.hh`, `xmpp/foo.hh`) — never `../` relative paths.)

- **std::string_view for read-only params**: Replace `const std::string& s` (and `const char*`) with `std::string_view s`. Only `std::string(...)` cast when storing or returning ownership.
- **std::span for byte buffers**: Use for owned data passed to C APIs, e.g.:
  ```cpp
  [[nodiscard]] auto base64_encode_raw(std::span<const std::uint8_t> data) -> std::string;
  // ...
  std::span<unsigned char> out_view{out};
  int n = BIO_read(..., out_view.data(), ...);
  ```
  Create locals from `std::vector`/`std::array`: `std::span<T> view{vec};`.
- **std::ranges + views pipelines** (replace manual loops / classical algos / eager temporaries):
  - Tokenization: `for (auto r : input | std::views::split(separator)) { std::string s(r.begin(), r.end()); ... }` (see `split()` helper).
  - Collect/filter/transform: 
    ```cpp
    std::ranges::copy(
        split(*devlist, ';')
        | std::views::transform(parse_uint32)
        | std::views::filter([](auto p){ return p && is_valid(*p); })
        | std::views::transform([](auto p){ return *p; }),
        std::back_inserter(devices));
    // or ` | std::ranges::to<std::vector<uint32_t>>() `
    ```
  - Side-effect iteration: `std::ranges::for_each(range, lambda)` (avoids index `for (size_t i=0; ...)` for joins).
  - Joins: simple `vec | std::views::join_with(", ") | std::ranges::to<std::string>()`; complex (e.g. colored separators) fall back to `for_each` + flag.
  - Other: `views::take(N)`, `views::filter` + `transform`, `enumerate` where index+value needed.
  - Upgrade any remaining `std::transform` / `std::for_each` / manual `for` + `push_back` + `if` to the above.
- **std::expected<T, std::string> for errors** (carry diagnostics instead of losing info in `optional`/`bool`/exceptions):
  ```cpp
  [[nodiscard]] auto parse_uint32(std::string_view value) -> std::expected<std::uint32_t, std::string>;
  // ...
  if (error != std::errc{} || ptr != end) return std::unexpected("invalid uint32");
  return parsed;
  ```
  Usage: `if (auto e = foo(); e) { use(*e); } else { log(e.error()); }`, `.value_or(default)`, `e ? *e : fallback`. Call sites often need zero changes due to bool-conversion + `value_or`. Established in `esfs_b64_decode`, `pkcs7_unpad`, `parse_uint32`/`parse_int64`, `load_tofu_trust`.
- **Modern container/string APIs**: `m.contains(k)` (not `m.find(k) != end()` or `m.count(k)>0`), `s.contains(substr)` (not `find != npos`).
- **Structured bindings** (everywhere for maps, pairs, from_chars, if-init):
  ```cpp
  for (auto& [_, acc] : accounts) { use(acc); }
  else if (auto it = m.find(k); it != m.end()) { auto& [key, val] = *it; ... }
  const auto [ptr, ec] = std::from_chars(...);
  ```
- **General**: Prefer `std::ranges::sort` / `unique` / `for_each` / `copy_if` etc. over raw loops or `<algorithm>`. Zero classical `<algorithm>` calls remain in src (except commented). Update this section when adding new patterns (e.g. more `views`).

**Build note**: Use parallel + ccache during iterative work: `CXX="ccache c++" make` (parallel `-j` is on by default; see Build System). Run `make` (not clean) after every logical group of changes; verify doctests pass.

(Concrete examples of these patterns are visible throughout `src/` — e.g. OMEMO helpers, account/channel map handling, connection data-form/OG parsing, and avatar/ base64 paths. Extend them surgically.)

### WeeChat Plugin Conventions

- **Output and buffer operations**: use `UiPort`, `BufferPort`, `LineStorePort`, and `RenderEvent` in handler/command logic — not raw `weechat_printf` / `weechat_buffer_*` (see Port abstraction). Direct `weechat_*` calls belong only in port adapters and hook registration glue.
- Hook/callback **function signatures** must match WeeChat exactly (`weechat_*` types at the C boundary).
- Use `weechat_prefix()` (or `RuntimePort::color()`) when building formatted strings passed to `UiPort`.
- Buffer display values: `"1"` (don't auto-switch), `"auto"` (auto-switch)
- Never reload plugin - always restart WeeChat (race condition with timer hooks)

### Stanza Construction & Inbound Parsing

**Raw `xmpp_stanza_new()` calls are forbidden in all new code and all `.inl` / `.cpp` files.**
Use the fluent stanza builder system for **outbound** stanzas.

**Raw `xmpp_stanza_get_*` / manual child-pointer walks are forbidden in handler and domain logic.**
Use `xmpp::StanzaView` for **inbound** reads — wrap at the handler edge (`StanzaView view{stanza};`), then use `attr_string()`, `child()`, `text()`, and range-for over `children()`.

#### The stanza builder (`src/xmpp/node.hh` + XEP `.inl` files)

- Base class: `stanza::spec` — subclassed in `src/xmpp/xep-NNNN.inl` files.
- Attributes: `attr("name", value)`, namespace: `xmlns<NsType>()`, children: `child(spec&)`, text: `text(sv)`.
- Build to `shared_ptr<xmpp_stanza_t>`: `auto sp = my_spec.build(ctx);`
- All namespace types live in `src/xmpp/ns.hh` (e.g. `urn::xmpp::omemo::_2`, `urn::xmpp::hints`, `eu::siacs::conversations::axolotl`).
- OMEMO:2 and legacy axolotl types are in `src/xmpp/xep-0384.inl` (`stanza::xep0384::encrypted`, `stanza::xep0384::header`, `stanza::xep0384::keys`, `stanza::xep0384::key`, `stanza::xep0384::payload`, `stanza::xep0384::store_hint`, and axolotl_ variants).

#### Returning ownership from builder

Functions that return a raw `xmpp_stanza_t*` (transferring ownership to the caller) must ref-bump before the `shared_ptr` destructs. Use `xmpp_stanza_clone` (libstrophe 0.14) which increments the refcount and returns the same pointer:

```cpp
auto sp = my_spec.build(ctx);
xmpp_stanza_clone(sp.get());  // bump refcount; shared_ptr dtor will release its ref
return sp.get();               // caller owns one reference; must call xmpp_stanza_release()
```

#### Sending without ownership transfer

`connection.send()` does not take ownership. Pass `.get()` from the `shared_ptr` directly:

```cpp
auto msg_sp = stanza::message().type("chat").to(jid).id(uuid).build(ctx);
account.connection.send(msg_sp.get());   // shared_ptr releases on scope exit
```

#### Incremental child construction in loops

`spec::child()` copies the spec by value, so specs can be built up in loops:

```cpp
stanza::xep0384::keys keys_spec(jid);
for (auto &dev : device_list) {
    keys_spec.add_key(stanza::xep0384::key(rid, b64, is_kex));
}
header_spec.add_keys(keys_spec);
```

### Memory Management

**Raw `malloc`/`free`/`new`/`delete` are forbidden.** Use RAII exclusively.

#### Byte buffers from `malloc`/`calloc` (e.g. `base64_decode` output)

Use `heap_buf` / `make_heap_buf` from `src/omemo.hh`:

```cpp
// heap_buf = std::unique_ptr<uint8_t[], decltype(&free)>
uint8_t *raw = nullptr;
size_t len = base64_decode(text, strlen(text), &raw);
heap_buf buf = make_heap_buf(raw);   // auto-freed on scope exit
```

#### LMDB key buffers

Use `std::string` + `fmt::format` — never `malloc`/`snprintf`:

```cpp
std::string k_foo = fmt::format("prefix_{}_{}",  name, id);
MDB_val mdb_key = { .mv_size = k_foo.size(), .mv_data = k_foo.data() };
```

#### gcrypt allocations

- `gcry_random_bytes()` — allocate once, **free with `gcry_free()`** (not `free()`).
- `gcry_md_read()` — returns an **internal pointer** into gcrypt's handle; **never call `free()` on it**.

#### `weechat_string_dyn_free` flag

- `weechat_string_dyn_free(ptr, 0)` — frees only the `char**` container; reallocs `*ptr` to exact size and returns it. `*ptr` is a caller-owned heap string.
  Assign `*ptr` to a `std::unique_ptr<char, decltype(&free)>` or call `free()` when done.
- `weechat_string_dyn_free(ptr, 1)` — frees **both** the `char**` container **and** `*ptr`; `*ptr` is dangling afterwards. Returns `nullptr`.

#### Null-terminated pointer arrays

Use `std::vector<T>` for storage and `std::vector<T*>` for the pointer array (push `nullptr` as terminator):

```cpp
std::vector<t_pre_key> storage;
std::vector<t_pre_key *> ptrs;
// ... populate storage, push &storage.back() into ptrs ...
ptrs.push_back(nullptr);                  // null terminator
func(ptrs.data());                        // pass raw array
```

#### Exception-safe fixed-size arrays

Use `std::make_unique<T[]>(N)` instead of `malloc`:

```cpp
auto buf = std::make_unique<xmpp_stanza_t*[]>(101);
xmpp_stanza_t **children = buf.get();     // auto-freed on scope exit
```

#### LMDB transactions

Always commit **or** abort every transaction — never let one go out of scope uncommitted.

## Architecture

### Key Files

The codebase has been refactored so that large `.inl` implementation fragments are
each compiled as their own translation unit via a thin wrapper `.cpp` in a subdirectory.
**Do not open `.inl` files to read logic — open the corresponding `.cpp` wrapper instead,
which sets up all necessary includes and then `#include`s the `.inl`.**

- **src/connection/helpers.cpp** — anonymous-namespace helpers shared by connection TUs
- **src/connection/presence_handler.cpp** — XMPP presence stanza handler
- **src/connection/message_handler.cpp** — thin adapter; delegates to `src/xmpp/message_*.cpp` slices
- **src/connection/iq_handler.cpp** — thin adapter; delegates to `src/xmpp/iq_*.cpp` slices and `iq_handlers.cpp`
- **src/xmpp/stanza_view.cpp** — inbound stanza reads (`StanzaView`)
- **src/xmpp/iq_handlers.cpp** — pure IQ reply builders (version, time, ping)
- **src/xmpp/message_*.cpp**, **src/xmpp/iq_*.cpp** — protocol parse/render slices (prefer extending these over raw strophe in handlers)
- **src/weechat/ui_port.cpp**, **buffer_port.cpp**, **line_store.cpp**, **render_event.cpp** — WeeChat port implementations
- **src/connection/session_lifecycle.cpp** — stream management + connect/disconnect lifecycle
- **src/connection/internal.hh** — declarations shared across connection TUs
- **src/account/callbacks.cpp** — WeeChat hook callbacks (fd, timer, input, etc.)
- **src/account/lmdb_cache.cpp** — LMDB MAM/caps/OMEMO cache read/write
- **src/account.cpp** — Account object: connect, disconnect, reset, channel/roster management
- **src/command/account.cpp**, **channel.cpp**, **messaging.cpp**, **ephemeral.cpp**,
  **notify.cpp**, **archive.cpp**, **encryption.cpp**, **history.cpp**,
  **presence.cpp**, **roster.cpp**, **rooms.cpp**, **muc_admin.cpp** — one `.cpp` per `/xmpp` sub-command
- **src/channel.cpp** — Chat buffer management (PM and MUC), `send_message`
- **src/config.cpp** — Plugin configuration
- **src/omemo/api.cpp** — Full OMEMO implementation (replaces the old `src/omemo.cpp`)
- **src/pgp.cpp** — PGP encryption support

### Port abstraction (Strophe + WeeChat) — required for new code

Thin ports isolate inbound libstrophe reads and WeeChat output from domain logic.
**All new and touched code must use ports and stanza builders — not raw `weechat_*` or `xmpp_stanza_*` in handler/command/domain logic.** Migrate legacy calls surgically when editing that code path (same PR, no drive-by rewrites).

#### Inbound XMPP (libstrophe)

| Prefer | Instead of |
|--------|------------|
| `xmpp::StanzaView` (`src/xmpp/stanza_view.hh`) | `xmpp_stanza_get_name`, `xmpp_stanza_get_attribute`, `xmpp_stanza_get_children`, manual child-pointer walks |
| `stanza::spec` builders (`src/xmpp/node.hh`, XEP `.inl` files) | `xmpp_stanza_new`, `xmpp_stanza_add_child`, `xmpp_stanza_set_*` |
| `xmpp::handle_*_iq` (`src/xmpp/iq_handlers.hh`) | Inline IQ reply construction in connection handlers |

Handler slices follow **parse → domain struct → render** (`src/xmpp/message_*.cpp`, `src/xmpp/iq_*.cpp`, `src/connection/*_handler.cpp`). Add new protocol features by extending or mirroring these slices — not by growing monolithic `.inl` files with raw API calls.

Connection TUs register thin C adapters: receive `xmpp_stanza_t*`, wrap into `StanzaView`, delegate to pure functions.

#### WeeChat output and host queries

| Port | Header | Role |
|------|--------|------|
| `weechat::UiPort` | `src/weechat/ui_port.hh` | Buffer output (`printf`, `printf_error`, `printf_info`, `printf_network`, `printf_date_tags`); `UiPort::for_buffer()` |
| `weechat::RuntimePort` | `src/weechat/runtime_port.hh` | Host queries (`version_string`, `color`); `RuntimePort::default_runtime()` |
| `weechat::BufferPort` | `src/weechat/buffer_port.hh` | Buffer search + nicklist mutations |
| `weechat::LineStorePort` | `src/weechat/line_store.hh` | Line updates by message tag (receipts, retractions, reactions, tombstones) |
| `RenderEvent` / `UiAction` | `src/weechat/render_event.hh` | Sum type for the handler render step (print, nicklist, line glyph updates) |

Commands and handlers take or create `UiPort` (or return `RenderEvent`s) — not `weechat_printf` / `weechat_buffer_*` in `.inl` logic.

#### C-ABI boundaries (keep at the edge only)

- WeeChat hook/callback registrations (`weechat_*` in `callbacks.cpp` and similar).
- libstrophe `xmpp_handler_add` registrations — handlers receive `xmpp_stanza_t*`, wrap into `StanzaView` on entry.
- LMDB cursors, gcrypt, libsignal handles.

#### Tests

`tests/weechat_stub.hh` provides `CapturingUiPort`, `NullUiPort`, and `StubRuntimePort`. Handler-slice doctests exercise `StanzaView` + `NullUiPort` without a live WeeChat instance.

Migration history: `TODO.md` § Port Abstraction (Waves 1–4 complete).

### Channel Types

Two channel types affect behavior throughout:
- `chat_type::PM` - Private messages (1-on-1)
- `chat_type::MUC` - Multi-user chat (group rooms)

Check channel type before operations that differ (typing indicators, encryption, etc.)

### MAM Timestamp Sentinel Values

Critical for proper PM buffer lifecycle:
- `0` = Brand new channel - fetch 7 days of history
- `-1` = User deliberately closed - skip MAM, don't auto-create from presence
- `>0` = Existing channel - fetch only new messages since timestamp

### LMDB Database Structure

- Location: account-specific `mam_db_path`
- Tables: `messages`, `timestamps`, `capabilities`
- Load caches on account connect
- Save to disk after updates

## Documentation Workflow

### When Adding Features

1. **Implement the feature** in appropriate .cpp/.hh files
2. **Update README.md**:
   - Add to "This Fork" feature list if user-visible
   - Document new commands in Commands section (Org format with examples)
   - Mark relevant TODO items as `[X]`
3. **Update DOAP.xml** if implementing XEP support:
   - Add `<implements>` block with XEP number
   - Set `<xmpp:status>` (complete/partial/planned)
   - Add descriptive `<xmpp:note>`
   - Update version number if needed
4. **Test thoroughly** - build, run, verify functionality
5. **Commit with descriptive message** (see Git Conventions below)

### Documentation Format

- **README.md**: Markdown format (GitHub-flavored)
  - Use `#`, `##`, `###` for heading levels (or keep Org-style `*` headings if present in legacy sections)
  - Code blocks: triple-backtick fences with language (```cpp, ```sh, etc.)
  - Commands: Show usage, examples, notes
  - Keep consistent with existing style in the file

- **DOAP.xml**: RDF/XML format
  - Follow existing structure
  - XEP versions should match spec version implemented
  - Use descriptive notes for clarity

## Release & Packaging

### Version scheme

Xepher follows **semantic versioning** (MAJOR.MINOR.PATCH):
- `PATCH` bump — bug fixes, performance improvements, no new user-visible features
- `MINOR` bump — new user-visible features or significant behaviour changes
- `MAJOR` bump — breaking changes or major architectural rewrites

### Release checklist

1. **All commits pushed** and 109 doctests passing (`make`).
2. **Bump version** in the three packaging files (all must match):
   - `packaging/arch/PKGBUILD` — `pkgver=X.Y.Z`
   - `packaging/rpm/weechat-xmpp.spec` — `Version: X.Y.Z` + new `%changelog` entry
   - `packaging/debian/changelog` — new stanza at the top
3. **Commit** the version bump: `chore: bump packaging to vX.Y.Z`
4. **Tag** the release: `git tag -a vX.Y.Z -m "vX.Y.Z — <one-line summary>"`
5. **Push** commits and tag: `git push && git push origin vX.Y.Z`
6. **Push the tag** — GitHub Actions (`.github/workflows/packages.yml`) builds all five distro packages sequentially and attaches them to the release automatically.
7. **Edit the release** on GitHub if needed (title, notes) — `gh release edit vX.Y.Z`.

### Building packages

**CI (default for releases):** push `vX.Y.Z` tag → Actions runs `packaging/github-build.sh X.Y.Z` → artifacts attached to the release. Monitor with `gh run watch`.

**Local verification before tagging** (requires Docker):
```sh
bash packaging/github-build.sh X.Y.Z              # all distros
bash packaging/github-build.sh X.Y.Z --debian     # single distro
```

**Optional — persistent distrobox containers** (faster iterative local builds):
```sh
bash packaging/distrobox-build.sh X.Y.Z
bash packaging/distrobox-build.sh X.Y.Z --fedora
```

Output lands in `packaging/build/`. All packaging paths pass `PACKAGE_BUILD=1` to `make`.

### Manual release fallback

If CI is unavailable, build locally then:
```sh
gh release create vX.Y.Z \
  --title "vX.Y.Z — <summary>" \
  --notes "..." \
  --target master \
  packaging/build/xepher_X.Y.Z-1_amd64.deb \
  packaging/build/xepher-dbgsym_X.Y.Z-1_amd64.deb \
  packaging/build/xepher-X.Y.Z-1.fcNN.x86_64.rpm \
  packaging/build/xepher-X.Y.Z-1-x86_64.pkg.tar.zst \
  packaging/build/xepher-debug-X.Y.Z-1-x86_64.pkg.tar.zst \
  packaging/build/xepher-X.Y.Z_1.x86_64.xbps \
  packaging/build/xepher-X.Y.Z-r0.apk
```

### Branch protection

`master` is protected on GitHub:
- Force pushes and branch deletion are **blocked**.
- Direct pushes from the sole maintainer are still allowed.
- To change protection rules: `gh api repos/ekollof/xepher/branches/master/protection`



### Commit Message Format

```
<type>: <short description>

[optional detailed explanation]
```

**Types:**
- `feat:` - New feature
- `fix:` - Bug fix
- `docs:` - Documentation only
- `refactor:` - Code restructuring without behavior change
- `test:` - Adding/updating tests
- `chore:` - Maintenance tasks

**Examples:**
```
feat: implement /bookmark command for MUC bookmark management
fix: typing indicator shows nickname in MUC instead of room JID
docs: add comprehensive command documentation to README.md
```

### Commit Workflow

- Make focused, atomic commits (one logical change per commit)
- Test builds before committing
- Update documentation in same commit as feature (keeps history clean)
- Push regularly to backup work

## Common Gotchas & Constraints

### Critical Bugs to Avoid

1. **PM Buffer Recreation**: Three causes must be prevented
   - MAM cache containing old messages
   - MAM fetch logic ignoring closed channels
   - Presence handler auto-creating PM channels
   - Solution: Use timestamp sentinel `-1` and check everywhere

2. **Typing Indicators**: Different display for PM vs MUC
   - MUC: Show nickname (resource part of JID)
   - PM: Show bare JID (user@domain)

3. **Auto-encryption Detection**: Only for PM channels
   - Check channel type before auto-enabling
   - Show notification when auto-enabled

### WeeChat Quirks

- Buffer display `"auto"` causes unwanted buffer switching on plugin load
- Use `"1"` to keep user in current buffer
- Can't safely reload plugin - must restart WeeChat

### Biboumi/IRC Gateway Detection

Skip autojoin for IRC gateway rooms (causes connection issues):
- JID contains `%` character
- JID contains "biboumi"
- JID contains "@irc."

### Testing Limitations

- **109 doctests** cover handler slices, `StanzaView`, IQ builders, and port stubs (`make` runs them automatically) — extend these when adding protocol logic
- Full WeeChat integration still requires manual testing in a live instance
- Use `/debug dump` for troubleshooting
- Check logs: `/set xmpp.look.debug_level 2`

### WeeChat Log Locations

- **Main logs directory**: `~/.local/share/weechat/logs/`
- **XMPP plugin logs**: `~/.local/share/weechat/logs/xmpp.account.<account>.weechatlog`
- **Raw XML log**: `~/.local/share/weechat/xmpp/raw_xml_<account>.log` (only written when `xmpp.look.raw_xml_log on`)
- **OMEMO correlation helper**: `tools/correlate_omemo_xml.sh`
- **MAM LMDB cache inspector**: `tools/dump_mam_db` — dumps all 6 tables in the MAM cache with pretty-printed values. Build via `make -C tools`. Run with `XMPP_ACCOUNT=<account> tools/dump_mam_db` (or `--db <path>`). Tables: `messages` (key: `<channel>:<ts>:<msg_id>`, val: `<from>|<ts>|<body>`), `timestamps` (key: `<channel>`, val: time_t), `retractions`, `cursors` (RSM cursors), `omemo_plaintext`, `capabilities`. Use `--filter <prefix>` to narrow to a specific channel, `--limit N` to cap output, `--table <name>` to dump one table.
- For OMEMO log debugging, always run `tools/correlate_omemo_xml.sh --account <account>` first to correlate event logs with raw XML before proposing protocol-level fixes.
- Example: `tail -n 300 ~/.local/share/weechat/logs/xmpp.account.andrath.weechatlog`
- Filter logs: `grep "OMEMO\|bundle\|devicelist" ~/.local/share/weechat/logs/xmpp.account.*.weechatlog`
- **Note**: Plugin must be restarted (WeeChat closed/reopened) after code changes to test - cannot safely reload in-place

### Debug Options (agents must know these)

The plugin has two independent opt-in debug modes, both off by default.
**Agents investigating protocol issues should enable both before inspecting logs.**

#### `xmpp.look.debug` — verbose protocol buffer

Routes internal protocol messages (PEP, avatar, vCard, OMEMO, stream
management, CSI, upload service, devicelists) to the `xmpp.debug` WeeChat
buffer instead of the account buffer.  Each line has a `[file:line]` prefix.

Enable via the debug socket:
```bash
bash ~/Code/weechat-export/weechat-cmd.sh '/set xmpp.look.debug on'
```
Or directly inside WeeChat:
```
/set xmpp.look.debug on
```
View the buffer:
```
/buffer xmpp.debug
```

**`XDEBUG(...)` log file**: WeeChat writes the `xmpp.debug` buffer to:
```
~/.local/share/weechat/logs/xmpp.debug.weechatlog
```
This is the on-disk record of all `XDEBUG` output. Tail it directly to
monitor debug messages without opening WeeChat:
```bash
tail -f ~/.local/share/weechat/logs/xmpp.debug.weechatlog
```

#### `xmpp.look.raw_xml_log` — wire-level XML file

Appends every SEND and RECV XML stanza to a per-account log file:
```
~/.local/share/weechat/xmpp/raw_xml_<account>.log
```

Enable:
```bash
bash ~/Code/weechat-export/weechat-cmd.sh '/set xmpp.look.raw_xml_log on'
```
Or inside WeeChat:
```
/set xmpp.look.raw_xml_log on
```

Read recent entries:
```bash
tail -n 100 ~/.local/share/weechat/xmpp/raw_xml_<account>.log
```

Search for a specific stanza:
```bash
grep -A 20 "RECV iq" ~/.local/share/weechat/xmpp/raw_xml_<account>.log | head -60
```

#### Workflow for protocol debugging

1. Enable both options (via debug socket or FIFO so WeeChat stays running).
2. Reproduce the issue.
3. Read `xmpp.debug` buffer for high-level protocol event sequence.
4. Cross-reference with `raw_xml_<account>.log` for exact wire content.
5. For OMEMO issues: run `tools/correlate_omemo_xml.sh --account <account>`.
6. Disable both options when done to restore normal behaviour.

### Interacting with a Running WeeChat Instance

Two mechanisms are available for sending commands to or evaluating expressions in a running WeeChat process without touching the terminal.

#### 1. Debug socket (preferred — two-way)

`weechat_debug_socket.py` opens a Unix socket at:

```
$XDG_RUNTIME_DIR/weechat/weechat_debug.sock
# typically: /run/user/1000/weechat/weechat_debug.sock
```

Use the `weechat-cmd.sh` wrapper from `~/Code/weechat-export/`:

```bash
# Eval a WeeChat expression (must use ${...} syntax) — returns result
bash ~/Code/weechat-export/weechat-cmd.sh '${info:version}'
bash ~/Code/weechat-export/weechat-cmd.sh '${weechat.color.chat_bg}'

# Execute a WeeChat command — prints "ok", no output returned
bash ~/Code/weechat-export/weechat-cmd.sh '/xmpp feed andrath@deimos.hackerheaven.org urn:xmpp:microblog:0'
```

Or directly with `socat`:

```bash
echo '${info:version}' | socat - UNIX-CONNECT:/run/user/1000/weechat/weechat_debug.sock
```

**Important**: The script must be loaded in the running WeeChat first. If the
socket is absent, load it via the FIFO (see below):

```bash
echo "*/python load weechat_debug_socket.py" > /run/user/1000/weechat/weechat_fifo_<pid>
```

**Do NOT use the debug socket to run `/plugin reload xmpp`** — unloading the
compiled plugin mid-session crashes WeeChat. Install a new `xmpp.so` to
`~/.local/share/weechat/plugins/` and restart WeeChat instead.

#### 2. FIFO pipe (one-way, no output)

WeeChat creates a FIFO at:

```
$XDG_RUNTIME_DIR/weechat/weechat_fifo_<pid>
# typically: /run/user/1000/weechat/weechat_fifo_<pid>
```

Find the current FIFO:

```bash
ls /run/user/1000/weechat/weechat_fifo_*
```

Send a command (note the `*\t` prefix — `*` means "core buffer", tab-separated):

```bash
echo "*/python load weechat_debug_socket.py" > /run/user/1000/weechat/weechat_fifo_<pid>
```

The FIFO is write-only and returns no output. Use it only when the debug socket
is not yet available (e.g. to bootstrap loading `weechat_debug_socket.py`).
Prefer the debug socket for all other interactions.

**Testing Approach:**
- **Pragmatic manual testing** - The codebase uses minimal automated tests due to:
  - Complex WeeChat plugin API dependencies
  - XMPP protocol interactions requiring real servers
  - Encryption libraries (OMEMO, PGP) hard to mock
- **Test incrementally** - Build and manually verify each feature as implemented
- **Document test procedures** - Keep notes on how to verify critical features
- **Focus on regression prevention** - Manually retest previously fixed bugs

## Development Workflow

0. **Read `AGENTS.md`** — full file at session start and again after any context compaction before continuing
1. **Understand the request** - ask clarifying questions if needed
2. **Explore existing code** - find similar handler slices and port usage (match `StanzaView`/`UiPort` patterns in nearby files)
3. **Make minimal changes** - surgical fixes, don't refactor unnecessarily
4. **Build and test**: `make && <test in WeeChat>`
5. **Update documentation** - README.md, DOAP.xml if applicable
6. **Commit with clear message** - follow conventions above
7. **Push to repository** - backup work regularly

## Manual Testing Checklist

After implementing features, manually verify in WeeChat:

**Critical Features to Retest:**
- [ ] PM buffers don't reappear after `/close`
- [ ] Typing indicators show nicknames (not room JID) in MUC
- [ ] Plain text is default for new PMs (not OMEMO)
- [ ] Auto-encryption enables when receiving encrypted messages
- [ ] Buffer doesn't auto-switch on plugin load
- [ ] `/bookmark` command works (add, list, delete, autojoin)
- [ ] Biboumi/IRC gateway rooms don't autojoin
- [ ] `/list` discovers public rooms
- [ ] `/ping` provides feedback
- [ ] Capability cache persists across restarts

**Basic Smoke Test:**
1. Load plugin: `/plugin load xmpp.so`
2. Connect account: `/xmpp connect <account>`
3. Join MUC: `/join room@conference.server`
4. Send message: Type and press enter
5. Verify typing indicators work
6. Close and verify no recreation: `/close` then reconnect

## Feature Implementation Checklist

When implementing a new feature:

- [ ] Code implementation in appropriate files
- [ ] Build succeeds (`make`)
- [ ] Manual testing in WeeChat (see Manual Testing Checklist)
- [ ] README.md updated (feature list, commands, TODOs)
- [ ] DOAP.xml updated (if XEP-related)
- [ ] Test critical regressions (PM recreation, typing indicators, etc.)
- [ ] Commit with descriptive message
- [ ] Push to repository

## Repository Information

- **Origin**: Fork of bqv/weechat-xmpp
- **Remote**: https://github.com/ekollof/xepher.git
- **Branch**: master (protected — no force-push, no deletion)
- **Language**: C++23 (`-std=c++23`)
- **Dependencies**: libstrophe, LMDB, WeeChat API

### Project URLs

| Resource | URL |
|----------|-----|
| GitHub repository | https://github.com/ekollof/xepher |
| GitHub wiki | https://github.com/ekollof/xepher/wiki — clone with `git clone https://github.com/ekollof/xepher.wiki.git` |
| GitHub Pages (website) | https://ekollof.github.io/xepher — source on the `gh-pages` branch |
| Releases | https://github.com/ekollof/xepher/releases |

When updating documentation:
- **Code-level guidance for agents** → `AGENTS.md` (this file)
- **User-facing docs** → `README.md` (Markdown) and `DOAP.xml` in the repo root
- **Wiki** → clone `https://github.com/ekollof/xepher.wiki.git`, edit, commit, push (no PRs — direct push to `master`)
- **Website** → `gh-pages` branch; edit `index.html` directly

## Useful Commands

```bash
# Build (parallel by default; NPROC overrides core count)
make
CXX="ccache c++" make

# Serial build (debugging only)
make -j1

# Clean build (avoid; prefer ccache incremental)
make clean && make

# Distribution build (no .source embed — matches packaging)
make PACKAGE_BUILD=1 weechat-xmpp

# Package all distros via Docker (same script as CI)
bash packaging/github-build.sh X.Y.Z
bash packaging/github-build.sh X.Y.Z --fedora

# Optional: persistent distrobox package builds
bash packaging/distrobox-build.sh X.Y.Z

# Watch CI package build after tagging
gh run watch --repo ekollof/xepher

# Check git status
git status

# See what changed
git diff

# Correlate OMEMO events with raw XML before OMEMO fixes
tools/correlate_omemo_xml.sh --account <account>

# Commit everything
git add -A && git commit -m "message"

# Push changes
git push
```

## When in Doubt

- **Re-read `AGENTS.md`** if the thread was compacted or you are continuing from a summary
- **Follow existing patterns** in the codebase (ports + stanza builders + modernized `ranges`/`string_view`/`fmt` in touched areas)
- **Make minimal changes** - don't refactor working code; migrate raw `weechat_*` / `xmpp_stanza_*` only in code you are already editing
- **Test incrementally** - build after each logical change
- **Document as you go** - don't leave docs for later
- **Ask before major changes** - discuss architecture decisions
- **Never create loose summary/planning documents** - only update README.md, DOAP.xml, or code comments
