        else if (line.rfind("spks=", 0) == 0)
            bundle.signed_pre_key_signature = line.substr(5);
        else if (line.rfind("ik=", 0) == 0)
            bundle.identity_key = line.substr(3);
        else if (line.rfind("pk=", 0) == 0)
        {
            const auto payload = line.substr(3);
            const auto separator = payload.find(',');
            if (separator != std::string::npos)
            {
                bundle.prekeys.emplace_back(payload.substr(0, separator),
                                            payload.substr(separator + 1));
            }
        }
    }

    return bundle;
}

void ensure_db_open(omemo &self)
{
    if (self.db_path.empty())
        throw std::runtime_error {"OMEMO database path is empty"};

    std::filesystem::create_directories(std::filesystem::path {self.db_path}.parent_path());
    if (!self.db_env)
    {
        self.db_env = lmdb::env::create();
        self.db_env.set_max_dbs(4);
        self.db_env.set_mapsize(32U * 1024U * 1024U);
        self.db_env.open(self.db_path.c_str(), MDB_NOSUBDIR | MDB_CREATE, 0664);
    }

    auto transaction = lmdb::txn::begin(self.db_env);
    self.dbi.omemo = lmdb::dbi::open(transaction, "omemo", MDB_CREATE);
    transaction.commit();
}

void store_string(omemo &self, std::string_view key, std::string_view value)
{
    auto transaction = lmdb::txn::begin(self.db_env);
    self.dbi.omemo.put(transaction, key, value);
    transaction.commit();
}

[[nodiscard]] auto load_string(omemo &self, std::string_view key) -> std::optional<std::string>
{
    auto transaction = lmdb::txn::begin(self.db_env, nullptr, MDB_RDONLY);
    std::string_view value;
    if (!self.dbi.omemo.get(transaction, key, value))
        return std::nullopt;
    return std::string {value};
}

[[nodiscard]] auto load_bundle(omemo &self, std::string_view jid,
                               std::uint32_t remote_device_id) -> std::optional<bundle_metadata>
{
    const auto serialized = load_string(self, key_for_bundle(jid, remote_device_id));
    if (!serialized)
        return std::nullopt;
    return deserialize_bundle(*serialized);
}

void store_bundle(omemo &self, std::string_view jid, std::uint32_t remote_device_id,
                  const bundle_metadata &bundle)
{
    store_string(self, key_for_bundle(jid, remote_device_id), serialize_bundle(bundle));
}

[[nodiscard]] auto make_local_bundle_metadata(omemo &self) -> std::optional<bundle_metadata>
{
    OMEMO_ASSERT(self.context, "signal context must exist before exporting the local bundle");

    if (!self.identity)
        return std::nullopt;

    const auto signed_pre_key_id = load_string(self, kSignedPreKeyId);
    const auto signed_pre_key_public = load_bytes(self, kSignedPreKeyPublic);
    const auto signed_pre_key_signature = load_bytes(self, kSignedPreKeySignature);
    const auto identity_public = load_bytes(self, kIdentityPublicKey);
    const auto prekeys_serialized = load_string(self, kPrekeys);

    if (!signed_pre_key_id || !signed_pre_key_public
        || !signed_pre_key_signature || !identity_public || !prekeys_serialized)
    {
        return std::nullopt;
    }

    bundle_metadata bundle;
    bundle.signed_pre_key_id = *signed_pre_key_id;
    bundle.signed_pre_key = base64_encode_raw(signed_pre_key_public->data(),
                                              signed_pre_key_public->size());
    bundle.signed_pre_key_signature = base64_encode_raw(signed_pre_key_signature->data(),
                                                        signed_pre_key_signature->size());
    bundle.identity_key = base64_encode_raw(identity_public->data(),
                                            identity_public->size());

    if (bundle.signed_pre_key_id.empty() || !parse_uint32(bundle.signed_pre_key_id))
    {
        weechat_printf(nullptr,
                       "%somemo: local bundle metadata has invalid signed prekey id '%s'",
                       weechat_prefix("error"),
                       bundle.signed_pre_key_id.c_str());
        return std::nullopt;
    }
    if (bundle.signed_pre_key.empty() || bundle.signed_pre_key_signature.empty()
        || bundle.identity_key.empty())
    {
        weechat_printf(nullptr,
                       "%somemo: local bundle metadata is missing spk/spks/ik payloads",
                       weechat_prefix("error"));
        return std::nullopt;
    }

    for (const auto &entry : split(*prekeys_serialized, ';'))
    {
        const auto separator = entry.find(',');
        if (separator == std::string::npos)
            continue;

        const auto id = entry.substr(0, separator);
        const auto key = entry.substr(separator + 1);
        const auto parsed_id = parse_uint32(id);
        if (!parsed_id || key.empty())
        {
            weechat_printf(nullptr,
                           "%somemo: local bundle metadata contains invalid prekey entry '%s'",
                           weechat_prefix("error"),
                           entry.c_str());
            return std::nullopt;
        }

        const bool duplicate_id = std::ranges::any_of(
            bundle.prekeys,
            [&](const auto &prekey) {
                return prekey.first == id;
            });
        if (duplicate_id)
        {
            weechat_printf(nullptr,
                           "%somemo: local bundle metadata contains duplicate prekey id '%s'",
                           weechat_prefix("error"),
                           id.c_str());
            return std::nullopt;
        }

        bundle.prekeys.emplace_back(id, key);
    }

    if (bundle.prekeys.empty())
        return std::nullopt;

    if (bundle.prekeys.size() < kMinPreKeyCount)
    {
        weechat_printf(nullptr,
                       "%somemo: local bundle metadata has only %zu prekeys (XEP-0384 minimum is %u)",
                       weechat_prefix("error"),
                       bundle.prekeys.size(),
                       kMinPreKeyCount);
    }

    store_bundle(self, "self", self.device_id, bundle);
    return bundle;
}

