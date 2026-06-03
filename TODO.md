# OMEMO Refactor: Axolotl-only + BTBV TOFU

Branch: `omemo-axolotl-btbv`

## Goal

Collapse the dual OMEMO:2 / axolotl implementation down to a single
`eu.siacs.conversations.axolotl` stack, replace the XEP-0450 ATM trust model
with Gajim-style BTBV (Blind Trust Before Verification) TOFU, and remove all
dead OMEMO:2 code paths.

---

## Trust Model (BTBV)

| Value | Name       | Meaning                                          |
|-------|------------|--------------------------------------------------|
| 0     | UNTRUSTED  | Explicitly distrusted by user                    |
| 1     | VERIFIED   | Manually verified by user (fingerprint)          |
| 2     | UNDECIDED  | New device for JID that already has trusted keys |
| 3     | BLIND      | Auto-trusted (TOFU — first device for this JID)  |

**`get_default_trust(jid)`**: scan `trust:{jid}:*` LMDB keys; if any has value
VERIFIED(1) or UNTRUSTED(0) → return UNDECIDED(2); else → BLIND(3).

**Encrypt gate**: only VERIFIED(1) and BLIND(3) devices receive key material.

**LMDB key**: `trust:{bare_jid}:{device_id}` → decimal string `"0"`–`"3"`

---

## Status: ALL PHASES COMPLETE ✓

### Phase 0 — Planning & branch setup
- [x] Create `omemo-axolotl-btbv` branch
- [x] Write this TODO.md
- [x] Update `.github/copilot-instructions.md`

### Phase 1 — LMDB trust schema (`internal_prelude.inl`)
- [x] Add `enum class omemo_trust` with UNTRUSTED/VERIFIED/UNDECIDED/BLIND
- [x] Add `key_for_tofu_trust`, `store_tofu_trust`, `load_tofu_trust`
- [x] Add `get_default_trust(self, jid)` (BTBV scan)
- [x] Remove ATM trust helpers (`key_for_atm_trust`, etc.)
- [x] Remove `sce_wrap()`, `sce_wrap_content()`, `make_rpad()`
- [x] Remove `store_device_mode()`, `load_device_mode()`
- [x] Remove OMEMO:2 constants (`kOmemoNs`, `kDevicesNode`, `kBundlesNode`)
- [x] Remove `atm_fingerprint_b64()`, `key_for_devicelist()`, `utc_timestamp_now()`, `xml_escape()`
- [x] Remove truncated `hmac_sha256` declaration

### Phase 2 — Signal store trust callbacks (`internal_signal_store.inl`)
- [x] Rewrite `identity_save()`: BTBV `get_default_trust` on new identity
- [x] Rewrite `identity_is_trusted()`: VERIFIED/BLIND → 1; UNTRUSTED/UNDECIDED → 0
- [x] Remove unused `extract_devices_from_items`
- [x] Remove unused `extract_bundle_from_items` (and its orphaned body in `internal_stanza_parse.inl`)

### Phase 3 — Strip ATM (`session_flow.inl`, `commands.inl`)
- [x] Remove ATM functions from `commands.inl`
- [x] Remove ATM/OMEMO:2 code from `session_flow.inl`
- [x] Remove unused `devicelist_contains_device` static
- [x] Remove stale `request_devicelist` wrapper body
- [x] Replace thin `send_key_transport` wrapper with full inline implementation
- [x] Remove `/omemo approve`, `/omemo optout`, `/omemo optout-ack` subcommands
- [x] Rewrite `distrust_fp` to accept `std::optional<uint32_t>` device_id

### Phase 4 — Collapse encode/decode to axolotl-only (`codec.inl`)
- [x] Remove OMEMO:2 encode path
- [x] Remove OMEMO:2 decode path
- [x] Remove `peer_mode` dispatch
- [x] Fix `static_cast<int>(trust)` where trust is `std::optional<omemo_trust>`

### Phase 5 — Remove OMEMO:2 crypto (`internal_crypto.inl`)
- [x] Remove `omemo2_encrypt()`, `omemo2_decrypt()`
- [x] Remove orphaned `hmac_sha256` function body tail

### Phase 6 — Remove OMEMO:2 lifecycle (`lifecycle.inl`)
- [x] Remove `get_bundle()` (OMEMO:2 PEP publish path)
- [x] Remove `handle_devicelist()` (OMEMO:2 handler)
- [x] Remove `missing_omemo2_devicelist` tracking

