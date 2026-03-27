# -*- coding: utf-8 -*-
# feed_compose.py — open $EDITOR to compose a /feed post, then populate the
# WeeChat input bar so the user can review and press Enter.
#
# Install: copy to ~/.local/share/weechat/python/autoload/ (or load manually
#          with /script load feed_compose.py)
#
# Usage:   /feed-compose [initial text]
# Alias:   /alias add fc /feed-compose
#
# Config (set via /set plugins.var.python.feed_compose.<option> <value>):
#   editor        — editor command; falls back to $EDITOR env var, then "vi"
#   run_externally — "true" to use weechat.hook_process (GUI/tmux editors);
#                    "false" (default) to block in subprocess (vim/nano/etc.)
#
# SPDX-License-Identifier: MIT

import json
import os
import subprocess
import tempfile

import weechat

SCRIPT_NAME = "feed_compose"
SCRIPT_AUTHOR = "weechat-xmpp"
SCRIPT_VERSION = "1.0.0"
SCRIPT_LICENSE = "MIT"
SCRIPT_DESC = "Compose /feed posts in $EDITOR, then populate the input bar"

DEFAULTS = {
    "editor": "",  # falls back to $EDITOR, then vi
    "run_externally": "false",  # true = hook_process (GUI editors)
}


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


def _build_input(buf: str, text: str) -> str:
    """Return the full string to place in the input bar."""
    buf_type = weechat.buffer_get_string(buf, "localvar_type")
    remote_jid = weechat.buffer_get_string(buf, "localvar_remote_jid")

    if buf_type == "feed" and remote_jid:
        # remote_jid is "service/node" for FEED buffers
        service, _, node = remote_jid.partition("/")
        return f"/feed post {service} {node} -- {text}"

    # Not a FEED buffer — let the user fill in service/node themselves
    return f"/feed post -- {text}"


def _finish(buf: str, path: str) -> None:
    """Read the temp file, populate the input bar, clean up."""
    try:
        with open(path, "r", encoding="utf-8") as fh:
            text = fh.read().rstrip()
    except OSError as exc:
        weechat.prnt(buf, f"{SCRIPT_NAME}: error reading temp file: {exc}")
        return
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass

    if not text:
        weechat.prnt(buf, f"{SCRIPT_NAME}: empty content, cancelled")
        return

    cmd = _build_input(buf, text)
    weechat.buffer_set(buf, "input", cmd)
    weechat.buffer_set(buf, "input_pos", str(len(cmd)))
    weechat.command(buf, "/window refresh")


# ---------------------------------------------------------------------------
# hook_process callback (non-blocking / run_externally path)
# ---------------------------------------------------------------------------


def _process_cb(
    raw_data: str, _command: str, return_code: int, _out: str, _err: str
) -> int:
    if return_code < 0:
        return weechat.WEECHAT_RC_OK  # still running

    data = json.loads(raw_data)
    _finish(data["buf"], data["path"])
    return weechat.WEECHAT_RC_OK


# ---------------------------------------------------------------------------
# /feed-compose command handler
# ---------------------------------------------------------------------------


def _cmd_feed_compose(_data: str, buf: str, args: str) -> int:

    editor = _editor_cmd()
    initial = args.strip()

    # Write initial content (from args or empty) to a temp file
    try:
        fd, path = tempfile.mkstemp(suffix=".md", prefix="weechat-feed-")
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            if initial:
                fh.write(initial + "\n")
    except OSError as exc:
        weechat.prnt(buf, f"{SCRIPT_NAME}: cannot create temp file: {exc}")
        return weechat.WEECHAT_RC_ERROR

    run_externally = _cfg("run_externally").strip().lower() == "true"

    if run_externally:
        # Non-blocking: suitable for GUI editors or "tmux new-window vim …"
        full_cmd = f"{editor} {path}"
        data_str = json.dumps({"buf": buf, "path": path})
        weechat.hook_process(full_cmd, 0, "_process_cb", data_str)
    else:
        # Blocking: suitable for terminal editors (vim, nano, …)
        try:
            subprocess.Popen([editor, path]).wait()
        except OSError as exc:
            weechat.prnt(buf, f"{SCRIPT_NAME}: cannot launch editor '{editor}': {exc}")
            try:
                os.unlink(path)
            except OSError:
                pass
            return weechat.WEECHAT_RC_ERROR
        _finish(buf, path)

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
        "Compose a /feed post in $EDITOR, then populate the input bar",
        "[initial text]",
        (
            "Opens $EDITOR (or the configured editor) with a temporary file.\n"
            "On save+quit the content is placed in the WeeChat input bar\n"
            "as a ready-to-send /feed post command.\n\n"
            "Options (set with /set plugins.var.python.feed_compose.<opt>):\n"
            "  editor         — editor binary; falls back to $EDITOR, then vi\n"
            "  run_externally — 'true' for GUI/tmux editors (non-blocking)\n\n"
            "Example alias: /alias add fc /feed-compose\n"
            "Example key:   /key bind meta-e /feed-compose"
        ),
        "",  # no completions
        "_cmd_feed_compose",
        "",
    )
