# -*- coding: utf-8 -*-
# feed_compose.py — open $EDITOR to compose a /feed post or reply, then
# populate the WeeChat input bar so the user can review and press Enter.
#
# Install: copy to ~/.local/share/weechat/python/autoload/ (or load manually
#          with /script load feed_compose.py)
#
# Usage:   /feed-compose [--reply <alias>] [initial text]
# Alias:   /alias add fc /feed-compose
#
# For posts the temp file is pre-filled with YAML frontmatter:
#
#   ---
#   title:
#   ---
#
#   Body text here…
#
# Fill in a title to add an Atom <title> headline.  Leave it blank (or delete
# the frontmatter entirely) to publish body-only — the text goes into
# <content type='text'> with no separate headline, which is the correct format
# for Movim social/microblog posts.
#
# For replies the frontmatter is omitted — comments don't have headlines.
#
# Config (set via /set plugins.var.python.feed_compose.<option> <value>):
#   editor         — editor command; falls back to $EDITOR env var, then "vi"
#   run_externally — "true" to use weechat.hook_process (GUI/tmux editors);
#                    "false" (default) to block in subprocess (vim/nano/etc.)
#
# SPDX-License-Identifier: MIT

import json
import os
import re
import subprocess
import tempfile

import weechat

SCRIPT_NAME = "feed_compose"
SCRIPT_AUTHOR = "Xepher"
SCRIPT_VERSION = "1.3.0"
SCRIPT_LICENSE = "MIT"
SCRIPT_DESC = "Compose /feed posts and replies in $EDITOR, then populate the input bar"

DEFAULTS = {
    "editor": "",  # falls back to $EDITOR, then vi
    "run_externally": "false",  # true = hook_process (GUI editors)
}

_FRONTMATTER_RE = re.compile(r"^\s*---\s*\n(.*?)\n---\s*\n?", re.DOTALL)
_TITLE_RE = re.compile(r"^title\s*:\s*(.*)$", re.MULTILINE)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _cfg(key: str) -> str:
    return weechat.config_get_plugin(key)


def _editor_cmd() -> str:
    cmd = _cfg("editor").strip()
    if not cmd:
        cmd = os.environ.get("EDITOR", "").strip()
    if not cmd:
        cmd = "vi"
    return cmd


def _mouse_enabled() -> bool:
    """Return True if WeeChat mouse support is currently on."""
    opt = weechat.config_get("weechat.look.mouse")
    return bool(opt) and weechat.config_boolean(opt) != 0


def _initial_content(initial_body: str = "", is_reply: bool = False) -> str:
    """Return the template written to the temp file.

    For replies the title frontmatter block is omitted entirely — comments
    don't have headlines, and showing the field would only confuse the user.
    """
    if is_reply:
        return initial_body if initial_body else ""
    return f"---\ntitle: \n---\n\n{initial_body}"


def _parse_content(raw: str) -> tuple[str, str]:
    """
    Parse raw editor content into (title, body).

    Strips YAML frontmatter if present and extracts the `title:` field.
    Returns (title, body) both stripped of leading/trailing whitespace.
    An empty or absent title means no <title> element will be emitted.
    """
    title = ""
    body = raw

    m = _FRONTMATTER_RE.match(raw)
    if m:
        fm_block = m.group(1)
        body = raw[m.end() :]
        tm = _TITLE_RE.search(fm_block)
        if tm:
            title = tm.group(1).strip()

    body = body.strip()
    return title, body


def _build_input(buf: str, title: str, body: str, reply_id: str = "") -> str:
    """Return the full string to place in the input bar."""
    buf_type = weechat.buffer_get_string(buf, "localvar_type")
    remote_jid = weechat.buffer_get_string(buf, "localvar_remote_jid")

    # Encode title for embedding in the command line.
    # We pass it as: --title <title> --
    # The C++ parser reads everything between --title and -- as the title.
    title_flag = f"--title {title} " if title else ""

    if reply_id:
        if buf_type == "feed" and remote_jid:
            return f"/feed reply {reply_id} {title_flag}-- {body}"
        if remote_jid:
            service, _, node = remote_jid.partition("/")
            return f"/feed reply {service} {node} {reply_id} {title_flag}-- {body}"
        return f"/feed reply {reply_id} {title_flag}-- {body}"

    if buf_type == "feed" and remote_jid:
        service, _, node = remote_jid.partition("/")
        return f"/feed post {service} {node} {title_flag}-- {body}"

    return f"/feed post {title_flag}-- {body}"