### Phase 7 — Remove OMEMO:2 from connect lifecycle (`connect_lifecycle.inl`)
- [x] Stop fetching/publishing OMEMO:2 devicelist/bundle on connect

### Phase 8 — Remove OMEMO:2 from IQ/message handlers
- [x] `iq_handler.inl`: remove OMEMO:2 IQ dispatch, unused variables
- [x] `message_handler.inl`: remove ATM blocks, OMEMO:2 PEP handler, XEP-0434 feature ad

### Phase 9 — Clean up `omemo.hh`
- [x] Remove `peer_mode` enum
- [x] Remove `pending_atm_trust_from_unauthenticated`, `pending_atm_trust_for_unknown_key`
- [x] Remove `missing_omemo2_devicelist`, `omemo_opted_out_peers`
- [x] Remove OMEMO:2 / ATM method declarations
- [x] Add `trust_jid`, updated `distrust_fp` declarations

### Phase 10 — Config cleanup
- [x] `omemo_atm` config boolean left in place (referenced by existing config files)
       but no longer used by any OMEMO code path

### Phase 11 — Update `/omemo` commands
- [x] `commands.inl`: trust/distrust use `omemo_trust::` enum values
- [x] `command/encryption.inl`: `distrust_fp` call-site updated
- [x] `command/channel.inl`: `request_axolotl_devicelist` call updated
- [x] `command.cpp`: help text cleaned

### Phase 12 — Account API
- [x] `account.hh` / `account.cpp`: `get_legacy_devicelist()` renamed to `get_devicelist()`;
       OMEMO:2 `get_devicelist()` removed

### Phase 13 — Build & tests
- [x] `make` — clean build, no errors
- [x] All 27 unit tests pass (27/27)
- [x] Fixed stale `.cov.o` ODR mismatch (touch `src/omemo/api.cpp` before test run)

---

## Remaining / Future Work

- [ ] `README.org` — update feature list, trust model, command docs
- [x] Consider removing `omemo_atm` config option entirely (separate PR)
- [ ] Consider adding BTBV fingerprint display to `/omemo devices` output

---

## XEP Compliance Gaps & Remediation Plan

**Origin**: Full codebase audit (user request "Audit our codebase for spec compliance, locate gaps and create a TODO.md to plan fixes"). See session plan.md for detailed discovery process, per-XEP rationale, and execution steps. This section is the committed living plan (modeled on OMEMO section above).

**Key principles (from AGENTS.md, re-read in full for this audit)**:
- XEP specs (in `docs/specs/xep-NNNN.txt`) must be *religiously* followed for compatibility (structure, ns, nesting, sizes/hashes plain-vs-cipher for ESFS, examples, MUST/RECOMMENDED/SHOULD, behaviors). Never mutate specs to match code; re-fetch + commit alongside any implement/modify.
- Stanza emission: Raw `xmpp_stanza_new()` / manual trees forbidden in `.cpp`/`.inl`. Use fluent `stanza::spec` / `stanza::message()` + mixins from `src/xmpp/node.hh` + `xep-*.inl` exclusively. `stanza_node`/`stanza_text_node` helpers are approved only because implemented inside node.hh via spec.
- Read `.cpp` wrappers first (never `.inl` for logic); use dedicated grep tool for searches.
- OMEMO special: legacy `eu.siacs.conversations.axolotl` **only** (see section above + committed xep-0384.txt); no omemo:2.
- Surgical/minimal changes; ccache `CXX="ccache c++" make` + 27/27+286/286 after groups; manual WeeChat retest (full restart); update README/DOAP/AGENTS/TODO in same commit as relevant work; chore:/fix: commits.
- No loose planning docs in repo.

