# GitHub Copilot Instructions for weechat-xmpp

## Project Overview

This is a WeeChat plugin for XMPP/Jabber written in C++. It uses libstrophe for XMPP protocol handling and LMDB for local persistence (MAM cache, capabilities cache). This is a fork with bug fixes and feature enhancements.

## OMEMO Protocol Reference

- Primary OMEMO specification: https://xmpp.org/extensions/xep-0384.html
- Local implementation checklist: `docs/specs/xep-0384-reference.md`
- For OMEMO changes, always verify stanza structure, namespaces, and pubsub node behavior against XEP-0384 before finalizing code.
- Prefer strict OMEMO:2 compliance. If a compatibility fallback is added, keep it explicit, narrow, and documented.

## Build System

- **Build command**: `make`
- **Clean command**: `make clean`
- **Output**: `xmpp.so` (WeeChat plugin)
- **Dependencies**: Managed via git submodules in `deps/`
- Always run `make` after code changes to verify compilation

## Coding Style & Practices

### General C++ Guidelines

- Minimize changes - make surgical, targeted fixes
- Use existing code style and patterns consistently
- **Use C++23 features** (project standard via `-std=c++23`)
- Use `nullptr` not `NULL`
- RAII for resource management
- Keep functions focused and concise

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

### WeeChat Plugin Conventions

- All WeeChat API calls start with `weechat_`
- Use `weechat_prefix()` for message coloring
- Hook functions must match WeeChat signatures exactly
- Buffer display values: `"1"` (don't auto-switch), `"auto"` (auto-switch)
- Never reload plugin - always restart WeeChat (race condition with timer hooks)

### XMPP/Strophe Patterns

- Use `xmpp_stanza_new()` family for stanza creation
- Always set namespace with `xmpp_stanza_set_ns()`
- Use `xmpp_uuid_gen()` for generating UUIDs (returns `char*`)
- Free strophe objects with `xmpp_stanza_release()`, `xmpp_free()`
- Handler functions return `int` (1 = keep handler, 0 = remove handler)

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

#### Strophe stanza ownership helpers (`src/xmpp/stanza.hh`)

- `with_free(ptr)` — transfers heap ownership to the stanza; the stanza will call `free()`.
  Do **not** also wrap `ptr` in a smart pointer.
- `with_noop(ptr)` — stanza **does not** take ownership; caller must keep the value alive
  until the stanza is consumed, then free it via RAII (smart pointer or `std::string`).

#### `weechat_string_dyn_free` flag

- `weechat_string_dyn_free(ptr, 0)` — frees the `char**` container **and** `*ptr`; `*ptr` is dangling afterwards.
- `weechat_string_dyn_free(ptr, 1)` — frees only the container; `*ptr` is a caller-owned heap string.
  Assign `*ptr` to a `std::unique_ptr<char, decltype(&free)>` or call `free()` when done.

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

- **connection.cpp**: XMPP stanza handlers (presence, message, IQ)
- **account.cpp**: Account management, cache operations
- **channel.cpp**: Chat buffer management (PM and MUC)
- **command.cpp**: WeeChat command implementations
- **config.cpp**: Plugin configuration
- **omemo.cpp/pgp.cpp**: Encryption support

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
2. **Update README.org**:
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

- **README.org**: Emacs Org-mode format
  - Use `*`, `**`, `***` for heading levels
  - Code blocks: `#+BEGIN_SRC ... #+END_SRC`
  - Commands: Show usage, examples, notes
  - Keep consistent with existing style

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

1. **All commits pushed** and 15 unit tests passing (`make`).
2. **Bump version** in the three packaging files (all must match):
   - `packaging/arch/PKGBUILD` — `pkgver=X.Y.Z`
   - `packaging/rpm/weechat-xmpp.spec` — `Version: X.Y.Z` + new `%changelog` entry
   - `packaging/debian/changelog` — new stanza at the top
3. **Commit** the version bump: `chore: bump packaging to vX.Y.Z`
4. **Tag** the release: `git tag -a vX.Y.Z -m "vX.Y.Z — <one-line summary>"`
5. **Push** commits and tag: `git push && git push origin vX.Y.Z`
6. **Build packages** (see below).
7. **Create GitHub release** and attach binaries (see below).

### Building packages

Use the distrobox build script — it spins up isolated containers for each
distro, builds, then tears them down:

```sh
# Build all five formats (Debian, Fedora, Arch, Void, Alpine)
bash packaging/distrobox-build.sh X.Y.Z

# Build a single format
bash packaging/distrobox-build.sh X.Y.Z --debian
bash packaging/distrobox-build.sh X.Y.Z --fedora
bash packaging/distrobox-build.sh X.Y.Z --arch
bash packaging/distrobox-build.sh X.Y.Z --void
bash packaging/distrobox-build.sh X.Y.Z --alpine
```

Output lands in `packaging/build/`. Prerequisites: `distrobox` + `docker` or
`podman` installed and accessible to the current user.

The Alpine and Void builds use `docker run` directly (no distrobox container)
and must run as root inside the container — this is handled automatically.

### Creating the GitHub release

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
docs: add comprehensive command documentation to README.org
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

- No automated test suite for WeeChat plugins
- Manual testing required in WeeChat
- Use `/debug dump` for troubleshooting
- Check logs: `/set xmpp.look.debug_level 2`

### WeeChat Log Locations

- **Main logs directory**: `~/.local/share/weechat/logs/`
- **XMPP plugin logs**: `~/.local/share/weechat/logs/xmpp.account.<account>.weechatlog`
- **Raw XML log**: `~/.local/share/weechat/xmpp/raw_xml_<account>.log` (only written when `xmpp.look.raw_xml_log on`)
- **OMEMO correlation helper**: `tools/correlate_omemo_xml.sh`
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

1. **Understand the request** - ask clarifying questions if needed
2. **Explore existing code** - find similar patterns to follow
3. **Make minimal changes** - surgical fixes, don't refactor unnecessarily
4. **Build and test**: `make && <test in WeeChat>`
5. **Update documentation** - README.org, DOAP.xml if applicable
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
- [ ] README.org updated (feature list, commands, TODOs)
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
- **Code-level guidance for agents** → `.github/copilot-instructions.md` (this file)
- **User-facing docs** → `README.org` and `DOAP.xml` in the repo root
- **Wiki** → clone `https://github.com/ekollof/xepher.wiki.git`, edit, commit, push (no PRs — direct push to `master`)
- **Website** → `gh-pages` branch; edit `index.html` directly

## Useful Commands

```bash
# Build
make

# Clean build
make clean && make

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

- **Follow existing patterns** in the codebase
- **Make minimal changes** - don't refactor working code
- **Test incrementally** - build after each logical change
- **Document as you go** - don't leave docs for later
- **Ask before major changes** - discuss architecture decisions
- **Never create loose summary/planning documents** - only update README.org, DOAP.xml, or code comments
