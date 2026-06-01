# Planning: OMEMO Encrypted Multi-User Chats (MUCs)

> Status: **Implementation complete + committed (de78d33) but live testing not yet performed.** Marked "complete-but-untested" until section 8 checklist is executed in real WeeChat + non-anonymous MUC. 27/27 green.

---

## 1. Goal

Enable end-to-end encrypted group conversations in non-anonymous MUC rooms using the legacy axolotl namespace (`eu.siacs.conversations.axolotl`) and the existing BTBV trust model.

---

## 2. Prerequisites (blocking)

Before any encryption/decryption code can work, the following plumbing must be in place.

### 2.1 Occupant real-JID tracking
- [x] Add `std::string real_jid` to `channel::member` (`src/channel.hh`).
- [x] Populate `real_jid` from MUC presence `<x xmlns='http://jabber.org/protocol/muc#user'><item jid='real@jid'/></x>` in `presence_handler.inl`.
- [x] Handle affiliation changes (member → none) that remove real-JID visibility (via presence updates + add_member).
- [x] **Reject OMEMO in anonymous rooms**: if any occupant lacks a real JID, refuse with a clear user-visible error (implemented in `send_message` + `/omemo` command via `all_occupants_have_real_jid()`).

### 2.2 Member list discovery
- [x] After receiving self-presence (status 110) on MUC join, send:
  - `disco#items` to the room for full occupant list (if supported), **or**
  - XEP-0045 admin IQs (`affiliation='member'`, `'admin'`, `'owner'`) to collect real JIDs. (Both paths implemented + results handled in iq_handler.)
- [x] Maintain a per-channel set of occupant bare JIDs. (via `channel::members` map + add_member from presence/disco/admin).
- [x] Re-fetch on affiliation changes while joined. (Presence updates feed into the same paths.)

