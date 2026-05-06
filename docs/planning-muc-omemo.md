# Planning: OMEMO Encrypted Multi-User Chats (MUCs)

> Status: **Not implemented** — blocked at the UI and send layers until this checklist is completed.

---

## 1. Goal

Enable end-to-end encrypted group conversations in non-anonymous MUC rooms using the legacy axolotl namespace (`eu.siacs.conversations.axolotl`) and the existing BTBV trust model.

---

## 2. Prerequisites (blocking)

Before any encryption/decryption code can work, the following plumbing must be in place.

### 2.1 Occupant real-JID tracking
- [ ] Add `std::string real_jid` to `channel::member` (`src/channel.hh`).
- [ ] Populate `real_jid` from MUC presence `<x xmlns='http://jabber.org/protocol/muc#user'><item jid='real@jid'/></x>` in `presence_handler.inl`.
- [ ] Handle affiliation changes (member → none) that remove real-JID visibility.
- [ ] **Reject OMEMO in anonymous rooms**: if any occupant lacks a real JID, refuse with a clear user-visible error.

### 2.2 Member list discovery
- [ ] After receiving self-presence (status 110) on MUC join, send:
  - `disco#items` to the room for full occupant list (if supported), **or**
  - XEP-0045 admin IQs (`affiliation='member'`, `'admin'`, `'owner'`) to collect real JIDs.
- [ ] Maintain a per-channel set of occupant bare JIDs.
- [ ] Re-fetch on affiliation changes while joined.

### 2.3 Device list fetching for all occupants
- [ ] Iterate occupant bare JIDs and call `request_axolotl_devicelist()` for each.
- [ ] Cache results in the existing LMDB `axolotl:devicelist:{jid}` table (no schema change needed).
- [ ] Handle devicelist updates (pubsub notifications) for any occupant while joined.

### 2.4 Bundle fetching for all occupant devices
- [ ] After each devicelist arrives, fetch bundles for every new/updated device ID.
- [ ] Track per-channel `pending_bundle_count`; block sends until all bundles are fetched.

---

## 3. Encoding (outgoing messages)

`omemo::encode()` is currently strictly 1:1. It must learn to encrypt for multiple recipients.

### 3.1 Multi-recipient `encode()`
- [ ] Change `encode()` signature (or add overload `encode_muc()`) to accept:
  - `std::vector<std::string>` of recipient bare JIDs (all occupants + own account JID).
  - `struct t_gui_buffer *buf` (for error logging).
  - `const char *room_jid` (for the `<keys jid='…'>` wrapper).
  - `const char *payload` (plaintext body).
- [ ] For each recipient bare JID:
  - Load their devicelist from LMDB.
  - For each device ID, build/fetch a Signal session.
  - Encrypt the same payload key for every device.
  - Emit `<keys jid='recipient_bare_jid'>` containing all `<key rid='device_id'>` children.
- [ ] Include own devices in the `<keys>` wrappers so other clients can decrypt.
- [ ] Message type must be `groupchat`.
- [ ] Body must be `OMEMO_ADVICE`.

### 3.2 Partial failure handling
- [ ] Decide: if one occupant has no devicelist/bundle, fail the entire send or skip that occupant?
  - **Recommended for v1**: fail the entire send with a clear error listing which JIDs are missing keys.
  - This avoids sending an undecryptable message to some members.

### 3.3 Key transport in MUC
- [ ] `send_key_transport()` currently hardcodes `type="chat"` and `.to(peer_jid)`.
- [ ] Add MUC variant: `type="groupchat"`, destination is the room bare JID.
- [ ] Encrypt the key-transport payload for every occupant device (same multi-recipient logic as regular messages).

---

## 4. Decoding (incoming messages)

### 4.1 Correct sender JID
- [ ] MUC message `from` is `room@service/nick`. The sender's real bare JID is **not** in the stanza.
- [ ] Map sender nick → real bare JID using `channel::member::real_jid`.
- [ ] If mapping is missing (e.g., anonymous room or presence not yet received), skip decryption and show placeholder.
- [ ] Pass the **sender's real bare JID** (not `from_bare`) to `decode()` so Signal sessions are keyed correctly.

### 4.2 `<keys jid='…'>` matching
- [ ] `decode()` already parses `<keys jid='…'>` wrappers (legacy axolotl format).
- [ ] Ensure it selects the `<keys>` block whose `jid` matches our account bare JID.
- [ ] Then decrypt using the session keyed on `(sender_real_bare_jid, device_id)`.

---

## 5. Channel / UI integration

### 5.1 Remove MUC blocks
- [ ] Revert the `/omemo` MUC guard in `src/command/encryption.inl`.
- [ ] Revert the `send_message` MUC guard in `src/channel.cpp`.
- [ ] Remove `chat_type::PM` guard from auto-enable logic in `message_handler.inl`.
- [ ] Remove `chat_type::PM` guard from `flush_pending_omemo_messages()` in `channel.cpp`.
- [ ] Remove `chat_type::PM` guard from `handle_axolotl_devicelist()` in `omemo/session_flow.inl`.

### 5.2 Pending message queue for MUCs
- [ ] Decide strategy:
  - **Option A (queue)**: queue MUC messages while occupant device lists/bundles are still arriving; flush when all are ready. Complex because of partial failures across many JIDs.
  - **Option B (block)**: refuse the send with "Waiting for occupant keys…" until all bundles are fetched. Simpler and safer.
  - **Recommended for v1**: Option B.

### 5.3 Transport bar / status
- [ ] `xmpp_encryption` bar item already checks `channel->omemo.enabled` generically — no change needed.
- [ ] Consider adding a status line showing how many occupant keys are still pending.

---

## 6. Trust model

- [ ] No schema changes needed. BTBV trust is already per `(bare_jid, device_id)`.
- [ ] When building sessions for MUC occupants, `load_tofu_trust()` uses the occupant's real bare JID — works as-is.
- [ ] **New concern**: blind-trusting every MUC occupant on first use may be surprising. Consider whether users should be able to require manual verification for MUCs, or whether BTBV is acceptable for group chats.

---

## 7. MAM / history replay

- [ ] `mam_cache_store_omemo_plaintext()` and `mam_cache_lookup_omemo_plaintext()` already use `channel_jid:msg_id` keys — compatible with MUC room JIDs.
- [ ] **Live decryption** must succeed for caching to work (depends on §4 above).
- [ ] `mam_cache_message()` in `message_handler.inl` already stores MUC messages with the nick as `cache_from` — no change needed.

---

## 8. Testing checklist

- [ ] Join a non-anonymous members-only MUC.
- [ ] Run `/omemo` — should enable without error.
- [ ] Send a message — should encrypt for all occupants and own devices.
- [ ] Verify `<keys jid='…'>` wrappers in the sent stanza (raw XML log).
- [ ] Another client in the room should decrypt and display the message.
- [ ] Restart WeeChat, rejoin room, trigger MAM replay — messages should display in cleartext.
- [ ] Anonymous room: `/omemo` should be refused with a clear error.
- [ ] Mixed-devicelist room: occupant with no OMEMO support should cause the send to fail with an informative error.

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
