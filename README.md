# Xepher

![Xepher logo](assets/xepher.png)

Xepher is a WeeChat plugin written in C++23 that adds full XMPP support to
WeeChat. It targets [XMPP Compliance Suite 2022 (XEP-0459)](https://xmpp.org/extensions/xep-0459.html)
and implements a broad set of modern XEPs — including OMEMO encryption, Message
Archive Management, HTTP file upload, microblogging via PubSub, and more.

> **Fork of [bqv/weechat-xmpp](https://github.com/bqv/weechat-xmpp)**  
> Original author: **Tony Olagbaiye** &lt;bqv@fron.io&gt;  
> Maintained at [github.com/ekollof/xepher](https://github.com/ekollof/xepher)

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
git clone --depth 1 git@github.com:ekollof/xepher.git
cd xepher
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

### Message Correction — `/edit` and `/edit-to` (XEP-0308)

Opens an **interactive picker** showing your last 20 non-retracted sent messages
so you can select which one to correct, then pre-fills the input bar with the
original text ready for editing.

```
/edit                   # open interactive picker — select message, edit in input bar
/edit-to <id> <text>    # correct a specific message by ID (used internally by picker)
```

**How it works:**

1. Type `/edit` with no arguments to open the interactive picker.
2. Navigate with `↑`/`↓` and press `Enter` to select the message you want to
   correct.
3. The picker closes and the input bar is pre-filled with
   `/edit-to <id> <original text>`.  Edit the text and press `Enter` to send.
4. A correction stanza is sent referencing the original message ID. The local
   buffer updates the line with an `[edit]` prefix.

**Notes:**

- In MUC rooms, correction uses the server-assigned stanza-id (XEP-0359) when
  available, falling back to the origin-id.
- Recipients who do not support XEP-0308 will receive the correction as a new
  message with a `[CORRECTION]` fallback body.
- You must be connected and inside a chat buffer (PM or MUC) to use `/edit`.

**Example:**

```
you: Hello eweryone!
/edit
[interactive picker opens — select the message, press Enter]
> input bar is pre-filled: /edit-to abc123 Hello eweryone!
> edit to: /edit-to abc123 Hello everyone!   [Enter]
you: [edit] Hello everyone!
```

---

### Message Retraction — `/retract` (XEP-0424)

Deletes one of your own messages from the current buffer.

```
/retract
```

Takes no arguments. Opens an **interactive picker** showing your last 20
non-retracted messages. Navigate with `↑`/`↓`, press `Enter` to retract the
selected message, or `q`/`Esc` to cancel.

**How it works:**

1. An interactive picker opens listing your last 20 sent messages (most recent
   at the top).
2. Select the message to retract and press `Enter`.
3. A retraction stanza is sent referencing that message's ID. In MUC rooms the
   server-assigned stanza-id (XEP-0359) is used, which is required by the spec.
4. Supporting clients replace the original message with a tombstone. The local
   buffer shows `[Message deleted]` in place of the original text.
5. A `store` hint (XEP-0334) is included so MAM archives record the retraction,
   and other devices that sync history will also see the tombstone.

**Notes:**

- Retraction is **not a guarantee**: recipients who already received the message
  keep it. Clients that do not support XEP-0424 will instead display the
  fallback body: `[user] retracted a previous message, but it's unsupported by
  your client.`
- You cannot retract another user's message with `/retract`. Use `/moderate`
  (XEP-0425) for that (requires MUC moderator role).
- You must be connected and inside a chat buffer (PM or MUC) to use `/retract`.

**Example:**

```
you: I should not have said that
/retract
[interactive picker opens — select the message, press Enter]
-- xmpp: message retraction sent
you: [Message deleted]
```

---

### Message Reactions — `/react` (XEP-0444)

Sends an emoji reaction to the last received message in the current buffer.

```
/react <emoji>
```

**How it works:**

1. Type `/react` followed by one or more emoji (the entire argument after
   `/react` is used as the reaction string).
2. The plugin finds the most recent message in the buffer that was *not* sent
   by you (identified by having an `id_` tag and no `self_msg` tag).
3. A reaction stanza is sent referencing that message's ID.

**Notes:**

- Reactions target the last *incoming* message, not your own messages.
- In MUC rooms, the stanza-id (XEP-0359) is used when available.
- Multiple emoji in a single argument are sent as one reaction (e.g.,
  `/react 👍❤️` sends the two-emoji string as a single reaction).
- Clients that do not support XEP-0444 ignore the reaction stanza silently.
- You must be connected and inside a chat buffer to use `/react`.

**Examples:**

```
/react 👍
/react ❤️
/react 😂
/react 🎉
```

---

### Message Replies — `/reply` and `/reply-to` (XEP-0461)

Sends a reply with a message reference so supporting clients can display a
quoted preview.

```
/reply                  # open interactive picker to choose which message to reply to
/reply <text>           # reply immediately to the last received message
/reply-to <id> <text>   # reply to a specific message by ID (used internally by picker)
```

**How `/reply` works (no arguments — interactive picker):**

1. An interactive picker opens listing the last 20 messages received from others
   (non-own, non-retracted).
2. Navigate with `↑`/`↓` and press `Enter` to select.
3. The picker closes and the input bar is pre-filled with `/reply-to <id> `.
4. Type your reply text and press `Enter` to send.

**How `/reply <text>` works (with text):**

The plugin finds the most recent non-own message and sends a reply directly,
without opening the picker. Useful for quick replies.

**Notes:**

- Clients that do not support XEP-0461 will display the plain message text
  without the quoted context.
- In MUC rooms, the MUC-assigned stanza-id (XEP-0359) is used when present,
  as required by the spec for rooms with `stanza-id` support.
- You must be connected and inside a chat buffer to use `/reply`.

**Examples:**

```
/reply                        # opens picker
/reply Thanks, that helped!   # immediate reply to last received message
```

---

### Message Moderation — `/moderate` (XEP-0425)

Removes another user's message from a MUC room (requires moderator role).

```
/moderate [reason]
```

Opens an **interactive picker** showing the last 20 non-retracted messages in
the room. Navigate with `↑`/`↓`, press `Enter` to moderate the selected
message, or `q`/`Esc` to cancel. The optional reason is forwarded in the
moderation stanza.

**How it works:**

1. An interactive picker opens listing the last 20 messages in the MUC buffer
   (both own and others', excluding already-retracted messages).
2. Select the message to moderate and press `Enter`.
3. A moderation stanza is sent referencing the server-assigned stanza-id
   (XEP-0359), which is required by XEP-0425 for MUC moderation.
4. Supporting clients replace the original message with a moderation tombstone.

**Notes:**

- Requires MUC moderator role. The server will reject the request otherwise.
- Use `/retract` to delete your *own* messages instead.
- Moderation is **not a guarantee**: clients that do not support XEP-0425 may
  still show the original message.
- Must be used inside a MUC buffer (not a PM channel).

**Examples:**

```
/moderate
/moderate Spam message
/moderate Violates community guidelines
```

---

### Microblogging — `/feed` (XEP-0277 / XEP-0472)

Publish and interact with microblog posts on PubSub services such as
[Movim](https://movim.eu/) and [Libervia/Salut-à-Toi](https://libervia.org/).
XMPP microblogging is XMPP's equivalent of a federated social timeline — posts
are Atom entries stored on PubSub nodes; contacts who subscribe to your
`urn:xmpp:microblog:0` PEP node receive your posts in real-time.

> **Note:** XEP-0277 is Deferred and XEP-0472 is Experimental. Server support
> varies. Movim and Libervia implement the full stack; ejabberd supports the
> underlying XEP-0060 mechanics.

**Short aliases (#N):**

When you fetch a feed, each item is assigned a short `#N` alias shown in green.
Use it in place of the full item-id in any write command:

```
/feed reply #3 That's a great point!
/feed repeat #3 Great content
/feed comments #3
/feed retract #3
```

**Publishing a post:**

```
# Long form
/feed post movim.eu urn:xmpp:microblog:0 Hello from WeeChat!
/feed post movim.eu myblog --open My public post     # --open = public node
/feed post movim.eu myblog --title My Headline -- Body text   # with Atom title

# Short form from a feed buffer (service/node inferred)
/feed post Hello everyone!
/feed post -- https://example.com check this out     # '--' avoids JID-like parse

# Compose in $EDITOR (requires feed_compose.py)
/feed post --edit
```

Sends an Atom `<entry>` to the PubSub node with `pubsub#type=urn:xmpp:microblog:0`
and the `urn:xmpp:pubsub-social-feed:1` publish-option required by XEP-0472.
Failed publishes are reported in the buffer with the server error condition.

**Embedding media in a post (XEP-0363 + XEP-0447):**

Top-level microblog posts (not comments) support Jinja2-style embed tags that
upload files via HTTP File Upload and insert XEP-0447 `<file-sharing>` children
into the Atom entry:

```
{{ embed "photo.jpg" }}                  # inline image → ![photo.jpg](url)
{{ embed "photo.jpg" alt="sunset" }}     # with alt text
{{ attach "paper.pdf" }}                 # download link → [paper.pdf](url)
{{ attach "paper.pdf" alt="My Paper" }}
{{ video "demo.mp4" }}                   # inline video → ![demo.mp4](url)
{{ video "demo.mp4" alt="Demo" }}
```

- `embed` and `video` set `disposition='inline'`; `attach` sets `disposition='attachment'`
- Files are uploaded one at a time (async); the Atom entry is published only when
  all uploads have completed
- On any upload failure the post is **aborted** and a draft is saved to
  `$weechat_data_dir/xmpp/drafts/<account>-<timestamp>.md` with YAML frontmatter;
  the draft path is printed in the feed buffer
- Embed tags are not supported in comments nodes; attempting them prints an error

Receiving clients that support XEP-0447 (Conversations, Dino, Gajim) render the
media natively.  Xepher displays a summary line for each attachment:

```
[Image: photo.jpg (image/jpeg, 1.2 MB) https://…]
[File:  paper.pdf (application/pdf, 340 KB) https://…]
```

**Replying to a post (threaded):**

```
/feed reply movim.eu urn:xmpp:microblog:0 abc123 That's a great point!
/feed reply #3 That's a great point!    # short form using alias
/feed reply #3 --edit                   # compose reply in $EDITOR
```

Adds a `thr:in-reply-to` element (RFC 4685) referencing the original item.
Clients such as Movim render threaded conversations from these references.

**Boosting / repeating a post (XEP-0472 §4.5):**

```
/feed repeat movim.eu urn:xmpp:microblog:0 abc123
/feed repeat movim.eu urn:xmpp:microblog:0 abc123 Great post!
/feed repeat #3                         # short form using alias
/feed repeat #3 Great content!          # short form with comment
```

Publishes a new entry with `<link rel='via'>` pointing to the original item.
An optional comment is included in the entry body.

**Retracting (deleting) a post:**

```
/feed retract movim.eu urn:xmpp:microblog:0 abc123
/feed retract #3    # short form using alias
```

Sends a PubSub retract IQ. Server errors are reported in the buffer.

**Fetching comments for a post:**

```
/feed comments movim.eu urn:xmpp:microblog:0 abc123
/feed comments #3   # short form using alias
```

If the post carried a `<link rel='replies'>` element (XEP-0277 §4.1), this
fetches the linked comments node into a dedicated FEED buffer. The hint is shown
in the feed buffer as `Comments: /feed comments #N`. Run
`/feed movim.eu urn:xmpp:microblog:0` first if no hint is shown.

**Subscribing and unsubscribing:**

```
/feed subscribe news.movim.eu Phoronix
/feed unsubscribe news.movim.eu Phoronix
/feed subscriptions news.movim.eu
```

Sends XEP-0060 subscribe/unsubscribe IQ stanzas. Feedback (success or error
condition) is printed in the originating buffer.

**Receiving posts from contacts:**

Incoming PEP pushes from contacts' `urn:xmpp:microblog:0` nodes are
automatically rendered in the WeeChat buffer for that contact, showing the
author name, timestamp, content body, and — when present — threaded reply
references (`thr:in-reply-to`), boost/repeat provenance (`Repeated from:`),
comments links (`Comments: /feed comments #N`), category tags, enclosure URLs,
and geolocation. XHTML and HTML content is rendered with WeeChat formatting.
Duplicate items (already seen via IQ fetch) are suppressed via LMDB deduplication.

**Reading a microblog feed:**

```
/feed movim.eu urn:xmpp:microblog:0
/feed movim.eu urn:xmpp:microblog:0 --limit 5
/feed movim.eu urn:xmpp:microblog:0 --before <item-id>   # older page (RSM)
/feed movim.eu urn:xmpp:microblog:0 --latest             # reset cursor; newest page
```

The RSM cursor is persisted in LMDB. Use `--latest` to go back to the newest
page after paging back through older items.

**Auto-discovering your server's PubSub services:**

At connect time the plugin queries your server's `disco#items` (XEP-0030) to
find all components, then queries `disco#info` on each one. Any component
reporting `identity category='pubsub'` is remembered. You can then use:

```
/feed                        # fetch subscriptions from all discovered services
/feed discover               # list discovered services without fetching
/feed discover --all         # fetch every node from every discovered service
```

---

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
/account register <name> <jid> <password>   # create account on server (XEP-0077)
/account unregister <account>               # cancel account on server (XEP-0077)
/account password <account> <new-password>  # change password in-band (XEP-0077)
```

### Messaging

| Command | Description |
|---------|-------------|
| `/enter <jid>` | Join a MUC room |
| `/join <jid>` | Alias for `/enter` |
| `/open <jid>` | Open a direct chat (PM) |
| `/query <jid> [msg]` | Alias for `/open` |
| `/msg <text>` | Send a message to the current buffer |
| `/me <text>` | Send a `/me` action (XEP-0245) |
| `/ephemeral <seconds> <text>` | Send an ephemeral message that disappears after N seconds (XEP-0466) |
| `/invite <jid> [reason]` | Invite a user to the current MUC (XEP-0249) |
| `/selfping` | Verify MUC membership (XEP-0410) |
| `/edit` | Picker: choose a sent message to correct (XEP-0308) |
| `/edit-to <id> <text>` | Correct a specific message by ID — used by the `/edit` picker |
| `/retract` | Picker: choose a sent message to delete (XEP-0424) |
| `/moderate [reason]` | Picker: choose a MUC message to moderate (XEP-0425) |
| `/react <emoji>` | React to the last received message (XEP-0444) |
| `/reply` | Picker: choose a message to reply to (XEP-0461) |
| `/reply <text>` | Reply to the last received message (XEP-0461) |
| `/reply-to <id> <text>` | Reply to a specific message by ID (XEP-0461) |
| `/spoiler [hint:] <text>` | Send a spoiler message (XEP-0382) |
| `/buzz` | Send attention request to current PM contact (XEP-0224) |
| `/whois [jid]` | Retrieve vCard information (XEP-0054) |
| `/setvcard <field> <value>` | Publish a vCard field (XEP-0054) |
| `/setavatar <filepath>` | Publish avatar image (XEP-0084) |

### MUC room management

| Command | Description |
|---------|-------------|
| `/kick <nick> [reason]` | Kick a user from the MUC (requires moderator role) |
| `/ban <jid> [reason]` | Ban a user by JID (requires admin/owner role) |
| `/topic [text]` | Set or clear the room topic |
| `/nick [newnick]` | Change your nickname in the current MUC |

### MUC nicklist prefixes

Each nick in a MUC room displays a prefix indicating their role or affiliation
(XEP-0045). Affiliation takes precedence over role when both apply.

| Prefix | XEP-0045 role/affiliation | Meaning |
|--------|--------------------------|---------|
| `~` | affiliation: owner | Room owner — full control |
| `&` | affiliation: admin | Administrator — can grant/revoke roles |
| `@` | role: moderator | Can kick and mute participants |
| `%` | affiliation: member | Registered member (voice in members-only rooms) |
| `+` | role: participant | Can send messages (standard occupant) |
| `?` | role: visitor | Read-only in a moderated room |
| `!` | affiliation: outcast | Banned from the room |
| `.` | *(none)* | No role or affiliation set yet |

### Privacy & blocking

| Command | Description |
|---------|-------------|
| `/block <jid> [...]` | Block one or more JIDs (XEP-0191) |
| `/unblock [jid ...]` | Unblock JIDs, or all if no argument given |
| `/blocklist` | List all blocked JIDs |

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

Uploads a file via HTTP File Upload (XEP-0363) and announces it in the current
channel using a three-layer stanza for maximum client compatibility:

1. **XEP-0447 Stateless File Sharing** (`urn:xmpp:sfs:0`, `disposition='inline'`) —
   used by Conversations ≥ 2.10, Dino, and Gajim to display inline image previews
   without the user having to click a link.
2. **XEP-0385 SIMS** (`urn:xmpp:sims:1`) — older inline media sharing, understood
   by clients that pre-date XEP-0447.
3. **XEP-0066 OOB** (`jabber:x:oob`) — plain URL fallback; renders as a clickable
   link in any client that does not understand the above.

For JPEG and PNG images the plugin automatically detects `<width>` and `<height>`
from the file's binary headers (JPEG SOF markers, PNG IHDR chunk) and includes
them in the XEP-0446 `<file>` element so receiving clients can pre-size preview
boxes before the image finishes downloading.

Incoming XEP-0447 file-sharing stanzas sent by other clients are also parsed and
displayed, deduplicated against any SIMS or OOB element for the same URL so the
file appears only once per message.

### Archive & history

```
/mam [days]                          # fetch history (default: 7 days)
/mam prefs                           # show MAM preferences (XEP-0441)
/mam prefs default <always|never|roster>  # set default archiving policy
/mam prefs always <jid>              # add JID to always-archive list
/mam prefs never <jid>               # add JID to never-archive list
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
| `/feed` | Fetch subscriptions from all auto-discovered PubSub services (XEP-0060) |
| `/feed discover` | List auto-discovered PubSub services |
| `/feed discover --all` | Fetch every node from every discovered service |
| `/feed <service-jid>` | Fetch all subscribed nodes on a service |
| `/feed <service-jid> --all` | Discover and fetch all nodes via disco#items |
| `/feed <service-jid> <node>` | Fetch a specific node into a dedicated buffer |
| `/feed ... --limit N` | Override the per-node item limit (default: 20) |
| `/feed ... --before <id>` | Fetch items older than `<id>` (XEP-0059 RSM paging) |
| `/feed ... --latest` | Clear saved RSM cursor; return to the newest page |
| `/feed subscribe <service> <node>` | Subscribe to a PubSub node |
| `/feed unsubscribe <service> <node>` | Unsubscribe from a PubSub node |
| `/feed subscriptions <service>` | List subscribed nodes on a service |
| `/feed post <service> <node> [--open] [--title <t>] <text>` | Publish a microblog Atom entry (XEP-0472); `--title` sets Atom `<title>` |
| `/feed post [--open] [--title <t>] <text>` | Short form: post from a feed buffer (service/node inferred) |
| `/feed post --edit` | Open `$EDITOR` via `feed_compose.py`; YAML frontmatter for optional title |
| `/feed post -- <text>` | `--` separator: body starts with JID-like word or URL |
| `/feed reply <service> <node> <item-id\|#N> <text>` | Reply with `thr:in-reply-to` threading (no title) |
| `/feed reply #N <text>` | Short form reply using item alias |
| `/feed reply #N --edit` | Compose reply in `$EDITOR` via `feed_compose.py` (no frontmatter) |
| `/feed repeat <service> <node> <item-id> [comment]` | Boost/repeat a post (XEP-0472 §4.5) |
| `/feed repeat #N [comment]` | Short form boost using item alias |
| `/feed retract <service> <node> <item-id>` | Retract (delete) a published post |
| `/feed retract #N` | Short form retract using item alias |
| `/feed comments <service> <node> <item-id\|#N>` | Fetch comments node for a post (XEP-0277) |
| `/feed comments #N` | Short form fetch comments using item alias |
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

### Interactive picker UI

Several commands open an in-buffer interactive picker when invoked without
arguments. Picker key bindings:

| Key | Action |
|-----|--------|
| `↑` / `↓` | Navigate up/down |
| `Enter` | Confirm selection |
| `q` / `Esc` | Cancel and return to previous buffer |

---

## Debugging

Three complementary mechanisms are available for diagnosing protocol issues.
All are opt-in and off by default so normal users see no extra noise.

### Verbose protocol log — `xmpp.look.debug`

Routes internal protocol messages (PEP events, avatar updates, vCard
auto-fetches, OMEMO device lists and session bootstrap, stream management,
client state, upload service discovery) to a dedicated **`xmpp.debug`** buffer
(`xmpp.debug` in WeeChat buffer notation) instead of cluttering account
buffers.

```
/set xmpp.look.debug on
```

The buffer is created lazily on first use. Each line carries a dim
`[file:line]` source prefix so you can trace back to the exact code path.

To jump to the debug buffer:

```
/buffer xmpp.debug
```

To turn it off again (messages stop appearing; buffer remains open):

```
/set xmpp.look.debug off
```

### Raw XML stanza log — `xmpp.look.raw_xml_log`

Appends every inbound (`RECV`) and outbound (`SEND`) XML stanza to a
per-account log file on disk. Useful when you need the full wire-level picture
for protocol analysis or when filing bug reports.

```
/set xmpp.look.raw_xml_log on
```

Log files are written to:

```
~/.local/share/weechat/xmpp/raw_xml_<account>.log
```

Each entry is formatted as:

```
[YYYY-MM-DD HH:MM:SS] SEND|RECV <stanza-name>
<full xml text>

```

Turn off to stop writing (existing log file is kept):

```
/set xmpp.look.raw_xml_log off
```

### OMEMO log correlation helper

When diagnosing OMEMO key transport or session bootstrap issues, the
`correlate_omemo_xml.sh` helper cross-references WeeChat log events with raw
XML stanzas so you can see what the plugin received at the exact moment an
error occurred:

```sh
tools/correlate_omemo_xml.sh --account <account>
```

This is most useful when `xmpp.look.raw_xml_log` is also enabled, since the
helper reads from `raw_xml_<account>.log`.

---

## Companion scripts

Optional Python scripts live in `scripts/`.  They are **not** loaded
automatically — install the ones you want by hand.

### feed_compose.py — compose posts in `$EDITOR`

Opens a temporary Markdown file in your editor.  For **posts** the file
includes a YAML frontmatter block with an optional `title:` field — fill it in
to set the Atom `<title>` headline, or leave it blank for a body-only post.
For **replies** and when run from a **comments buffer** the frontmatter is
omitted automatically (comments carry no Atom title).  On save the content is
placed in the WeeChat input bar as a `/feed post …` or `/feed reply …` command
ready to review and send.

The easiest way to invoke it is with the `--edit` flag:

```
/feed post --edit          # new post in $EDITOR
/feed reply #3 --edit      # reply in $EDITOR
```

Or install and invoke directly:

```sh
cp scripts/feed_compose.py ~/.local/share/weechat/python/autoload/
```

Inside WeeChat:

```
/alias add fc /feed-compose
/key bind meta-e /feed-compose
```

See [`scripts/README.md`](scripts/README.md) for full configuration options.

---

## Contributing

Pull requests and issues are welcome.  
Please keep to the existing indentation style (C++23, clang-format enforced).

---

## XMPP Compliance (XEP-0459: CCS2022)

### Core IM

- ✅ XEP-0030: Service Discovery
- ✅ XEP-0045: Multi-User Chat (core features)
- ✅ XEP-0054: vcard-temp (retrieval via `/whois`, publishing via `/setvcard`)
- ✅ XEP-0077: In-Band Registration (`/account register`, `unregister`, `password`)
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
- ✅ XEP-0292: vCard4 Over XMPP (fetch via `/whois`; no publish support yet)
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
- ✅ XEP-0446: File Metadata Element (image dimensions sent with uploads)
- ✅ XEP-0447: Stateless File Sharing (inline previews for Conversations/Dino/Gajim)
- ✅ XEP-0490: Message Displayed Synchronization
- ✅ XEP-0511: Link Metadata (incoming previews + outgoing OpenGraph)
 - ⚡ XEP-0277: Microblogging over XMPP (Deferred — publish/reply/retract via `/feed post|reply|retract`; receive PEP microblog push events; Atom metadata: title, author, categories, enclosures, geolocation, replies links; `/feed comments` fetches comments nodes)
 - ⚡ XEP-0394: Message Markup (Experimental — receive-only: `<markup xmlns='urn:xmpp:markup:0'>` parsed and rendered to WeeChat colour codes; `<span emphasis>` → italic, `<span strong>` → bold, `<span code>` → cyan, `<span deleted>` → grey, `<bcode>` → grey block, `<bquote>` → green-prefixed lines, `<list>/<li>` → bullet markers; takes precedence over XEP-0393 when present; advertised in caps)
- ⚡ XEP-0413: Order-By for MAM (Experimental — `urn:xmpp:order-by:1 field='creation-date'` included in XEP-0442 pubsub MAM queries for newest-first ordering; advertised in caps)
 - ⚡ XEP-0442: Pubsub MAM (Experimental — on reconnect, disco#info probes each pubsub service; if `urn:xmpp:mam:2` is supported, fetches feed history via MAM with `node=` filter and XEP-0413 Order-By; falls back to XEP-0060 `max_items` for servers without pubsub MAM support; Atom entries extracted from forwarded MAM result messages and rendered to the feed buffer with dedup and alias assignment; RSM cursor persisted to LMDB)
 - ⚡ XEP-0452: MUC Mention Notifications (Experimental — receive side: `<addresses type='mentioned'>` notification messages from MUC service detected and forwarded message body rendered to the MUC buffer with highlight, so missed @mentions surface even when not present in the room)
 - ⚡ XEP-0441: MAM Preferences (Experimental — `/mam prefs` displays current server default policy and always/never JID lists; `/mam prefs default <always|never|roster>` sets default; `/mam prefs always <jid>` / `/mam prefs never <jid>` add JIDs to the respective list)
 - ⚡ XEP-0466: Ephemeral Messages (Experimental — send: `/ephemeral <seconds> <message>` attaches `<ephemeral timer='N'/>` + `<no-permanent-store/>` hint; receive: timer value displayed as `[⏱ Ns]` prefix, message automatically tombstoned after N seconds; `urn:xmpp:ephemeral:0` advertised in caps)
 - ⚡ XEP-0472: Pubsub Social Feed (Experimental — publishes `pubsub#type=urn:xmpp:microblog:0`, advertises `urn:xmpp:pubsub-social-feed:1` in caps; `thr:in-reply-to@ref` uses real Atom entry IRI; feed-level `<feed>` metadata items rendered; XHTML/HTML Atom content properly rendered)
- ⚡ XEP-0433: Extended Channel Search (Searcher role; Search Service role not implemented)

### Planned

- ⏳ XEP-0153: vCard-Based Avatars (publishing own avatar)
- ⏳ XEP-0398: Avatar Conversion

---

> **Known limitation:** Plugin reload while WeeChat is running is not
> supported due to fundamental race conditions with timer hooks. Always
> restart WeeChat instead of reloading the plugin.

---

## License

Licensed under the **Mozilla Public License 2.0**.  
See [LICENSE](LICENSE) or https://www.mozilla.org/en-US/MPL/2.0/

Original project © Tony Olagbaiye — https://github.com/bqv/weechat-xmpp