void ensure_local_identity(omemo &self)
{
    if (!self.context)
        return;

    OMEMO_ASSERT(self.db_env, "LMDB environment must exist before generating local identity");

    if (self.identity)
        return;

    const auto public_data = load_bytes(self, kIdentityPublicKey);
    const auto private_data = load_bytes(self, kIdentityPrivateKey);
    if (public_data && private_data && !public_data->empty() && !private_data->empty())
    {
        // Load via raw decode + explicit SIGNAL_UNREF, matching the legacy-safe
        // ownership model: ratchet_identity_key_pair_create takes refs to keys.
        ec_public_key *public_key_raw = nullptr;
        ec_private_key *private_key_raw = nullptr;
        if (curve_decode_point(&public_key_raw,
                               public_data->data(), public_data->size(),
                               self.context) == 0
            && curve_decode_private_point(&private_key_raw,
                                          private_data->data(), private_data->size(),
                                          self.context) == 0
            && public_key_raw && private_key_raw)
        {
            self.identity = libsignal::identity_key_pair(public_key_raw, private_key_raw);
            SIGNAL_UNREF(public_key_raw);
            SIGNAL_UNREF(private_key_raw);
            return;
        }

        if (public_key_raw)
            SIGNAL_UNREF(public_key_raw);
        if (private_key_raw)
            SIGNAL_UNREF(private_key_raw);
    }

    self.identity = libsignal::identity_key_pair::generate(self.context);

    signal_buffer *serialized_public_raw = nullptr;
    signal_buffer *serialized_private_raw = nullptr;

    // ratchet_identity_key_pair_get_public/private return non-owning borrowed
    // pointers — do NOT wrap in RAII (would SIGNAL_UNREF the key pair's
    // internal EC keys, corrupting the identity key pair).
    ec_public_key *public_key_raw = ratchet_identity_key_pair_get_public(self.identity);
    ec_private_key *private_key_raw = ratchet_identity_key_pair_get_private(self.identity);
    if (!public_key_raw || !private_key_raw)
        return;
    if (ec_public_key_serialize(&serialized_public_raw, public_key_raw) != 0)
        return;
    if (ec_private_key_serialize(&serialized_private_raw, private_key_raw) != 0)
        return;

    unique_signal_buffer serialized_public {serialized_public_raw};
    unique_signal_buffer serialized_private {serialized_private_raw};

    store_bytes(self,
                kIdentityPublicKey,
                signal_buffer_const_data(serialized_public.get()),
                signal_buffer_len(serialized_public.get()));
    store_bytes(self,
                kIdentityPrivateKey,
                signal_buffer_const_data(serialized_private.get()),
                signal_buffer_len(serialized_private.get()));
}

void ensure_registration_id(omemo &self)
{
    OMEMO_ASSERT(self.context, "signal context must exist before generating registration id");

    if (load_string(self, kRegistrationIdKey))
        return;

    std::uint32_t registration_id = 0;
    if (signal_protocol_key_helper_generate_registration_id(&registration_id, 0, self.context) == 0)
        store_string(self, kRegistrationIdKey, fmt::format("{}", registration_id));
}

// Rebuild the kPrekeys index from the actual prekey:N records stored in LMDB.
// Called when the index exists but is known to be shorter than the actual records,
// e.g. because a previous write partially failed or the index was never updated for
// IDs outside the originally-generated range.
// Returns the number of entries successfully rebuilt.
static std::size_t repair_prekeys_index(omemo &self, xmpp_ctx_t *context)
{
    OMEMO_ASSERT(self.context, "signal context required for prekey index repair");
    OMEMO_ASSERT(context != nullptr, "xmpp context required for prekey index repair");

    // Scan all prekey:N LMDB records to discover which IDs actually exist.
    // We cannot scan by range directly; instead we probe each ID in the
    // originally-generated range [kPreKeyStart, kPreKeyStart + kPreKeyCount - 1].
    // Any record that exists and can be deserialized contributes an index entry.
    std::vector<std::string> index_entries;
    index_entries.reserve(kPreKeyCount);

    for (std::uint32_t id = kPreKeyStart; id < kPreKeyStart + kPreKeyCount; ++id)
    {
        const auto record_bytes = load_bytes(self, key_for_prekey_record(id));
        if (!record_bytes || record_bytes->empty())
            continue;

        session_pre_key *pre_key_raw = nullptr;
        if (session_pre_key_deserialize(&pre_key_raw,
                                        record_bytes->data(), record_bytes->size(),
                                        self.context) != 0)
            continue;
        libsignal::object<session_pre_key> pre_key {pre_key_raw};

        ec_key_pair *pair = session_pre_key_get_key_pair(pre_key.get());
        if (!pair)
            continue;
        ec_public_key *pub = ec_key_pair_get_public(pair);
        if (!pub)
            continue;

        const auto pub_b64 = serialize_public_key(context, pub);
        if (pub_b64.empty())
            continue;

        index_entries.push_back(fmt::format("{},{}", session_pre_key_get_id(pre_key.get()), pub_b64));
    }

    if (index_entries.empty())
        return 0;

    store_string(self, kPrekeys, join(index_entries, ";"));
    weechat_printf(nullptr,
                   "%somemo: repaired prekeys index — rebuilt %zu entries",
                   weechat_prefix("network"),
                   index_entries.size());
    return index_entries.size();
}

