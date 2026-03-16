# weechat-xmpp-fixed

A WeeChat plugin written in C/C++23 that adds full XMPP support, including a
comprehensive set of XEPs targeting CCS2022 compliance.

> **Fork of [bqv/weechat-xmpp](https://github.com/bqv/weechat-xmpp)**  
> Original author: **Tony Olagbaiye** &lt;bqv@fron.io&gt;  
> This fork is maintained at
> [git.hackerheaven.org/ekollof/weechat-xmpp-improved](https://git.hackerheaven.org/ekollof/weechat-xmpp-improved)
> and carries critical bug fixes, new XEP implementations, and ongoing
> refactoring not yet merged upstream.

---

## What's different in this fork

### Bug fixes

- **Memory corruption**: Fixed `xmlDocDumpFormatMemory` misuse and unaligned
  allocations that caused crashes.
- **Segfault prevention**: Added comprehensive destructor safety checks to
  prevent crashes on WeeChat exit.
- **WeeChat 4.3.0+ compatibility**: Updated base64 API calls for newer WeeChat
  while retaining backwards compatibility.
- **MAM (Message Archive Management)**:
  - Fixed IQ routing: PM queries were sent to the contact's JID instead of the
    local server domain.
  - Fixed UTC timestamp parsing: `strptime` fills a UTC `struct tm` but
    `mktime` was interpreting it as local time, shifting all timestamps.
  - Fixed multi-page MAM results: the global discovery query stalled after the
    first RSM page and never fetched newer messages.
  - Fixed single-page / empty `<fin>` handling: a server-sent
    `<fin complete='true'/>` with no `<set>` child left the query dangling.
  - Fixed channel re-open suppression: a `-1` sentinel in LMDB prevented
    auto-recreation of closed buffers, so MAM never ran for those channels.
  - Enabled automatic MAM retrieval on connect (7-day history for PM channels).
  - Full message caching with LMDB — instant display from cache, then fetch new
    messages.
- **Channel type detection**: Fixed PM vs MUC detection for proper message
  handling.

### Display improvements

- Messages show sender's bare JID instead of client resource (e.g.
  `user@domain.org` instead of `Conversations.xxxx`).
- Typing indicators show bare JID instead of resource in both the buffer and
  the bar.
- Encryption status indicator in the status bar (OMEMO / PGP / plaintext).
- Message styling (XEP-0393): renders **bold**, _italic_, `code`,
  ~~strikethrough~~, and quotes.
- Reduced logging noise: debug-level log spam disabled by default.
- OMEMO PM noise cleanup: high-frequency per-device status messages removed
  while preserving actionable queue/error output.

### OMEMO interoperability

- Publishes both OMEMO:2 and legacy OMEMO:1 (axolotl) devicelist/bundle nodes
  on connect and republish.
- Per-device mode resolution (OMEMO:2 vs legacy) for bundle bootstrap and key
  transport.
- Suppresses repeated "no key for our device" spam once bootstrap is pending.
- Prevents payloadless OMEMO key-transport stanzas from creating PM buffers.

### New features

| Feature | XEP |
|---------|-----|
| Stream Management | XEP-0198 |
| Blocking Command (`/block`, `/unblock`, `/blocklist`) | XEP-0191 |
| MUC Self-Ping (`/selfping`) | XEP-0410 |
| Personal Eventing Protocol | XEP-0163 |
| Direct MUC Invitations (`/invite`) | XEP-0249 |
| The `/me` Command | XEP-0245 |
| vcard-temp (`/whois`) | XEP-0054 |
| MUC leave on `/close` | XEP-0045 |
| XMPP Ping (`/ping`) | XEP-0199 |
| Message Correction (`/edit`) | XEP-0308 |
| Message Retraction (`/retract`) | XEP-0424 |
| Message Moderation (`/moderate`) | XEP-0425 |
| Message Styling | XEP-0393 |
| Message Reactions (`/react`) | XEP-0444 |
| Message Replies (`/reply`) | XEP-0461 |
| Unique and Stable Stanza IDs | XEP-0359 |
| Message Processing Hints | XEP-0334 |
| Enhanced Chat State Notifications | XEP-0085 |
| HTTP File Upload (`/upload`) | XEP-0363 |
| User Mood (`/mood`) | XEP-0107 |
| User Activity (`/activity`) | XEP-0108 |
| PEP Native Bookmarks (`/bookmark`) | XEP-0402 |
| PGP key persistence | — |
| Encryption status bar item | — |
| Roster management (`/roster`) | RFC 6121 |
| Public room search (`/list`) | XEP-0433 |
| Entity Capability caching | XEP-0115 |
| User Avatar (colored Unicode symbols) | XEP-0084 |
| MUC status code handling | XEP-0045 |
| Ad-hoc Commands (`/adhoc`) | XEP-0050 |
| XHTML-IM rendering | XEP-0071 |
| Message Displayed Synchronization | XEP-0490 / XEP-0283 |
| References (`@mentions`) | XEP-0372 |
| Message status glyphs (⌛ ✓ ✓✓) | XEP-0184 / XEP-0333 |
| Link Metadata / previews | XEP-0511 |

> **Known limitation:** Plugin reload while WeeChat is running is not
> supported due to fundamental race conditions with timer hooks. Always
> restart WeeChat instead of reloading the plugin.

---

## XMPP Compliance (XEP-0459: CCS2022)

### Core IM

- ✅ XEP-0030: Service Discovery
- ✅ XEP-0045: Multi-User Chat (core features)
- ✅ XEP-0054: vcard-temp (retrieval via `/whois`)
- ✅ XEP-0115: Entity Capabilities (persistent caching)
- ✅ XEP-0163: Personal Eventing Protocol
- ✅ XEP-0191: Blocking Command
- ✅ XEP-0198: Stream Management

### Advanced IM

- ✅ XEP-0245: The `/me` Command
- ✅ XEP-0249: Direct MUC Invitations
- ✅ XEP-0308: Last Message Correction
- ✅ XEP-0363: HTTP File Upload
- ✅ XEP-0393: Message Styling
- ✅ XEP-0410: MUC Self-Ping
- ✅ XEP-0424: Message Retraction
- ✅ XEP-0425: Message Moderation
- ✅ XEP-0444: Message Reactions
- ✅ XEP-0461: Message Replies

### Additional implemented XEPs

- ✅ XEP-0004: Data Forms (rendered in-buffer for Ad-Hoc Commands)
- ✅ XEP-0048: Bookmark Storage (Private XML)
- ✅ XEP-0049: Private XML Storage
- ✅ XEP-0050: Ad-Hoc Commands
- ✅ XEP-0059: Result Set Management (MAM paging)
- ✅ XEP-0066: Out of Band Data
- ✅ XEP-0071: XHTML-IM (bold, italic, underline, code, blockquotes, links, CSS colors)
- ✅ XEP-0085: Chat State Notifications (all 5 states)
- ✅ XEP-0092: Software Version
- ✅ XEP-0107: User Mood
- ✅ XEP-0108: User Activity
- ✅ XEP-0153: vCard-Based Avatars (receive; publishing not yet implemented)
- ✅ XEP-0172: User Nickname
- ✅ XEP-0184: Message Delivery Receipts
- ✅ XEP-0199: XMPP Ping
- ✅ XEP-0202: Entity Time
- ✅ XEP-0203: Delayed Delivery
- ✅ XEP-0223: Persistent Storage of Private Data via PubSub
- ✅ XEP-0224: Attention
- ✅ XEP-0280: Message Carbons
- ✅ XEP-0283: Moved (account migration notices)
- ✅ XEP-0297: Forwarded Messages
- ✅ XEP-0300: Cryptographic Hash Functions
- ✅ XEP-0313: Message Archive Management (with LMDB caching)
- ✅ XEP-0319: Last User Interaction in Presence
- ✅ XEP-0333: Chat Markers
- ✅ XEP-0334: Message Processing Hints
- ✅ XEP-0352: Client State Indication
- ✅ XEP-0359: Unique and Stable Stanza IDs
- ✅ XEP-0372: References
- ✅ XEP-0380: Explicit Message Encryption
- ✅ XEP-0382: Spoiler Messages
- ✅ XEP-0384: OMEMO Encryption
- ✅ XEP-0385: Stateless Inline Media Sharing
- ✅ XEP-0392: Consistent Color Generation
- ✅ XEP-0402: PEP Native Bookmarks
- ✅ XEP-0422: Message Fastening
- ✅ XEP-0428: Fallback Indication
- ✅ XEP-0490: Message Displayed Synchronization
- ✅ XEP-0511: Link Metadata (incoming previews + outgoing OpenGraph)
- ⚡ XEP-0433: Extended Channel Search (Searcher role; Search Service role not implemented)

### Planned

- ⏳ XEP-0153: vCard-Based Avatars (publishing own avatar)
- ⏳ XEP-0398: Avatar Conversion

---

## Installation

### Dependencies

| Library | Type |
|---------|------|
| libstrophe | runtime |
| libxml2 | runtime |
| lmdb | runtime |
| libomemo-c (libsignal-protocol-c) | runtime |
| gpgme | runtime |
| libfmt | runtime |
| g++ >= GCC 12 | build |
| bison | build |
| flex | build |
| doctest | test |
| WeeChat >= 3.0 | runtime |

### Supported platforms

The plugin is developed and tested on **Linux** (Arch, Debian/Ubuntu, Fedora).

**FreeBSD, OpenBSD, and NetBSD** receive best-effort support: the build system
and scripts have been ported to POSIX sh and BSD-compatible make, but these
platforms are **not routinely tested**. Known considerations:

- Use `gmake` instead of `make` on BSD (BSD make has different syntax).
- `libsignal-protocol-c` and `libfmt` may not be available in base package
  repositories on OpenBSD/NetBSD and may need to be built from ports/pkgsrc.
- The `DEBUG=1` address-sanitizer flags (`-lasan -lrt`) are Linux-only and are
  automatically skipped on other platforms.
- `objcopy` is Linux-specific; the `.source` section embedding step is silently
  skipped when `objcopy`/`llvm-objcopy` is not found.

### Build

```sh
git clone --depth 1 git@git.hackerheaven.org:ekollof/weechat-xmpp-improved.git
cd weechat-xmpp-fixed
make install-deps   # installs system packages (requires sudo)
make
make test
make install        # installs to ~/.local/share/weechat/plugins/ — do NOT run as root
```

On BSD, replace `make` with `gmake` throughout.

`make install-deps` automatically detects your distribution (Debian/Ubuntu,
Fedora/RHEL, Arch, openSUSE, Void, Alpine, Gentoo, FreeBSD, OpenBSD, NetBSD)
and installs the required packages.

### WeeChat version compatibility

WeeChat 4.3.0 (May 2024) changed the base64 API. This fork defaults to
`>= 4.3.0`. If you get a compilation error like
`invalid conversion from 'int' to 'const char*'`, open `omemo.cpp`, find this
block near line 42, and change `#if 1` to `#if 0`:

```cpp
#if 1  // Set to 0 for WeeChat < 4.3.0
    #define WEECHAT_BASE64 "64"
#else
    #define WEECHAT_BASE64 64
#endif
```

Then rebuild with `make clean && make`.

---

## Usage

### Quick start

```
/account add work user@example.com mypassword
/account connect work
```

Use `/help account` for full details.

### Encryption status indicator

Add the `xmpp_encryption` bar item to your status bar to see the current
buffer's encryption mode (🔒OMEMO / 🔒PGP / empty for plaintext):

```
/set weechat.bar.status.items "[time],[buffer_last_number],[buffer_plugin],buffer_number+:+buffer_name+(buffer_modes)+{buffer_nicklist_count}+buffer_zoom+buffer_filter,xmpp_encryption,[lag],[hotlist],completion"
```

### Typing indicator

1. Enable typing in WeeChat:

   ```
   /set typing.look.enabled_nicks on
   /set typing.look.enabled_self on
   /set typing.look.input_min_chars 2
   /save typing
   ```

2. Add the typing bar item:

   ```
   /set weechat.bar.typing.items "typing"
   /save
   ```

### Message status glyphs

Sent messages display an inline status glyph that updates automatically:

| Glyph | Meaning |
|-------|---------|
| ⌛ | Pending — sent, no receipt yet |
| ✓ | Delivered — recipient's client received it (XEP-0184) |
| ✓✓ | Read — recipient marked it displayed (XEP-0333) |

### XHTML-IM rendering (XEP-0071)

When a message contains an `<html>` body, the plugin renders it with WeeChat
color/attribute support instead of raw markup.

| Element | Rendered as |
|---------|-------------|
| `<b>`, `<strong>` | Bold |
| `<i>`, `<em>` | Italic |
| `<u>` | Underline |
| `<del>`, `<s>`, `<strike>` | Darkgray |
| `<code>`, `<tt>` | Gray monospace |
| `<pre>` | Gray-wrapped preformatted block |
| `<a href="...">text</a>` | `text (blue URL)` |
| `<img alt="..."/>` | `[alt]` placeholder |
| `<br>` | Newline |
| `<blockquote>` | Green `>` prefix |
| `<span style="color:...">` | Terminal color (16 named + `#rgb` hex) |

### Link previews (XEP-0511)

**Incoming:** When a message contains `<rdf:Description>` (XEP-0511) carrying
OpenGraph metadata, a compact preview is rendered:

```
alice: Check this out! https://example.com/article
        ┌ The Best Webpage
        │ This is a great webpage and you will really like it
        └ https://example.com/canonical-url [img]
```

**Outgoing:** URLs in your messages automatically trigger a background fetch;
a metadata stanza is sent as a follow-up so the recipient can render a preview
even without server-side injection. To disable:

```
/set xmpp.look.outgoing_link_preview off
```

### Ad-hoc Commands and Data Forms (XEP-0050 / XEP-0004)

```
/adhoc example.com                          # list available commands
/adhoc example.com announce                 # execute a command (form rendered inline)
/adhoc example.com announce <id> subject=Hello body=World
```

Required fields are marked with `*`. Multi-step sessions are supported.

---

## Command reference

### Account management

```
/account list
/account add <name> <jid> <password>
/account connect <account>
/account disconnect <account>
/account reconnect <account>
/account delete <account>
```

### Messaging

| Command | Description |
|---------|-------------|
| `/enter <jid>` | Join a MUC room |
| `/open <jid>` | Open a direct chat (PM) |
| `/msg <text>` | Send a message to the current buffer |
| `/me <text>` | Send a `/me` action (XEP-0245) |
| `/invite <jid> [reason]` | Invite a user to the current MUC (XEP-0249) |
| `/selfping` | Verify MUC membership (XEP-0410) |
| `/edit <text>` | Correct last sent message (XEP-0308) |
| `/retract` | Delete last sent message (XEP-0424) |
| `/moderate [reason]` | Remove another user's message as moderator (XEP-0425) |
| `/react <emoji>` | React to the last received message (XEP-0444) |
| `/reply <text>` | Reply with context to the last received message (XEP-0461) |
| `/whois [jid]` | Retrieve vCard information (XEP-0054) |

### Privacy & blocking

| Command | Description |
|---------|-------------|
| `/block <jid> [...]` | Block one or more JIDs (XEP-0191) |
| `/unblock [jid ...]` | Unblock JIDs, or all if no argument given |
| `/blocklist` | Show all blocked JIDs |

### Encryption

| Command | Description |
|---------|-------------|
| `/omemo` | Enable OMEMO for the current buffer |
| `/omemo check` | Verify OMEMO bundle is published |
| `/omemo republish` | Republish OMEMO:2 + legacy nodes |
| `/omemo status` | Show device ID and status |
| `/omemo reset-keys` | Reset key database (forces renegotiation) |
| `/omemo fetch [jid] [device-id]` | Force devicelist/bundle refresh |
| `/omemo kex [jid] [device-id]` | Force key transport now |
| `/pgp [keyid\|status\|reset]` | Manage PGP encryption |
| `/plain` | Disable encryption (use plaintext) |

### File sharing

```
/upload [filename]   # interactive picker if no filename given
```

### Archive & history

```
/mam [days]   # default: 7 days
```

### Service discovery & roster

| Command | Description |
|---------|-------------|
| `/disco [jid]` | Discover services and features (XEP-0030) |
| `/adhoc <jid> [node] [id] [field=value ...]` | Execute ad-hoc commands (XEP-0050) |
| `/roster` | Display contact list |
| `/roster add <jid> [name]` | Add a contact |
| `/roster del <jid>` | Remove a contact |
| `/list [keywords]` | Search public MUC rooms (XEP-0433) |
| `/bookmark` | List bookmarks |
| `/bookmark add [jid] [name]` | Add a bookmark |
| `/bookmark del <jid>` | Remove a bookmark |
| `/bookmark autojoin <jid> <on\|off>` | Toggle autojoin |

### Network & status

| Command | Description |
|---------|-------------|
| `/ping [jid]` | Send XMPP ping (XEP-0199) |
| `/mood [mood [text]]` | Publish mood via PEP (XEP-0107) |
| `/activity [category[/specific] [text]]` | Publish activity via PEP (XEP-0108) |
| `/xml <stanza>` | Send raw XML (advanced/debug) |
| `/xmpp` | Show plugin version |
| `/trap` | Trigger debug breakpoint (developers only) |

---

## OMEMO log correlation

When diagnosing key transport or session bootstrap issues, correlate WeeChat
log events with raw XML:

```sh
tools/correlate_omemo_xml.sh --account <account>
```

---

## Contributing

Pull requests and issues are welcome.  
Please keep to the existing indentation style (C++23, clang-format enforced).

---

## License

Licensed under the **Mozilla Public License 2.0**.  
See [LICENSE](LICENSE) or https://www.mozilla.org/en-US/MPL/2.0/

Original project © Tony Olagbaiye — https://github.com/bqv/weechat-xmpp
