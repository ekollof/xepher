# Xepher — Open Work

Living task list for remaining implementation debt. Completed initiatives (OMEMO BTBV
refactor, XEP compliance emission migration, XEP-0045 MUC features, port abstraction
Waves 0–4, Phases 1–6 AGENTS.md compliance) are removed from this file — see git history
for archived phase-by-phase plans.

**Verification baseline:** `CXX="ccache clang++" make DEBUG=1` — 147 doctests, all assertions
green. Manual WeeChat retest after user-visible changes (full restart; no `/plugin reload`).

---

## Not planned

- [ ] **XEP-0353** Jingle Message Initiation (voice/video calls) — requires full Jingle stack
- [ ] **Full modern OMEMO SCE** (Stable Consensus Encryption) — tracked on a separate branch, not `master`

---

## OMEMO maintenance (ongoing)

Axolotl-only + BTBV refactor is **complete on `master`**. On any OMEMO change: verify against
`docs/specs/xep-0384.txt`, run `tools/correlate_omemo_xml.sh --account <account>`, enable
`xmpp.look.debug` + `xmpp.look.raw_xml_log`. No OMEMO:2 primary path; no ATM.

---

## AGENTS.md compliance gaps

Post-migration audit. Surgical/minimal changes only; update this section when items land.

### Gaps (prioritized)

| Priority | Gap | Location(s) | Remediation |
|----------|-----|-------------|-------------|
| Low | `debug.hh` bypasses ports | `XDEBUG` → raw `weechat_printf` | Optional: route through `UiPort` when refactoring debug path |
| Low | Manual prefix outside handlers | `channel.cpp`, `command/*.inl`, `account.cpp`, etc. | Use typed `UiPort` methods when those paths are next edited |

### Acceptable exceptions (no action unless touched)

Port adapters (`ui_port.cpp`, `runtime_port.cpp`, `buffer_port.cpp`, `line_store.cpp`);
`StanzaView` / builder impl (`stanza_view.cpp`, `node.cpp`, `node.hh`, `xep-0163.inl`);
C-ABI glue (`connection.cpp` SM counting, buffer creation, hook callbacks); `strophe.hh`
(parse-only alt wrapper, documented). Action-prefix dated chat lines in `message_handler.inl`
(`/me`, MAM mentions) intentionally embed `prefix("action")` in the message column.
`get_text(child.raw())` in `xhtml.cpp` for XHTML-IM text nodes (libstrophe text-node C API).

### New XEP support

When adding or modifying XEP support: fetch spec to `docs/specs/xep-NNNN.txt`, verify against
spec, commit spec with code, update DOAP/README, add a row here, `make DEBUG=1`, commit, push.
See AGENTS.md for the full checklist.