// Generate a new signed prekey with the given ID, persist it to LMDB, and
// store the current timestamp.  Returns true on success.
[[nodiscard]] static bool generate_and_store_spk(omemo &self, std::uint32_t new_spk_id)
{
    session_signed_pre_key *signed_pre_key_raw = nullptr;
    const auto now_secs = static_cast<std::int64_t>(std::time(nullptr));
    const auto now_ms   = static_cast<std::uint64_t>(now_secs) * 1000ULL;
    if (signal_protocol_key_helper_generate_signed_pre_key(
            &signed_pre_key_raw, self.identity, new_spk_id, now_ms, self.context) != 0)
    {
        return false;
    }

    libsignal::object<session_signed_pre_key> signed_pre_key {signed_pre_key_raw};
    signal_buffer *signed_pre_key_record_raw = nullptr;
    if (session_signed_pre_key_serialize(&signed_pre_key_record_raw, signed_pre_key.get()) != 0)
        return false;

    unique_signal_buffer signed_pre_key_record {signed_pre_key_record_raw};
    ec_key_pair *signed_pre_key_pair = session_signed_pre_key_get_key_pair(signed_pre_key.get());
    ec_public_key *signed_pre_key_public = ec_key_pair_get_public(signed_pre_key_pair);

    signal_buffer *signed_pre_key_public_raw = nullptr;
    if (ec_public_key_serialize(&signed_pre_key_public_raw, signed_pre_key_public) != 0)
        return false;
    unique_signal_buffer signed_pre_key_public_buffer {signed_pre_key_public_raw};

    store_string(self, kSignedPreKeyId, fmt::format("{}", new_spk_id));
    store_bytes(self,
                kSignedPreKeyRecord,
                signal_buffer_const_data(signed_pre_key_record.get()),
                signal_buffer_len(signed_pre_key_record.get()));
    store_bytes(self,
                key_for_signed_prekey_record(new_spk_id),
                signal_buffer_const_data(signed_pre_key_record.get()),
                signal_buffer_len(signed_pre_key_record.get()));
    store_bytes(self,
                kSignedPreKeyPublic,
                signal_buffer_const_data(signed_pre_key_public_buffer.get()),
                signal_buffer_len(signed_pre_key_public_buffer.get()));
    store_bytes(self,
                kSignedPreKeySignature,
                session_signed_pre_key_get_signature(signed_pre_key.get()),
                session_signed_pre_key_get_signature_len(signed_pre_key.get()));
    // Record the generation timestamp so rotation logic knows when to rotate next.
    store_string(self, kSignedPreKeyTimestamp, fmt::format("{}", now_secs));

    weechat_printf(nullptr,
                   "%somemo: signed prekey rotated to id %u",
                   weechat_prefix("network"),
                   new_spk_id);
    return true;
}

// Check whether the current signed prekey is old enough to rotate.  If it is
// (or if no timestamp is stored, meaning it was created before rotation tracking
// was added), generate a new signed prekey with an incremented ID and republish.
// Returns true if a rotation was performed.
[[nodiscard]] static bool rotate_signed_prekey_if_due(omemo &self)
{
    const auto ts_str = load_string(self, kSignedPreKeyTimestamp);
    const auto now_secs = static_cast<std::int64_t>(std::time(nullptr));
    const bool should_rotate = !ts_str
        || (now_secs - parse_int64(*ts_str).value_or(0)) >= kSignedPreKeyRotationSecs;

    if (!should_rotate)
        return false;

    // Determine the next SPK ID (wraps around at 2^31-1; must stay ≥ 1).
    const auto current_id_str = load_string(self, kSignedPreKeyId);
    const auto current_id = current_id_str
        ? parse_uint32(*current_id_str).value_or(0)
        : 0;
    const std::uint32_t next_id = (current_id == 0 || current_id >= kMaxOmemoDeviceId)
        ? 1
        : current_id + 1;

    return generate_and_store_spk(self, next_id);
}