def _finish(buf: str, path: str, reply_id: str, mouse_was_on: bool) -> None:
    """Read the temp file, populate the input bar, clean up."""
    if mouse_was_on:
        weechat.command(buf, "/mouse enable")

    try:
        with open(path, "r", encoding="utf-8") as fh:
            raw = fh.read()
    except OSError as exc:
        weechat.prnt(buf, f"{SCRIPT_NAME}: error reading temp file: {exc}")
        weechat.buffer_set(buf, "input", "")
        weechat.buffer_set(buf, "input_pos", "0")
        weechat.command(buf, "/redraw")
        return
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass

    title, body = _parse_content(raw)

    if not body:
        weechat.prnt(buf, f"{SCRIPT_NAME}: empty content, cancelled")
        weechat.buffer_set(buf, "input", "")
        weechat.buffer_set(buf, "input_pos", "0")
        weechat.command(buf, "/redraw")
        return

    cmd = _build_input(buf, title, body, reply_id)
    weechat.buffer_set(buf, "input", cmd)
    weechat.buffer_set(buf, "input_pos", str(len(cmd)))
    weechat.command(buf, "/redraw")


# ---------------------------------------------------------------------------
# hook_process callback (non-blocking / run_externally path)
# ---------------------------------------------------------------------------


def _process_cb(
    raw_data: str, _command: str, return_code: int, _out: str, _err: str
) -> int:
    if return_code < 0:
        return weechat.WEECHAT_RC_OK  # still running

    data = json.loads(raw_data)
    _finish(data["buf"], data["path"], data["reply_id"], data["mouse_was_on"])
    return weechat.WEECHAT_RC_OK


# ---------------------------------------------------------------------------
# /feed-compose command handler
# ---------------------------------------------------------------------------


def _cmd_feed_compose(_data: str, buf: str, args: str) -> int:
    # Parse optional --reply <alias> flag
    reply_id = ""
    tokens = args.split()
    if len(tokens) >= 2 and tokens[0] == "--reply":
        reply_id = tokens[1]
        initial_body = " ".join(tokens[2:])
    else:
        initial_body = args.strip()

    editor = _editor_cmd()

    # Write frontmatter + initial body to a temp file
    try:
        fd, path = tempfile.mkstemp(suffix=".md", prefix="weechat-feed-")
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(_initial_content(initial_body, is_reply=bool(reply_id)))
    except OSError as exc:
        weechat.prnt(buf, f"{SCRIPT_NAME}: cannot create temp file: {exc}")
        return weechat.WEECHAT_RC_ERROR

    run_externally = _cfg("run_externally").strip().lower() == "true"
    mouse_was_on = _mouse_enabled()

    if run_externally:
        full_cmd = f"{editor} {path}"
        data_str = json.dumps(
            {
                "buf": buf,
                "path": path,
                "reply_id": reply_id,
                "mouse_was_on": False,
            }
        )
        weechat.hook_process(full_cmd, 0, "_process_cb", data_str)
    else:
        if mouse_was_on:
            weechat.command(buf, "/mouse disable")
        try:
            subprocess.Popen([editor, path]).wait()
        except OSError as exc:
            weechat.prnt(buf, f"{SCRIPT_NAME}: cannot launch editor '{editor}': {exc}")
            if mouse_was_on:
                weechat.command(buf, "/mouse enable")
            try:
                os.unlink(path)
            except OSError:
                pass
            return weechat.WEECHAT_RC_ERROR
        _finish(buf, path, reply_id, mouse_was_on)

    return weechat.WEECHAT_RC_OK


# ---------------------------------------------------------------------------
# Registration
# ---------------------------------------------------------------------------

if weechat.register(
    SCRIPT_NAME, SCRIPT_AUTHOR, SCRIPT_VERSION, SCRIPT_LICENSE, SCRIPT_DESC, "", ""
):
    for key, default in DEFAULTS.items():
        if not weechat.config_is_set_plugin(key):
            weechat.config_set_plugin(key, default)

    weechat.hook_command(
        "feed-compose",
        "Compose a /feed post or reply in $EDITOR, then populate the input bar",
        "[--reply <alias>] [initial text]",
        (
            "Opens $EDITOR (or the configured editor) with a temporary Markdown\n"
            "file pre-filled with YAML frontmatter (posts only):\n\n"
            "  ---\n"
            "  title: \n"
            "  ---\n\n"
            "  Body text here…\n\n"
            "Set title: to add an Atom <title> headline to the post.  Leave it\n"
            "blank (or delete the frontmatter) to publish without a headline —\n"
            "the body goes into <content> directly, which is what Movim expects\n"
            "for social/microblog posts.\n\n"
            "For replies (--reply) the frontmatter is omitted — comments don't\n"
            "have headlines.\n\n"
            "  --reply <alias>  Compose a reply to item <alias> (e.g. #3).\n\n"
            "Mouse support is automatically disabled before launching a\n"
            "blocking terminal editor and re-enabled afterwards.\n\n"
            "Options (set with /set plugins.var.python.feed_compose.<opt>):\n"
            "  editor         — editor binary; falls back to $EDITOR, then vi\n"
            "  run_externally — 'true' for GUI/tmux editors (non-blocking)\n\n"
            "Example alias: /alias add fc /feed-compose\n"
            "Example key:   /key bind meta-e /feed-compose"
        ),
        "",
        "_cmd_feed_compose",
        "",
    )