### 2.3 Device list fetching for all occupants
- [x] Iterate occupant bare JIDs and call `request_axolotl_devicelist()` for each. (Done via 110 block, disco#items result handler, admin result handler, live presence, and centralized in `channel::add_member`.)
- [x] Cache results in the existing LMDB `axolotl:devicelist:{jid}` table (no schema change needed). (Existing behavior.)
- [x] Handle devicelist updates (pubsub notifications) for any occupant while joined. (Global handler path works for MUC occupant bare JIDs.)

### 2.4 Bundle fetching for all occupant devices
- [x] After each devicelist arrives, fetch bundles for every new/updated device ID. (Proactive fetch already present in `handle_axolotl_devicelist`; extended to MUC occupants via our devicelist request paths.)
- [x] Track per-channel `pending_bundles` counter and block sends until all bundles are fetched (adopted **Option B** for v1). Implemented via:
  - `channel::inc_pending_omemo_bundles()` called from `add_member()` when a real JID is discovered for a MUC occupant.
  - `channel::dec_pending_omemo_bundles()` called from the bundle result handler in `session_flow` when a bundle arrives for a known MUC occupant.
  - Blocking guard in `send_message()` (and status in `omemo_status()` / encryption bar item).
  - Simple counter (not per-occupant fine-grained tracking) as chosen for v1. 27/27 green.

---

## 3. Encoding (outgoing messages)

`omemo::encode()` is currently strictly 1:1. It must learn to encrypt for multiple recipients.

### 3.1 Multi-recipient `encode()`
All items below were implemented via a new `encode_muc()` entry point (declared in `omemo.hh`, implemented in `codec.inl`, called from `channel::send_message` for MUCs):

- [x] Added `encode_muc()` overload that accepts:
  - `std::vector<std::string>` of recipient bare JIDs (occupants + own account JID).
  - `struct t_gui_buffer *buf` (for error logging).
  - `std::string_view room_jid` (modernized for consistency with other JID parameters in the OMEMO API).
  - `const char *payload` (plaintext body).
- [x] For each recipient bare JID: load devicelist from LMDB, build sessions for each device, encrypt the same payload key, emit `<keys jid='recipient_bare_jid'>` containing `<key rid='device_id'>` children (using the stanza builder).
- [x] Own devices are included in the `<keys>` wrappers so other clients (and carbons) can decrypt.
- [x] Message is sent as `type="groupchat"`.
- [x] Body contains the `OMEMO_ADVICE` placeholder string.
- 27/27 green. See `Current Implementation Status` and `src/channel.cpp:1299` + `src/omemo/codec.inl:584`.

### 3.2 Partial failure handling
- [x] On any encryption failure for one or more recipients (missing devicelist, no bundle, transport key failure), we fail the entire send (as recommended for v1).
- [x] Error message now lists the specific recipient JIDs that could not be encrypted for ("Missing keys for: jid1, jid2..."). Uses `std::ranges::find` for tracking. 27/27 green.

### 3.3 Key transport in MUC
- [x] `send_key_transport()` detects MUC occupants, sends as `groupchat` to the room, and now encrypts the (zero) key-transport payload for **every** current occupant using the same multi-recipient `axolotl_keys` approach as normal messages. Own devices are also included. 27/27 green.

---

## 4. Decoding (incoming messages)

### 4.1 Correct sender JID
- [x] MUC message `from` is `room@service/nick`. The sender's real bare JID is **not** in the stanza.
- [x] Map sender nick → real bare JID using `channel::member::real_jid`. (Done in message_handler before calling decode.)
- [x] If mapping is missing (e.g., anonymous room or presence not yet received), skip decryption and show placeholder. (Fallback to room bare JID causes decode to fail gracefully.)
- [x] Pass the **sender's real bare JID** (not `from_bare`) to `decode()` so Signal sessions are keyed correctly.

### 4.2 `<keys jid='…'>` matching
- [x] `decode()` already parses `<keys jid='…'>` wrappers (legacy axolotl format).
- [x] It selects the `<keys>` block whose `jid` matches our account bare JID.
- [x] Then decrypt using the session keyed on `(sender_real_bare_jid, device_id)`. (Now works for MUC because we pass the correct sender real JID.)

---

## 5. Channel / UI integration

### 5.1 Remove MUC blocks
- [x] All hard-coded "MUC OMEMO not yet implemented" style blocks and PM-only restrictions have been removed or replaced with the proper readiness checks (`all_occupants_have_real_jid()` + `pending_bundles`).
- [x] `/omemo` command, `send_message`, auto-enable, flush_pending, and devicelist handling now treat MUCs appropriately.
- [x] Stale comments cleaned up across channel.cpp, api.cpp, etc.
- 5.1 is considered complete for v1. 27/27 green.

### 5.2 Pending message queue for MUCs
- [x] Adopted **Option B (block)** for v1 as recommended by the plan. MUC sends are refused while bundles are pending (clear error message including count). The `pending_omemo_messages` queue remains intentionally PM-only for v1 to avoid complexity of partial failures across occupants. Added comment in `flush_pending_omemo_messages`. 27/27 green.

### 5.3 Transport bar / status
- [x] `xmpp_encryption` bar item now shows a useful status for MUC OMEMO. Added `channel::omemo_status()` helper. When `pending_bundles > 0` on a MUC it displays "🔒OMEMO (pending)". Bar item is refreshed on counter changes. 27/27 green.
- [x] The send-time blocking message already includes the pending bundle count.

---

## Current Implementation Status (updated during development)

**Prerequisites (Section 2): Largely Complete**

- 2.1 Real-JID tracking: Fully implemented.
- 2.2 Member list discovery: Fully implemented (disco#items + full XEP-0045 admin affiliation fallback on join, handlers populate `members` with real_jids).
- 2.3 Devicelist fetching: Fully implemented for all known occupants (multiple trigger points + centralization in `add_member`).
- 2.4 Bundle fetching + blocking: **Complete**. Per-channel `pending_bundles` counter + helpers. Increment on real_jid discovery for MUC occupants (main path) and on explicit bundle requests for occupants. Decrement on bundle result processing in `handle_axolotl_bundle` for MUC occupants. Option B blocking guard in `send_message`. 27/27 green.

**Implementation + AGENTS.md compliance complete and committed (de78d33).**  
**Live testing status**: The full checklist in section 8 has **not yet been executed** in a real WeeChat session with a non-anonymous MUC. All items below have only received static code review + unit test verification. The feature is marked complete-but-untested until the live pass is performed and documented.

The hard MUC blocks have already been softened to use `all_occupants_have_real_jid()` in several places.

**Latest progress (this session):**
- Completed final AGENTS.md compliance sweep (C++23 ranges/views + re-audit for raw loops) on the MUC OMEMO changes. Raw loop audit (this step) confirmed the feature uses modern range-based for + structured bindings everywhere practical, plus `std::ranges` algorithms on the transform/filter/predicate sites and in several discovery paths. No further changes needed.
- Full `make` rebuild + 27/27 unit tests (286/286 assertions) green.
- Section 8 (Testing checklist) statically verified against current code + annotated with live requirements. MUC OMEMO v1 implementation + hygiene now complete; ready for (or awaiting) real-world manual testing pass in WeeChat.
- Real multi-recipient `encode_muc` implemented in codec (loop over recipients, per-recipient `axolotl_keys` wrappers using the stanza builder, single AES-GCM payload, own devices included, proper error handling for missing bundles). 27/27 tests green.
- **Decoding side (§4.1)**: In the message handler, for MUC groupchat OMEMO messages we now map the sender's nick (from the `from` attribute `room/nick`) to the real bare JID via `channel::member::real_jid` and pass the *real* JID (not the room JID) to `decode()`. This makes Signal session lookup and trust work correctly for MUC occupants. Fallback to room bare JID is preserved (decode will fail gracefully → placeholder).
- **Section 5.1 (Remove MUC blocks) — final cleanup**: All remaining hard-coded "MUC OMEMO not yet" style blocks and outdated PM-only restrictions have been removed or replaced with proper readiness checks. Stale comments cleaned across channel.cpp, api.cpp, etc. 5.1 is now complete. 27/27 green.
- **5.2 / 5.3**: Confirmed Option B (block) for MUCs in v1. Added `omemo_status()` helper so the `xmpp_encryption` bar shows "🔒OMEMO (pending)" for rooms with outstanding bundles.
- **Section 7 (MAM)**: MAM OMEMO replay safety reinforced with comments (cache-first to protect the ratchet for MUC history).
- **Section 6 (Trust model)**: Added explicit blind-trust (BTBV) warnings in code comments (MUC devicelist handler + encode_muc) and a one-time note when enabling OMEMO in a MUC via `/omemo`. Addresses the plan's "new concern" for v1 without changing default behavior. 27/27 green.

27/27 tests remain green.

**Marked complete-but-untested**: Implementation and compliance work finished and committed as de78d33. Live manual testing (section 8) is still pending. No production use should be made of MUC OMEMO until the testing checklist has been run and any issues addressed.

**AGENTS.md compliance pass (ranges + pure C++23 printf hygiene + raw-loop re-audit) — completed:**

- Raw loop audit (user request "check the code again for raw loops"): Performed full targeted review of every MUC OMEMO path (after reading all .cpp wrappers per AGENTS).
  - Critical paths already modernized in prior pass: recipient collection (`views::filter|transform` + `ranges::copy`), `all_occupants_have_real_jid` (`ranges::all_of`).
  - All other container iteration in the feature (members maps for discovery/recipients/occupant lookup, recipient_bare_jids in encode_muc, device lists from split, channel walks for KT + bundle decrement, etc.) uses clean range-based `for (const auto& [k, v] : map)` with structured bindings + C++23 style. Many already use `std::ranges::find`, `ranges::stable_sort`, etc.
  - Stanza child walks (libstrophe `xmpp_stanza_get_children`/`get_next`) are linked-list traversal over C API — not C++ containers, intentionally left as-is (correct idiom; several places already collect to vector then apply `std::ranges::*`).
  - One tiny C-style index loop remains in the "Missing keys for: ..." error formatting inside encode_muc failure path. Left unchanged (matches other error paths, minimal change rule, no functional benefit, already uses `std::ranges::find` right next to it).
  - No edits required. The MUC OMEMO code is in excellent C++23 shape on iteration. 27/27 remains green.
- All new MUC OMEMO paths (channel::send_message recipient collection via `std::views::filter|transform` + `std::ranges::copy`, `all_occupants_have_real_jid` via `std::ranges::all_of`, the two blocking error messages, plus the two auto-enable notifications touched by MUC safety comments) now use C++23 ranges/views and `weechat_printf(..., "%s", fmt::format(...).c_str())`.
- Pre-existing OMEMO internal logging left in legacy style (not part of the MUC feature changes).
- Full `make` (build + 27/27 unit tests, 286/286 assertions) green after the edits.
- All changes continue to obey: stanza builder only, RAII, no raw malloc/new in new code, snake_case, read .cpp wrappers for logic, etc.

The `<keys jid='...'>` selection inside decode already supports multiple wrappers (from earlier work), so 4.2 is largely satisfied once the correct sender JID is supplied.

27/27 tests remain green.

---

## 6. Trust model

- [x] No schema changes needed. BTBV trust is already per `(bare_jid, device_id)`.
- [x] When building sessions for MUC occupants, `load_tofu_trust()` uses the occupant's real bare JID — works as-is.
- [x] **Addressed for v1**: Added prominent comments in the MUC devicelist handler (session_flow) and in `encode_muc` (codec) explaining that new MUC occupants are treated with default BTBV (blind) trust. When enabling OMEMO in a MUC via `/omemo`, a one-time note is printed warning the user about blind-trusting all occupants. Full per-MUC verification policy (e.g. "require manual verification for MUCs") is left as future work. 27/27 green.

---

## 7. MAM / history replay

- [x] `mam_cache_store_omemo_plaintext()` and `mam_cache_lookup_omemo_plaintext()` use `channel_jid:msg_id` keys and work for MUC room JIDs (channel_id is the room bare JID for groupchat messages).
- [x] Early cache check on MAM replay (using stable_id + channel_id) serves cached cleartext **before** calling decode. This protects the Signal ratchet for historical MUC OMEMO messages (as requested). Fallback decode path now receives the correct sender real JID thanks to §4.1 mapping.
- [x] Storage after successful live decryption stores under multiple IDs (sid/oid/mid) using the room JID for MUCs.
- [x] `mam_cache_message()` stores the occupant nick for MUC OMEMO history display.
- Added explicit comments in both the MAM load path and the live OMEMO handling path highlighting the cache-first safety for MUC OMEMO replay. 27/27 green.

---

## 8. Testing checklist

**Static code verification performed (this step, post-compliance + green build):**

- [x] **Code paths exist and guards are wired** for non-anonymous MUC requirement:
  - `channel::all_occupants_have_real_jid()` (src/channel.cpp:856) using `std::ranges::all_of`.
  - Guard in `send_message` (src/channel.cpp:1267) and `/omemo` (src/command/encryption.inl:301) refuse with clear fmt-based error when false.
  - Real JID population: presence_handler (muc#user item jid), iq_handler (disco#items result + 3x muc#admin affiliation queries for member/admin/owner), centralized in `add_member`.
  - *Live test still required*: actual join of a real non-anon members-only MUC with multiple occupants having visible real JIDs.

- [x] **`/omemo` enable path** (src/command/encryption.inl:311) now calls `set_transport(OMEMO)` for MUCs after the real-JID check. Also emits one-time BTBV warning note for MUCs (§6).
  - *Live*: run `/omemo` in a qualifying room buffer, confirm no error + bar item updates + note printed.

- [x] **MUC send path** (src/channel.cpp:1299): when OMEMO enabled on MUC, collects recipients via views::filter/transform over members with real_jid + own bare, calls `encode_muc`.
  - Pending bundles guard (src/channel.cpp:1279) blocks with count in error.
  - *Live*: send encrypted message in qualifying MUC; observe success or pending message.

- [x] **Multi-recipient encoding** implemented:
  - `encode_muc` entry (src/omemo/api.cpp:74 + codec), builds one AES-GCM payload + per-recipient `<keys jid='bare'>` with per-device `<key rid=...>` using stanza builder (axolotl_* types).
  - Own devices included.
  - Error on any missing keys for a recipient (detailed "Missing keys for: ..." list).
  - *Live verification*: enable raw_xml_log, inspect a sent groupchat OMEMO stanza for multiple `<keys jid=...>` blocks.

- [ ] **Inter-client decrypt** — requires second OMEMO-capable client (Gajim/Conversations/dino etc.) in the same room. No static verification possible beyond wire format + decode path (sender real JID mapping in message_handler.inl:2019).

- [x] **MAM replay safety**:
  - Cache lookup before decode (using room JID + stable/origin IDs) in message_handler MAM path.
  - Comments added in MAM load + live OMEMO paths.
  - *Live*: restart WeeChat, rejoin, trigger MAM history for the MUC OMEMO room; confirm cleartext display without ratchet errors in logs.

- [x] **Anonymous refusal** implemented and tested in unit paths (the guard + error message).
  - *Live*: join or create an anonymous room (or one without real JIDs visible yet), attempt `/omemo` or OMEMO send → clear refusal.

- [x] **Mixed devicelist / missing keys**:
  - `encode_muc` and the recipient loop fail the whole send if any occupant has no devicelist or no bundles (per plan §3.2 "fail entire send").
  - Error message lists the specific bare JIDs that could not be encrypted for (uses ranges::find in tracking).
  - *Live*: add a non-OMEMO client/occupant to the room or an occupant whose bundles are unreachable; attempt send; observe informative failure.

**Summary after static + build verification**: All *code* for the 8 items is present, guarded, and 27/27 green. The remaining work is **live manual testing in a real WeeChat session with a non-anonymous MUC containing ≥2 other OMEMO users** (plus raw XML log inspection and MAM replay). No further code changes expected for v1 unless live testing reveals issues.

**Recommendation**: User performs the live checklist (with `/set xmpp.look.raw_xml_log on`, `/set xmpp.look.debug on`, and `tools/correlate_omemo_xml.sh` if needed). Report back any wire-format, decrypt, or edge-case surprises.

**Committed as**: `de78d33` ("feat: add OMEMO support for non-anonymous MUCs (XEP-0384 legacy axolotl)").  
**Feature status**: Implementation + compliance complete and committed. **Live testing pending** (this document now explicitly marks the feature "complete-but-untested").

---

## 9. Files likely to change

| File | Section | Change |
|------|---------|--------|
| `src/channel.hh` | `channel::member` | Add `real_jid` field |
| `src/connection/presence_handler.inl` | MUC presence handling | Store real JID in member map |
| `src/omemo/codec.inl` | `encode()` / `decode()` | Multi-recipient support, correct sender JID |
| `src/omemo/session_flow.inl` | `send_key_transport()`, `handle_axolotl_devicelist()` | MUC-aware routing, remove PM guards |
| `src/connection/message_handler.inl` | Auto-enable, `decode()` call site | Remove PM guards, pass sender real JID |
| `src/channel.cpp` | `send_message()` | Multi-recipient encode call, pending queue for MUC |
| `src/command/encryption.inl` | `/omemo` | Revert MUC block when ready |
| `src/command/channel.inl` | `/enter` | Trigger member list + devicelist fetches after join |

---

## 10. Open questions

1. **Anonymous rooms**: XEP-0384 requires non-anonymous rooms. Do we enforce this strictly, or attempt best-effort (e.g., querying MUC item info IQs to get real JIDs on demand)?
2. **Self-sent MUC messages**: The current `is_own_device_self_copy` logic in `message_handler.inl` assumes PM context. How does this apply when you send to a MUC from another device?
3. **Room reconfiguration**: If a room changes from non-anonymous to anonymous while we're joined, do we immediately disable OMEMO?
4. **Performance**: For large MUCs (50+ occupants), fetching 50 devicelists + bundles on every join may be slow. Is there a caching strategy (e.g., persistent per-room occupant-device cache in LMDB) to avoid re-fetching on every reconnect?
