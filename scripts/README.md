# scripts/

Companion Python scripts for **Xepher** (weechat-xmpp plugin).  
User-level `make install` installs both scripts and enables WeeChat Python
autoload. `make install-scripts` installs them without building the plugin.
Scripts are stored in `python/` with relative links in `python/autoload/`.
The default WeeChat data directory follows `XDG_DATA_HOME`, falling back to
`~/.local/share/weechat`; override it with `WEECHATHOME=/path/to/weechat`.

For direct CMake builds, configure `-DXEPHER_WEECHAT_HOME=/path/to/weechat`
and run `cmake --build build --target install-scripts`.
System/root plugin installation does not install user scripts.

Enable WeeChat's Python plugin. Restart WeeChat to autoload the scripts, or
use `/python load icat.py` and `/python load feed_compose.py` now. For already
loaded scripts, use `/python reload icat.py` or `/python reload feed_compose.py`.
`icat.py` requires Pillow (`python-pillow` or `pip install Pillow`); the installer
does not install Python dependencies or change `xmpp.look.icat`.

## feed_compose.py

Open `$EDITOR` to compose a `/feed post` message, then populate the WeeChat
input bar with the ready-to-send command so you can review it before pressing
Enter.

### Install

Install both companion scripts and their autoload links:

```sh
make install-scripts
```

Or load it once without autoloading:

```
/script load /path/to/scripts/feed_compose.py
```

### Usage

```
/feed-compose [initial text]
```

The script opens a temporary Markdown file in your editor.  
Save and quit — the content is placed in the input bar as a `/feed post …`
command ready to send.

When run from inside a FEED buffer the service JID and node are filled in
automatically from the buffer's local variables.  When run from any other
buffer you will need to add them by hand before pressing Enter.

#### Suggested alias

```
/alias add fc /feed-compose
```

#### Suggested key binding

```
/key bind meta-e /feed-compose
```

### Configuration

All options are set with `/set plugins.var.python.feed_compose.<option>`:

| Option | Default | Description |
|---|---|---|
| `editor` | `""` | Editor binary. Falls back to `$EDITOR`, then `vi`. |
| `run_externally` | `"false"` | `"true"` — non-blocking via `hook_process` (use for GUI editors such as `gvim -f` or `tmux new-window vim`). `"false"` — blocking via `subprocess` (use for terminal editors such as vim or nano). |

### Examples

Use `nano` regardless of `$EDITOR`:

```
/set plugins.var.python.feed_compose.editor nano
```

Use `gvim` in non-blocking mode:

```
/set plugins.var.python.feed_compose.editor gvim -f
/set plugins.var.python.feed_compose.run_externally true
```

Use vim in a new tmux window (non-blocking):

```
/set plugins.var.python.feed_compose.editor tmux new-window vim
/set plugins.var.python.feed_compose.run_externally true
```
