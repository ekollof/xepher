# Xepher — Open Work

Living task list for remaining implementation debt. Completed initiatives are in git
history (OMEMO BTBV refactor, port abstraction, XEP-0045 MUC, CMake migration, etc.).

**Verify changes:** `make DEBUG=1` (147 doctests). Restart WeeChat after rebuilding the
plugin — do not `/plugin reload xmpp`.

---

## Open tasks

_None._

---

## Not on `master`

| Item | Reason |
|------|--------|
| XEP-0353 Jingle (voice/video) | Requires a full Jingle stack |

---

## When you touch…

**OMEMO** — `docs/specs/xep-0384.txt`, `docs/planning-muc-omemo.md`, `tools/correlate_omemo_xml.sh`.
MUC OMEMO is implemented but lightly tested in production.

**A new or changed XEP** — fetch spec to `docs/specs/`, update DOAP/README, `make DEBUG=1`.
Full checklist: `AGENTS.md`.

**Coding style / ports / stanza rules** — `AGENTS.md` (authoritative; not duplicated here).