void ensure_prekeys(omemo &self, xmpp_ctx_t *context)
{
    OMEMO_ASSERT(self.context, "signal context must exist before generating prekeys");
    OMEMO_ASSERT(context != nullptr, "xmpp context must exist before serializing prekeys");

    // Rotate the signed prekey if it is older than kSignedPreKeyRotationSecs.
    // This is checked on every bundle/encode call; the rotation is cheap (no-op
    // unless the rotation interval has elapsed).
    const bool rotated = rotate_signed_prekey_if_due(self);

    if (!rotated && load_string(self, kSignedPreKeyRecord))
    {
        // Index exists — check whether it is complete.  If fewer than
        // kPreKeyCount entries are indexed (possible after a partial write or
        // an earlier bug), repair it from the on-disk records before returning.
        const auto existing_index = load_string(self, kPrekeys);
        if (existing_index)
        {
            const auto parts = split(*existing_index, ';');
            if (parts.size() < kPreKeyCount)
            {
                weechat_printf(nullptr,
                               "%somemo: prekeys index has only %zu entries (expected %u); repairing",
                               weechat_prefix("network"),
                               parts.size(),
                               kPreKeyCount);
                repair_prekeys_index(self, context);
            }
            return;
        }
        // Signed prekey record exists but index is missing — fall through to
        // regenerate prekeys (signed prekey is reused, only prekey material changes).
    }

    // First-time or post-rotation generation: generate SPK (id=1 for first time,
    // or the rotated id was already stored by rotate_signed_prekey_if_due) and
    // generate a fresh batch of prekeys.
    if (!load_string(self, kSignedPreKeyRecord))
    {
        // No SPK at all — generate id=1 fresh.
        if (!generate_and_store_spk(self, 1))
            return;
    }

    // Determine the starting ID for the new pre-key batch.
    // On first run kMaxPreKeyId is absent — start at kPreKeyStart (1).
    // After SPK rotation (or index loss), continue from max_id + 1 so we
    // never reuse IDs that may still be in-flight on a peer's bundle cache.
    // This mirrors Profanity's max_pre_key_id + 1 approach.
    const auto max_id_str = load_string(self, kMaxPreKeyId);
    const std::uint32_t prev_max = max_id_str
        ? parse_uint32(*max_id_str).value_or(kPreKeyStart - 1)
        : kPreKeyStart - 1;
    // Avoid wrapping past kMaxOmemoDeviceId; restart at kPreKeyStart if we
    // would overflow.
    const std::uint32_t start_id = (prev_max + 1 > kMaxOmemoDeviceId - kPreKeyCount)
        ? kPreKeyStart
        : prev_max + 1;

    signal_protocol_key_helper_pre_key_list_node *pre_key_head_raw = nullptr;
    if (signal_protocol_key_helper_generate_pre_keys(
            &pre_key_head_raw, start_id, kPreKeyCount, self.context) != 0)
    {
        return;
    }

    unique_pre_key_list pre_key_head {pre_key_head_raw};
    std::vector<std::string> serialized_prekeys;
    std::uint32_t new_max_id = prev_max;
    for (auto *node = pre_key_head.get(); node;
         node = signal_protocol_key_helper_key_list_next(node))
    {
        session_pre_key *pre_key = signal_protocol_key_helper_key_list_element(node);
        ec_key_pair *pre_key_pair = session_pre_key_get_key_pair(pre_key);
        ec_public_key *pre_key_public = ec_key_pair_get_public(pre_key_pair);

        signal_buffer *pre_key_record_raw = nullptr;
        if (session_pre_key_serialize(&pre_key_record_raw, pre_key) != 0)
            continue;
        unique_signal_buffer pre_key_record {pre_key_record_raw};
        store_bytes(self,
                    key_for_prekey_record(session_pre_key_get_id(pre_key)),
                    signal_buffer_const_data(pre_key_record.get()),
                    signal_buffer_len(pre_key_record.get()));

        const std::uint32_t this_id = session_pre_key_get_id(pre_key);
        if (this_id > new_max_id)
            new_max_id = this_id;

        serialized_prekeys.push_back(fmt::format(
            "{},{}",
            this_id,
            serialize_public_key(context, pre_key_public)));
    }

    if (serialized_prekeys.size() < kMinPreKeyCount)
    {
        weechat_printf(nullptr,
                       "%somemo: generated only %zu prekeys (XEP-0384 minimum is %u)",
                       weechat_prefix("error"),
                       serialized_prekeys.size(),
                       kMinPreKeyCount);
    }

    store_string(self, kPrekeys, join(serialized_prekeys, ";"));
    // Persist the new high-water mark so the next rotation or regeneration
    // continues from here rather than restarting at kPreKeyStart.
    store_string(self, kMaxPreKeyId, fmt::format("{}", new_max_id));
}

// Generate a fresh pre-key with the same ID as `used_id`, store it to LMDB,
// and update the kPrekeys index so the bundle reflects the new public key.
// Returns true if the replacement succeeded.
[[nodiscard]] bool replace_used_prekey(omemo &self, xmpp_ctx_t *context, std::uint32_t used_id)
{
    OMEMO_ASSERT(self.context, "signal context required for pre-key replacement");
    OMEMO_ASSERT(context != nullptr, "xmpp context required for pre-key replacement");

    // Generate a single replacement pre-key with the same ID.
    signal_protocol_key_helper_pre_key_list_node *node_raw = nullptr;
    if (signal_protocol_key_helper_generate_pre_keys(
            &node_raw, used_id, 1, self.context) != 0)
        return false;
    unique_pre_key_list node_head {node_raw};
    signal_protocol_key_helper_pre_key_list_node *node = node_head.get();
    if (!node) return false;

    session_pre_key *new_pre_key = signal_protocol_key_helper_key_list_element(node);
    ec_key_pair *new_pair = session_pre_key_get_key_pair(new_pre_key);
    ec_public_key *new_public = ec_key_pair_get_public(new_pair);

    signal_buffer *record_raw = nullptr;
    if (session_pre_key_serialize(&record_raw, new_pre_key) != 0)
        return false;
    unique_signal_buffer record {record_raw};

    // Store new record and update kPrekeys index.
    store_bytes(self, key_for_prekey_record(used_id),
                signal_buffer_const_data(record.get()),
                signal_buffer_len(record.get()));

    const auto new_public_b64 = serialize_public_key(context, new_public);
    const auto entry = fmt::format("{},{}", used_id, new_public_b64);

    // Replace the old entry (same id prefix) in kPrekeys.
    const auto existing = load_string(self, kPrekeys).value_or(std::string {});
    std::vector<std::string> parts = split(existing, ';');
    bool found = false;
    for (auto &p : parts)
    {
        auto comma = p.find(',');
        if (comma != std::string::npos)
        {
            const auto id_str = p.substr(0, comma);
            if (parse_uint32(id_str).value_or(0) == used_id)
            {
                p = entry;
                found = true;
                break;
            }
        }
    }
    if (!found) parts.push_back(entry);
    store_string(self, kPrekeys, join(parts, ";"));
    return true;
}