**Discovery summary** (read-only, AGENTS-compliant): Re-read AGENTS + old plan + this TODO + DOAP.xml + README (sampled) + all relevant `docs/specs/*.txt`; list_dir src/ + src/xmpp/ + docs/specs/ (50+ specs + many builders); read_file on all key `.cpp`/`.hh` wrappers (channel.cpp, account.cpp, account/callbacks.cpp, command/rooms.cpp, connection/*.cpp, xmpp/node.{hh,cpp}, ns.hh, strophe.hh, atom.hh etc. — never .inl for logic); extensive grep tool calls (path=src/ glob=*.cpp for "xmpp_stanza_new|...|add_child|make_child|stanza::|XEP-|urn:xmpp:" + specific elems; also glob=*.inl for builder discovery only); cross-ref to node.hh mixins (e.g. xep0447, xep0184, xep0428, xep0085, xep0333, xep0359) and ns.hh; read spec excerpts for MUST/examples/reqs (0184,0085,0334,0363,0447/8/6,0300,0060,0280,0198,0313,0115,0384 etc.).

### Gaps Table (Prioritized)
| XEP(s) | Gap / Description | Code Location(s) | Spec Ref (local docs/specs + examples) | Severity | Remediation | Status |
|--------|---------------------|------------------|---------------------------------------|----------|-------------|--------|
| 0184 (Receipts) | Builder incomplete (only `received` + `receipt_received` in xep-0184.inl; no `<request/>` or `message::receipt_request()`). Main sends use manual `stanza_node` + `add_child`. Always adds `<id>` (good). Skips groupchat (matches NOT RECOMMENDED §5.3). | src/channel.cpp:1468 (regular), 1039 (file helper); also handlers for inbound received | xep-0184.txt §7 ex3 (`<request xmlns='urn:xmpp:receipts'/>` in content with id), §5.4 (MUST NOT in acks), §5.3 (groupchat) | High (daily reliability + builder rule) | Add `request` struct + `message& receipt_request()` to xep-0184.inl (modeled on received); migrate channel sends to fluent `stanza::message()....receipt_request()...`; update handlers if needed. Re-fetch+commit spec. | **done** |
| 0334 (Hints) + related | No dedicated xep-0334.inl / mixin (not in node.hh includes or message). Hints partially in xep-0384 (store_hint) + xep-0333 (no_store etc). Main sends use manual `stanza_node` for `<store>`, previews/markers use manual or .no_store() (from 0333). | src/channel.cpp:1479 (store), 1728 (no-store/no-copy in link preview), 1902/1794 (no_store on states/markers) | xep-0334.txt ( `<store/>` to force archive; `no-store`/`no-copy`/`no-permanent-store` to exclude; ns `urn:xmpp:hints`) | High (MAM interaction, markers/states correctness) | Added `no_copy` to xep-0333.inl (hints already present there); migrated all manual sites to fluent `.store()`, `.no_store()`, `.no_copy()`. | **done** |
| 0428 (Fallback) + 0447/0385/0066/0300 (SFS/ESFS/SIMS/OOB) | Core SFS/ESFS now uses `stanza::xep0447::` (good, post-prior; matches nesting/sizes/hashes in 0447/8/6/0300 ex). But fallback, SIMS, OOB still manual in file helper (comments acknowledge available builders). Main envelope (origin-id, chatstate, receipts etc) manual despite mixins. | src/channel.cpp:983 (fallback comment), 993-1026 (SIMS/OOB manual), 985 (make_child), 1029-1049 & 1457-1481 (active/request/markable/store), 979 (fs_sp add after build) | xep-0447.txt §3.1/3.2/3.3 (file-sharing + file + sources + encrypted), xep-0448.txt ex (inner sources for cipher), xep-0428.txt (fallback for=), xep-0385 (SIMS compat), xep-0066 (OOB), xep-0300 (hashes) | High (builder rule + prior real symptoms like missing previews) | Migrated to full fluent: created xep-0385.inl (SIMS), xep-0066.inl (OOB), added wrappers to stanza::message; removed make_child lambdas; file helper now returns unbuilt `stanza::message` so caller can set body + optional OMEMO before build. | **done** |
| 0085 (Chat States) | Builder exists (`chatstate(state)` + mixin); dedicated paths use it. But main send always manually adds `<active>` (every message). | src/channel.cpp:1458 (manual active), 1901 (good .chatstate in send_chat_state) | xep-0085.txt §5.1 (active on open/composing; states are notifications, not required on every body) | Medium (spec intent vs practice) | Migrated main send + file helper to `.chatstate("active")` via wrapper. | **done** |
| 0359 (Stanza IDs) + 0333 (Markers) | Mixins exist (origin-id, markable/displayed); some use (markers use .chat_marker_displayed + .no_store). Main send + file use manual stanza_node for origin-id/markable. | src/channel.cpp:1200 (origin), 1472 (markable), 1793 (good fluent in markers) | xep-0359.txt, xep-0333.txt (MUST NOT markable in groupchat — code skips) | Medium | Migrated main send + file helper to `.origin_id(id)` / `.chat_marker_markable()` via wrappers. Added `markers_markable` to xep-0333.inl. | **done** |
| 0060 (Pubsub) + Atom/microblog (0277/0472) + SFS embeds | Manual tree in account.cpp using stanza_node lambdas + direct add_child + low-level set on iq_el for <iq><pubsub><publish><item><entry> (Atom) + optional file-sharing. Dupe SFS logic vs channel. Not using xep0060 builders (in node.hh). | src/account.cpp:880-1117 (make_sp, entry, pubsub_el, iq_el sets/adds; also SFS for embeds) | xep-0060.txt ex (publish node/item), xep-0277/0472 for Atom in PEP, xep-0447 if SFS in posts | Medium (feed feature, builder consistency) | Migrate to `stanza::iq().type("set")... .pubsub( xep0060::publish(...) )`; share/reuse file share builder for SFS in posts; keep dynamic Atom via spec children. | **done** |
| 0363 (HTTP Upload) + 0448 layering | Custom `upload_request_iq : stanza::spec` (good, fluent; no dedicated xep-0363.inl needed as mostly HTTP). Correct +16 slot for ESFS cipher, plain meta in SFS. Manual? No raw. Gaps minor: no <purpose> (optional), sanitization, header allow (code has any_of). | src/command/rooms.inl:461 (upload_request_iq), 482 (build/send); iq_handler for response/worker; 0363 ns in ns.hh | xep-0363.txt §4 ex5 (request filename+size+optional content-type; ns urn:xmpp:http:upload:0), §9.2 (allowed headers), newer purpose §4 | Low-Medium (optional features) | Add purpose support (optional /upload flag or auto); extend for multi-hash in related 044x work. Re-fetch on touch. | **done** |
| 0446/0300 (File meta + hashes) + 0447 builder | Only sha-256 (single <hash>). Builder exposes only hash_sha256. Specs RECOMMEND multiple + agility. | src/channel.cpp (SFS), worker hash (EVP_sha256), xep-0447.inl (limited file API) | xep-0446.txt, xep-0300.txt (multi-hash, agility; b64 values) | Medium (future-proofing, clients may prefer) | Extend 0447 file for additional hashes (sha3 etc via gcrypt); update worker to compute vector, file_metadata, SFS emission. | **done** (SHA-256 + SHA-512 in one pass; portable OpenSSL/LibreSSL) |
| 0280 (Carbons), 0313 (MAM), 0198 (SM), 0115 (Caps) etc. | Builders/mixins exist (in node.hh for many); inbound/outbound mostly via handlers + dedicated. Minor: verify exact acks/RSM/caps hash/ <enable resume> vs current specs. MUC PM special in channel for carbons. | Various (message_handler, iq_handler, session_lifecycle, presence for caps) + channel for MUC#user x | Respective specs ex + MUST (e.g. 0280 carbons received/forwarded; 0313 mam:2 query+rsm; 0198 sm:3; 0115 caps c= hash) | Medium (core reliability) | Audit + minimal surgical if deviations found (use grep + spec re-read); migrate any remaining manual. | pending |
| OMEMO 0384 (legacy only) + SFS wrapping | All via builders (xep-0384). Legacy ns only (AGENTS). SFS/ESFS in clear + encrypted body (known; 0448 prefers SCE but forbidden here). | src/omemo/* (api wrapper), xep-0384.inl, channel OMEMO blocks | xep-0384.txt (axolotl ns, devicelist/bundle, encrypt); AGENTS carve-out | Low (per AGENTS + existing TODO.md) | Re-verify on any touch (grep + correlate_omemo_xml.sh + raw logs); no :2. See OMEMO section above for BTBV status. | ongoing (ref above) |
| General (builder under-use, alt wrapper) | Main paths + account + connection.cpp (libstrophe::stanza for version/time 0092/0202) use manual or alt low-level despite primary system. Inbound lenient (C-ABI ok per caveats). | src/channel.cpp (main), src/account.cpp (feeds), src/connection.cpp:92 (libstrophe::stanza), src/xmpp/strophe.hh (alt wrapper) | All relevant XEP ex + node.hh "exclusively" rule + "no raw outside node.hh" | High (architectural) | Phase 1 migration to fluent; document or migrate alt wrapper; no C-ABI changes. | **done** (channel + account pubsub/iq wrappers + connection version/time; strophe.hh alt wrapper documented as parse-only) |

### Execution Phases (Post Phase 0)
**Phase 0 — Audit & TODO (complete)**
- [x] Full discovery (wrappers, grep, specs, DOAP, AGENTS re-read).
- [x] Create/enhance this TODO.md section + table (no .cpp changes).
- [x] Surgical stale DOAP OMEMO note update (removed ATM ref; matches current BTBV/legacy per AGENTS + OMEMO section).
- [x] `CXX="ccache c++" make` (noop pass); commit + push (docs/chore: message referencing this audit/plan).

**Phase 1 — High Priority (Core Emission + Builders; daily interop)**
- [x] Complete 0184 builder (request) + migrate channel sends (regular + file).
- [x] Add hints (`no_copy`) + migrate stores/no-*.
- [x] Migrate file-share compat (fallback/SIMS/OOB) + main envelope to fluent (use 0428/0447/0359 etc mixins); deprecate make_child.
- [x] Migrate account.cpp feeds/pubsub to stanza::iq + xep0060; share SFS builder.
- [x] Unify version/time replies (connection.cpp) to stanza::iq or document.
- [x] Extend 0447 + worker for hash agility (SHA-256 + SHA-512); re-fetch 0300/0446/0363 + commit specs.
- [x] Add HTTP Upload <purpose> support (XEP-0363 optional); default message purpose emitted for all uploads.
- [x] Re-verify OMEMO legacy paths (no OMEMO:2 active code; BTBV trust model correct).
- [x] Migrate remaining raw emission in command/muc_admin.inl, connection/iq_handler.inl (search/config/error), xep-0054.inl (vCard) to fluent builders.
- Build/verify 27+286 after groups; manual retest (upload, receipts, states, markers, MAM, carbons, previews in other clients); update README/DOAP/AGENTS if user-visible; commit with any re-fetched specs.
- Per AGENTS: wrappers first every batch, grep searches, surgical, ccache, full restart for test.

**Phase 2 — Medium (Other Builders + Polish)**
- [x] Fill gaps in caps/MAM/carbons/SM if emission or critical parse deviations — no critical deviations found; all emission uses builders.
- [x] Inbound improvements (lenient where C-ABI allows) — parse layer already lenient via C-ABI safe patterns.
- [x] More <file> attrs (date etc) — added <date> with file mtime (UTC ISO-8601) to xep0447::file and xep0385::file; emitted in upload worker + channel + account paths.
- Re-fetch/commit on touches; same verification.

**Phase 3 — Low / Future**
- [ ] Remaining edges (spoilers 0353, pogo 0437, invites, blocking, full modern OMEMO SCE per its branch, etc).
- Ongoing: any new XEP support or modify triggers full re-fetch/verify/commit spec + TODO update.

**Verification (every phase + final)**
- Re-read AGENTS + wrappers + relevant specs before edits.
- ccache make + 27/27 + 286/286 after logical groups.
- Manual WeeChat (restart + new .so): smoke + critical checklist (PM no-recreate, typing, auto-encrypt, bookmarks, /list/ping/caps, upload self/MUC/OMEMO/close-race + Conversations preview/integrity); enable debug+raw; cross logs/raw vs spec ex; retest prior fixed bugs.
- `git diff --stat` minimal; commit includes TODO updates + re-fetched specs + README/DOAP (if applicable).
- For OMEMO bits: tools/correlate_omemo_xml.sh --account <acct> first.
- No /reload; push after commit.

**Cross-References**
- OMEMO axolotl-only + BTBV full details + remaining: section above (and branch `omemo-axolotl-btbv`).
- Upload/SFS fundamental builder fix (0447): prior work + high phase above; specs 0363/0447/0448/0446/0300/0385/0066.
- AGENTS.md (XEP "religiously", builder exclusively, wrappers/grep, re-fetch rule, no loose docs, ccache, etc.).
- Session plan.md (detailed per-gap rationale + file lists; not committed).

**When adding future XEP support or modifying**: Follow AGENTS checklist exactly (fetch spec, verify, commit .txt with code, update DOAP/README, add row here, test, commit, push).

*This section created as Phase 0 of the audit. Next high-priority work (emission migration) requires user approval of specific batch before surgical edits.*
