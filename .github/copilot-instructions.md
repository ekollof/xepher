# GitHub Copilot Instructions for weechat-xmpp

## Project Overview

This is a WeeChat plugin for XMPP/Jabber written in C++. It uses libstrophe for XMPP protocol handling and LMDB for local persistence (MAM cache, capabilities cache). This is a fork with bug fixes and feature enhancements.

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

- Use smart pointers where appropriate
- Clean up strophe allocations promptly
- Be careful with string lifetimes from strophe (may be temporary)
- LMDB transactions: always commit or abort

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

## Git Conventions

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
- **Remote**: git.hackerheaven.org:ekollof/weechat-xmpp-fixed.git
- **Branch**: master
- **Language**: C++23 (`-std=c++23`)
- **Dependencies**: libstrophe, LMDB, WeeChat API

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