[[nodiscard]] auto signal_address_name(const signal_protocol_address *address) -> std::string
{
    return address ? std::string {address->name, address->name_len} : std::string {};
}

[[nodiscard]] auto make_signal_address(std::string_view jid, std::int32_t device_id)
    -> signal_address_view
{
    signal_address_view view;
    view.name = std::string {jid};
    view.address.name = view.name.c_str();
    view.address.name_len = view.name.size();
    view.address.device_id = device_id;
    return view;
}

[[nodiscard]] auto deserialize_public_key(std::string_view encoded,
                                          signal_context *signal_context_ptr)
    -> std::optional<libsignal::public_key>
{
    const auto decoded = base64_decode(nullptr, encoded);
    if (decoded.empty())
        return std::nullopt;

    return libsignal::public_key(decoded.data(), decoded.size(), signal_context_ptr);
}

[[nodiscard]] auto establish_session_from_bundle(omemo &self, xmpp_ctx_t * /*context*/,
                                                 std::string_view jid,
                                                 std::uint32_t remote_device_id)
    -> bool
{
    OMEMO_ASSERT(self.context, "signal context must exist before building a session from a bundle");
    OMEMO_ASSERT(self.store_context, "signal store context must exist before building a session from a bundle");
    OMEMO_ASSERT(!jid.empty(), "peer jid must be present when building a session from a bundle");
    OMEMO_ASSERT(remote_device_id != 0, "peer device id must be non-zero when building a session from a bundle");

    const auto bundle = load_bundle(self, jid, remote_device_id);
    if (!bundle || bundle->prekeys.empty())
    {
        weechat_printf(nullptr,
                       "%somemo: session bootstrap failed for %.*s/%u: no cached bundle or bundle has no prekeys",
                       weechat_prefix("error"),
                       static_cast<int>(jid.size()), jid.data(), remote_device_id);
        return false;
    }

    // Pick a uniformly random pre-key from the bundle (Signal protocol best
    // practice and matches Profanity's random index selection).  Using front()
    // would let the recipient correlate initiator identity via key-consumption
    // order, which is a privacy leak.
    const std::size_t pk_count = bundle->prekeys.size();
    std::size_t pk_index = 0;
    if (pk_count > 1)
    {
        thread_local std::mt19937 rng { std::random_device{}() };
        std::uniform_int_distribution<std::size_t> dist { 0, pk_count - 1 };
        pk_index = dist(rng);
    }
    const auto &chosen_prekey = bundle->prekeys[pk_index];

    const auto signed_pre_key_id_opt = parse_uint32(bundle->signed_pre_key_id);
    const auto pre_key_id_opt = parse_uint32(chosen_prekey.first);
    if (!signed_pre_key_id_opt || !pre_key_id_opt)
    {
        weechat_printf(nullptr,
                       "%somemo: session bootstrap failed for %.*s/%u: invalid signed-prekey id '%s' or prekey id '%s'",
                       weechat_prefix("error"),
                       static_cast<int>(jid.size()), jid.data(), remote_device_id,
                       bundle->signed_pre_key_id.c_str(), chosen_prekey.first.c_str());
        return false;
    }
    const auto signed_pre_key_id = *signed_pre_key_id_opt;
    const auto pre_key_id = *pre_key_id_opt;

    auto identity_key = deserialize_public_key(bundle->identity_key, self.context);
    auto signed_pre_key = deserialize_public_key(bundle->signed_pre_key, self.context);
    auto one_time_pre_key = deserialize_public_key(chosen_prekey.second, self.context);
    if (!identity_key || !signed_pre_key || !one_time_pre_key)
    {
        weechat_printf(nullptr,
                       "%somemo: session bootstrap failed for %.*s/%u: could not deserialize identity/signed-prekey/prekey public keys",
                       weechat_prefix("error"),
                       static_cast<int>(jid.size()), jid.data(), remote_device_id);
        return false;
    }

    auto signature = base64_decode(nullptr, bundle->signed_pre_key_signature);
    if (signature.empty())
    {
        weechat_printf(nullptr,
                       "%somemo: session bootstrap failed for %.*s/%u: signed prekey signature is empty/invalid base64",
                       weechat_prefix("error"),
                       static_cast<int>(jid.size()), jid.data(), remote_device_id);
        return false;
    }

    const auto signed_pre_key_raw = base64_decode(nullptr, bundle->signed_pre_key);
    if (signed_pre_key_raw.empty())
    {
        weechat_printf(nullptr,
                       "%somemo: session bootstrap failed for %.*s/%u: signed prekey payload is empty/invalid base64",
                       weechat_prefix("error"),
                       static_cast<int>(jid.size()), jid.data(), remote_device_id);
        return false;
    }

    // Validate bundle signature before invoking libsignal session setup.
    // Some broken bundles trigger signature failures; rejecting them here keeps
    // the failure explicit and avoids pushing invalid material further.
    if (curve_verify_signature(*(*identity_key),
                               signed_pre_key_raw.data(), signed_pre_key_raw.size(),
                               signature.data(), signature.size()) <= 0)
    {
        weechat_printf(nullptr,
                       "%somemo: session bootstrap rejected invalid signed-prekey signature for %.*s/%u",
                       weechat_prefix("error"),
                       static_cast<int>(jid.size()), jid.data(), remote_device_id);
        return false;
    }

    XDEBUG("omemo: building session for {}/{} using spk={} pk={} (bundle prekeys={})",
           std::string_view(jid.data(), jid.size()), remote_device_id,
           signed_pre_key_id, pre_key_id, bundle->prekeys.size());

    auto address = make_signal_address(jid, static_cast<std::int32_t>(remote_device_id));
    const std::uint32_t bundle_registration_id = remote_device_id;
    libsignal::pre_key_bundle pre_key_bundle(
        bundle_registration_id,
        static_cast<int>(remote_device_id),
        pre_key_id,
        *(*one_time_pre_key),
        signed_pre_key_id,
        *(*signed_pre_key),
        signature.data(),
        signature.size(),
        *(*identity_key));

    libsignal::session_builder builder(self.store_context, &address.address, self.context);
    try
    {
        builder.process_pre_key_bundle(pre_key_bundle);
    }
    catch (const std::exception &ex)
    {
        weechat_printf(nullptr,
                       "%somemo: session bootstrap failed for %.*s/%u during process_pre_key_bundle: %s",
                       weechat_prefix("error"),
                       static_cast<int>(jid.size()), jid.data(), remote_device_id,
                       ex.what());
        return false;
    }
    XDEBUG("omemo: session bootstrap succeeded for {}/{}",
           std::string_view(jid.data(), jid.size()), remote_device_id);
    return true;
}

