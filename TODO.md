# Xepher — Open Work

Living task list for remaining implementation debt. Completed initiatives (OMEMO BTBV
refactor, XEP compliance emission migration, XEP-0045 MUC features, port abstraction
Waves 0–4) are removed from this file — see git history for archived phase-by-phase plans.

**Verification baseline:** `CXX="ccache clang++" make DEBUG=1` — 127 doctests, all assertions
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

Post-migration audit after UiPort, StanzaView handler slices, RuntimePort, include
normalization, and builder emission work. Surgical/minimal changes only; update this section
when items land.

### Gaps (prioritized)

| Priority | Gap | Location(s) | Remediation |
|----------|-----|-------------|-------------|

| High | Parse utilities on raw libstrophe | `atom.cpp`, `xhtml.cpp`, `util.cpp` | Migrate to `StanzaView` when those paths are next edited |

| Medium | `BufferPort` underused | `nicklist.cpp`, `render_event.cpp`, buffer set/get in `channel.cpp` / `account.cpp` / commands | Extend port (nick add/search); migrate nicklist + render nicklist actions |
| Medium | Manual prefix in dated messages | `message_handler.inl`, `presence_handler.inl` | Use `printf_network` / `printf_error` instead of embedding `RuntimePort::prefix()` in `printf_date_tags` bodies |

| Low | `debug.hh` bypasses ports | `XDEBUG` → raw `weechat_printf` | Optional: route through `UiPort` when refactoring debug path |
| Low | `AGENTS.md` docs drift | Line 180 still cites `weechat_prefix()` | Align with `RuntimePort::default_runtime().prefix()` |

### Acceptable exceptions (no action unless touched)

Port adapters (`ui_port.cpp`, `runtime_port.cpp`, `buffer_port.cpp`, `line_store.cpp`);
`StanzaView` / builder impl (`stanza_view.cpp`, `node.cpp`, `node.hh`, `xep-0163.inl`);
C-ABI glue (`connection.cpp` SM counting, buffer creation, hook callbacks); `strophe.hh`
(parse-only alt wrapper, documented).

### Phase 4 — BufferPort + prefix cleanup

- [ ] Extend `BufferPort` (nicklist add/search; evaluate input/display setters)
- [ ] Migrate `nicklist.cpp`; wire render nicklist actions through port
- [ ] Replace embedded-prefix `printf_date_tags` with typed `UiPort` methods
- [ ] `make DEBUG=1`; manual MUC nicklist + network message display retest

### Phase 5 — Parse utilities (when touched)

- [ ] `atom.cpp`, `xhtml.cpp`, `util.cpp` → `StanzaView`
- [ ] Add/adjust doctests as needed

### Phase 6 — Docs hygiene

- [ ] `AGENTS.md`: align prefix guidance with `RuntimePort`; update stale TODO.md pointer (BTBV refactor complete)

### New XEP support

When adding or modifying XEP support: fetch spec to `docs/specs/xep-NNNN.txt`, verify against
spec, commit spec with code, update DOAP/README, add a row here, `make DEBUG=1`, commit, push.
See AGENTS.md for the full checklist.