int identity_get_key_pair(signal_buffer **public_data, signal_buffer **private_data, void *user_data)
{
    auto *self = static_cast<omemo *>(user_data);
    if (!self)
        return SG_ERR_INVAL;

    const auto public_bytes = load_bytes(*self, kIdentityPublicKey);
    const auto private_bytes = load_bytes(*self, kIdentityPrivateKey);
    if (!public_bytes || !private_bytes)
        return SG_ERR_INVALID_KEY;

    *public_data = signal_buffer_create(public_bytes->data(), public_bytes->size());
    *private_data = signal_buffer_create(private_bytes->data(), private_bytes->size());
    return (*public_data && *private_data) ? SG_SUCCESS : SG_ERR_NOMEM;
}

int identity_get_local_registration_id(void *user_data, std::uint32_t *registration_id)
{
    auto *self = static_cast<omemo *>(user_data);
    if (!self || !registration_id)
        return SG_ERR_INVAL;

    const auto stored = load_string(*self, kRegistrationIdKey);
    if (!stored)
        return SG_ERR_INVALID_KEY_ID;

    *registration_id = parse_uint32(*stored).value_or(0);
    return *registration_id != 0 ? SG_SUCCESS : SG_ERR_INVALID_KEY_ID;
}

int identity_save(const signal_protocol_address *address, std::uint8_t *key_data,
                  std::size_t key_len, void *user_data)
{
    auto *self = static_cast<omemo *>(user_data);
    if (!self || !address || !key_data)
        return SG_ERR_INVAL;

    const std::string addr_name = signal_address_name(address);
    store_bytes(*self, key_for_identity(addr_name, address->device_id), key_data, key_len);

    // XEP-0450 §5.2: apply any trust decisions that were deferred while this
    // identity key was not yet known.
    const std::vector<std::uint8_t> key_vec(key_data, key_data + key_len);
    const std::string fp = atm_fingerprint_b64(key_vec);
    if (!fp.empty())
    {
        const std::string pending_key = addr_name + '\x1F' + fp;
        auto it = self->pending_atm_trust_for_unknown_key.find(pending_key);
        if (it != self->pending_atm_trust_for_unknown_key.end())
        {
            store_atm_trust(*self, addr_name, fp, it->second);
            self->pending_atm_trust_for_unknown_key.erase(it);
        }
    }

    return SG_SUCCESS;
}

int identity_is_trusted(const signal_protocol_address *address, std::uint8_t *key_data,
                        std::size_t key_len, void *user_data)
{
    auto *self = static_cast<omemo *>(user_data);
    if (!self || !address || !key_data)
        return SG_ERR_INVAL;

    // XEP-0450: if ATM trust level is stored, use it instead of plain TOFU.
    if (weechat::config::instance && weechat_config_boolean(weechat::config::instance->look.omemo_atm))
    {
        const std::vector<std::uint8_t> incoming {key_data, key_data + key_len};
        const std::string fp = atm_fingerprint_b64(incoming);
        if (!fp.empty())
        {
            const auto trust = load_atm_trust(*self, signal_address_name(address), fp);
            if (trust)
            {
                if (*trust == "trusted")   return 1;
                if (*trust == "distrusted") return 0;
                // "undecided" → fall through to TOFU below
            }
        }
    }

    const auto stored = load_bytes(*self, key_for_identity(signal_address_name(address), address->device_id));
    if (!stored)
        return 1;

    const std::vector<std::uint8_t> incoming {key_data, key_data + key_len};
    return *stored == incoming ? 1 : 0;
}

int pre_key_load(signal_buffer **record, std::uint32_t pre_key_id, void *user_data)
{
    auto *self = static_cast<omemo *>(user_data);
    if (!self || !record)
        return SG_ERR_INVAL;

    const auto stored = load_bytes(*self, key_for_prekey_record(pre_key_id));
    if (!stored)
        return SG_ERR_INVALID_KEY_ID;

    *record = signal_buffer_create(stored->data(), stored->size());
    return *record ? SG_SUCCESS : SG_ERR_NOMEM;
}

int pre_key_store_record(std::uint32_t pre_key_id, std::uint8_t *record, std::size_t record_len, void *user_data)
{
    auto *self = static_cast<omemo *>(user_data);
    if (!self || !record)
        return SG_ERR_INVAL;

    store_bytes(*self, key_for_prekey_record(pre_key_id), record, record_len);
    return SG_SUCCESS;
}

int pre_key_contains(std::uint32_t pre_key_id, void *user_data)
{
    auto *self = static_cast<omemo *>(user_data);
    return self && load_bytes(*self, key_for_prekey_record(pre_key_id)) ? 1 : 0;
}

int pre_key_remove(std::uint32_t pre_key_id, void *user_data)
{
    auto *self = static_cast<omemo *>(user_data);
    if (!self)
        return SG_ERR_INVAL;

    auto transaction = lmdb::txn::begin(self->db_env);
    if (self->dbi.omemo.del(transaction, key_for_prekey_record(pre_key_id)))
        transaction.commit();
    else
        transaction.abort();
    return SG_SUCCESS;
}

int signed_pre_key_load(signal_buffer **record, std::uint32_t signed_pre_key_id, void *user_data)
{
    auto *self = static_cast<omemo *>(user_data);
    if (!self || !record)
        return SG_ERR_INVAL;

    const auto stored = load_bytes(*self, key_for_signed_prekey_record(signed_pre_key_id));
    if (!stored)
        return SG_ERR_INVALID_KEY_ID;

    *record = signal_buffer_create(stored->data(), stored->size());
    return *record ? SG_SUCCESS : SG_ERR_NOMEM;
}

int signed_pre_key_store_record(std::uint32_t signed_pre_key_id, std::uint8_t *record,
                                std::size_t record_len, void *user_data)
{
    auto *self = static_cast<omemo *>(user_data);
    if (!self || !record)
        return SG_ERR_INVAL;

    store_bytes(*self, key_for_signed_prekey_record(signed_pre_key_id), record, record_len);
    return SG_SUCCESS;
}

int signed_pre_key_contains(std::uint32_t signed_pre_key_id, void *user_data)
{
    auto *self = static_cast<omemo *>(user_data);
    return self && load_bytes(*self, key_for_signed_prekey_record(signed_pre_key_id)) ? 1 : 0;
}

int signed_pre_key_remove(std::uint32_t signed_pre_key_id, void *user_data)
{
    auto *self = static_cast<omemo *>(user_data);
    if (!self)
        return SG_ERR_INVAL;

    auto transaction = lmdb::txn::begin(self->db_env);
    if (self->dbi.omemo.del(transaction, key_for_signed_prekey_record(signed_pre_key_id)))
        transaction.commit();
    else
        transaction.abort();
    return SG_SUCCESS;
}

int session_load(signal_buffer **record, signal_buffer **user_record,
                 const signal_protocol_address *address, void *user_data)
{
    auto *self = static_cast<omemo *>(user_data);
    if (!self || !record || !address)
        return SG_ERR_INVAL;

    const auto stored = load_bytes(*self, key_for_session(signal_address_name(address), address->device_id));
    if (!stored)
        return 0;

    *record = signal_buffer_create(stored->data(), stored->size());
    if (user_record)
        *user_record = nullptr;
    return *record ? 1 : SG_ERR_NOMEM;
}

int session_get_sub_devices(signal_int_list **sessions, const char *name,
                            std::size_t name_len, void *user_data)
{
    auto *self = static_cast<omemo *>(user_data);
    if (!self || !sessions || !name)
        return SG_ERR_INVAL;

    *sessions = signal_int_list_alloc();
    if (!*sessions)
        return SG_ERR_NOMEM;

    const auto prefix = fmt::format("session:{}:", std::string_view {name, name_len});
    auto transaction = lmdb::txn::begin(self->db_env, nullptr, MDB_RDONLY);
    auto cursor = lmdb::cursor::open(transaction, self->dbi.omemo);
    std::string_view key;
    std::string_view value;
    int count = 0;

    for (bool found = cursor.get(key, value, MDB_FIRST); found;
         found = cursor.get(key, value, MDB_NEXT))
    {
        if (!key.starts_with(prefix))
            continue;

        const auto device_part = key.substr(prefix.size());
        if (const auto device_id = parse_uint32(device_part))
        {
            signal_int_list_push_back(*sessions, static_cast<int>(*device_id));
            ++count;
        }
    }

    return count;
}

int session_store_record(const signal_protocol_address *address, std::uint8_t *record,
                         std::size_t record_len, std::uint8_t *user_record,
                         std::size_t user_record_len, void *user_data)
{
    (void) user_record;
    (void) user_record_len;

    auto *self = static_cast<omemo *>(user_data);
    if (!self || !address || !record)
        return SG_ERR_INVAL;

    store_bytes(*self, key_for_session(signal_address_name(address), address->device_id), record, record_len);
    return SG_SUCCESS;
}

int session_contains(const signal_protocol_address *address, void *user_data)
{
    auto *self = static_cast<omemo *>(user_data);
    if (!self || !address)
        return 0;

    return load_bytes(*self, key_for_session(signal_address_name(address), address->device_id)) ? 1 : 0;
}

int session_delete(const signal_protocol_address *address, void *user_data)
{
    auto *self = static_cast<omemo *>(user_data);
    if (!self || !address)
        return SG_ERR_INVAL;

    auto transaction = lmdb::txn::begin(self->db_env);
    const bool deleted = self->dbi.omemo.del(transaction,
                                             key_for_session(signal_address_name(address), address->device_id));
    if (deleted)
        transaction.commit();
    else
        transaction.abort();
    return deleted ? 1 : 0;
}

int session_delete_all(const char *name, std::size_t name_len, void *user_data)
{
    auto *self = static_cast<omemo *>(user_data);
    if (!self || !name)
        return SG_ERR_INVAL;

    const auto prefix = fmt::format("session:{}:", std::string_view {name, name_len});
    auto transaction = lmdb::txn::begin(self->db_env);
    auto cursor = lmdb::cursor::open(transaction, self->dbi.omemo);
    std::string_view key;
    std::string_view value;
    int deleted = 0;

    for (bool found = cursor.get(key, value, MDB_FIRST); found;
         found = cursor.get(key, value, MDB_NEXT))
    {
        if (!key.starts_with(prefix))
            continue;
        cursor.del();
        ++deleted;
    }

    transaction.commit();
    return deleted;
}

int sender_key_store_record(const signal_protocol_sender_key_name *sender_key_name,
                            std::uint8_t *record, std::size_t record_len,
                            std::uint8_t *user_record, std::size_t user_record_len,
                            void *user_data)
{
    (void) user_record;
    (void) user_record_len;

    auto *self = static_cast<omemo *>(user_data);
    if (!self || !sender_key_name || !record)
        return SG_ERR_INVAL;

    store_bytes(*self,
                key_for_sender_key(std::string_view {sender_key_name->group_id, sender_key_name->group_id_len},
                                   signal_address_name(&sender_key_name->sender),
                                   sender_key_name->sender.device_id),
                record,
                record_len);
    return SG_SUCCESS;
}

int sender_key_load(signal_buffer **record, signal_buffer **user_record,
                    const signal_protocol_sender_key_name *sender_key_name,
                    void *user_data)
{
    auto *self = static_cast<omemo *>(user_data);
    if (!self || !record || !sender_key_name)
        return SG_ERR_INVAL;

    const auto stored = load_bytes(
        *self,
        key_for_sender_key(std::string_view {sender_key_name->group_id, sender_key_name->group_id_len},
                           signal_address_name(&sender_key_name->sender),
                           sender_key_name->sender.device_id));
    if (!stored)
        return 0;

    *record = signal_buffer_create(stored->data(), stored->size());
    if (user_record)
        *user_record = nullptr;
    return *record ? 1 : SG_SUCCESS;
}

void remove_prefixed_keys(omemo &self, std::string_view prefix)
{
    auto transaction = lmdb::txn::begin(self.db_env);
    auto cursor = lmdb::cursor::open(transaction, self.dbi.omemo);
    std::string_view key;
    std::string_view value;

    for (bool found = cursor.get(key, value, MDB_FIRST); found;
         found = cursor.get(key, value, MDB_NEXT))
    {
        if (key.starts_with(prefix))
            cursor.del();
    }

    transaction.commit();
}

[[nodiscard]] auto extract_devices_from_items(xmpp_stanza_t *items) -> std::vector<std::string>
{
    std::vector<std::string> device_ids;
    if (!items)
    {
        weechat_printf(nullptr, "%somemo: extract_devices: items stanza is nullptr", weechat_prefix("error"));
        return device_ids;
    }

    xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(items, "item");
    if (!item)
    {
        weechat_printf(nullptr, "%somemo: extract_devices: no <item> child in items", weechat_prefix("error"));
        return device_ids;
    }

    xmpp_stanza_t *devices = xmpp_stanza_get_child_by_name_and_ns(
        item, "devices", kOmemoNs.data());
    if (!devices)
    {
        weechat_printf(nullptr, "%somemo: extract_devices: no <devices> with namespace %s in item", 
                       weechat_prefix("error"), kOmemoNs.data());
        return device_ids;
    }

    int total_children = 0;
    int valid_devices = 0;
    
    for (xmpp_stanza_t *device = xmpp_stanza_get_children(devices);
         device;
         device = xmpp_stanza_get_next(device))
    {
        total_children++;
        const char *name = xmpp_stanza_get_name(device);
        if (!name || weechat_strcasecmp(name, "device") != 0)
        {
            XDEBUG("omemo: child {} has name '{}' (skipping, expected 'device')",
                   total_children, name ? name : "(null)");
            continue;
        }

        const char *id = xmpp_stanza_get_id(device);
        if (!id || !*id)
        {
            XDEBUG("omemo: child {} is <device> but has no id attribute", total_children);
            continue;
        }

        const auto parsed_id = parse_uint32(id);
        if (!parsed_id || !is_valid_omemo_device_id(*parsed_id))
        {
            weechat_printf(nullptr,
                           "%somemo: ignoring invalid device id '%s' in devicelist",
                           weechat_prefix("error"),
                           id);
            continue;
        }

        valid_devices++;
        XDEBUG("omemo: extracted device {} (valid_count={})", id, valid_devices);
        device_ids.emplace_back(id);
    }

    XDEBUG("omemo: stanza had {} total children, {} valid devices extracted",
           total_children, valid_devices);

    std::sort(device_ids.begin(), device_ids.end());
    device_ids.erase(std::unique(device_ids.begin(), device_ids.end()), device_ids.end());
    return device_ids;
}

[[nodiscard]] auto extract_bundle_from_items(xmpp_stanza_t *items) -> std::optional<bundle_metadata>
{
    if (!items)
        return std::nullopt;

    xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(items, "item");
    if (!item)
        return std::nullopt;

    xmpp_stanza_t *bundle_stanza = xmpp_stanza_get_child_by_name_and_ns(item, "bundle", kOmemoNs.data());
    if (!bundle_stanza)
        return std::nullopt;

    bundle_metadata bundle;

    for (xmpp_stanza_t *child = xmpp_stanza_get_children(bundle_stanza);
         child;
         child = xmpp_stanza_get_next(child))
    {
