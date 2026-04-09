struct mac_context {
    unique_gcry_mac handle;
};

struct digest_context {
    gcry_md_hd_t handle = nullptr;

    ~digest_context()
    {
        if (handle)
            gcry_md_close(handle);
    }
};

[[nodiscard]] auto aes_mode_for_signal(int cipher) -> std::optional<int>
{
    switch (cipher)
    {
        case SG_CIPHER_AES_CTR_NOPADDING:
            return GCRY_CIPHER_MODE_CTR;
        case SG_CIPHER_AES_CBC_PKCS5:
            return GCRY_CIPHER_MODE_CBC;
        default:
            return std::nullopt;
    }
}

int crypto_random(uint8_t *data, size_t len, void *user_data)
{
    (void) user_data;

    if (!data)
        return SG_ERR_INVAL;

    gcry_randomize(data, len, GCRY_STRONG_RANDOM);
    return SG_SUCCESS;
}

int crypto_hmac_sha256_init(void **hmac_context, const uint8_t *key, size_t key_len, void *user_data)
{
    (void) user_data;

    if (!hmac_context || !key)
        return SG_ERR_INVAL;

    gcry_mac_hd_t mac_raw = nullptr;
    if (gcry_mac_open(&mac_raw, GCRY_MAC_HMAC_SHA256, 0, nullptr) != 0)
        return SG_ERR_UNKNOWN;

    auto context = std::make_unique<mac_context>();
    context->handle.reset(mac_raw);
    if (gcry_mac_setkey(context->handle.get(), key, key_len) != 0)
        return SG_ERR_UNKNOWN;

    *hmac_context = context.release();
    return SG_SUCCESS;
}

int crypto_hmac_sha256_update(void *hmac_context, const uint8_t *data, size_t data_len, void *user_data)
{
    (void) user_data;

    auto *context = static_cast<mac_context *>(hmac_context);
    if (!context || !data)
        return SG_ERR_INVAL;

    return gcry_mac_write(context->handle.get(), data, data_len) == 0 ? SG_SUCCESS : SG_ERR_UNKNOWN;
}

int crypto_hmac_sha256_final(void *hmac_context, signal_buffer **output, void *user_data)
{
    (void) user_data;

    auto *context = static_cast<mac_context *>(hmac_context);
    if (!context || !output)
        return SG_ERR_INVAL;

    std::array<std::uint8_t, 32> digest {};
    size_t digest_len = digest.size();
    if (gcry_mac_read(context->handle.get(), digest.data(), &digest_len) != 0)
        return SG_ERR_UNKNOWN;

    *output = signal_buffer_create(digest.data(), digest_len);
    return *output ? SG_SUCCESS : SG_ERR_NOMEM;
}

void crypto_hmac_sha256_cleanup(void *hmac_context, void *user_data)
{
    (void) user_data;
    // RAII exception: Signal C API transfers ownership via void* — delete is the
    // mandatory cleanup paired with the make_unique<mac_context>().release() above.
    delete static_cast<mac_context *>(hmac_context); // NOLINT(cppcoreguidelines-owning-memory)
}

int crypto_sha512_init(void **digest_context_ptr, void *user_data)
{
    (void) user_data;

    if (!digest_context_ptr)
        return SG_ERR_INVAL;

    auto context = std::make_unique<digest_context>();
    if (gcry_md_open(&context->handle, GCRY_MD_SHA512, 0) != 0)
        return SG_ERR_UNKNOWN;

    *digest_context_ptr = context.release();
    return SG_SUCCESS;
}

int crypto_sha512_update(void *digest_context_ptr, const uint8_t *data, size_t data_len, void *user_data)
{
    (void) user_data;

    auto *context = static_cast<digest_context *>(digest_context_ptr);
    if (!context || !data)
        return SG_ERR_INVAL;

    gcry_md_write(context->handle, data, data_len);
    return SG_SUCCESS;
}

int crypto_sha512_final(void *digest_context_ptr, signal_buffer **output, void *user_data)
{
    (void) user_data;

    auto *context = static_cast<digest_context *>(digest_context_ptr);
    if (!context || !output)
        return SG_ERR_INVAL;

    gcry_md_final(context->handle);
    const auto *digest = gcry_md_read(context->handle, GCRY_MD_SHA512);
    if (!digest)
        return SG_ERR_UNKNOWN;

    *output = signal_buffer_create(digest, gcry_md_get_algo_dlen(GCRY_MD_SHA512));
    gcry_md_reset(context->handle);
    return *output ? SG_SUCCESS : SG_ERR_NOMEM;
}

void crypto_sha512_cleanup(void *digest_context_ptr, void *user_data)
{
    (void) user_data;
    // RAII exception: Signal C API transfers ownership via void* — delete is the
    // mandatory cleanup paired with the make_unique<digest_context>().release() above.
    delete static_cast<digest_context *>(digest_context_ptr); // NOLINT(cppcoreguidelines-owning-memory)
}

int crypto_encrypt(signal_buffer **output, int cipher,
                   const uint8_t *key, size_t key_len,
                   const uint8_t *iv, size_t iv_len,
                   const uint8_t *plaintext, size_t plaintext_len,
                   void *user_data)
{
    (void) user_data;

    const auto mode = aes_mode_for_signal(cipher);
    if (!output || !mode || !key || !iv || !plaintext)
        return SG_ERR_INVAL;

    gcry_cipher_hd_t cipher_raw = nullptr;
    if (gcry_cipher_open(&cipher_raw, GCRY_CIPHER_AES256, *mode, 0) != 0)
        return SG_ERR_UNKNOWN;
    unique_gcry_cipher handle {cipher_raw};

    if (gcry_cipher_setkey(handle.get(), key, key_len) != 0)
        return SG_ERR_UNKNOWN;
    if ((*mode == GCRY_CIPHER_MODE_CTR && gcry_cipher_setctr(handle.get(), iv, iv_len) != 0)
        || (*mode == GCRY_CIPHER_MODE_CBC && gcry_cipher_setiv(handle.get(), iv, iv_len) != 0))
    {
        return SG_ERR_UNKNOWN;
    }

    std::vector<uint8_t> out(plaintext, plaintext + plaintext_len);
    if (cipher == SG_CIPHER_AES_CBC_PKCS5)
        out = pkcs7_pad(std::string_view(reinterpret_cast<const char *>(plaintext), plaintext_len), 16);

    if (gcry_cipher_encrypt(handle.get(), out.data(), out.size(), out.data(), out.size()) != 0)
        return SG_ERR_UNKNOWN;

    *output = signal_buffer_create(out.data(), out.size());
    return *output ? SG_SUCCESS : SG_ERR_NOMEM;
}

int crypto_decrypt(signal_buffer **output, int cipher,
                   const uint8_t *key, size_t key_len,
                   const uint8_t *iv, size_t iv_len,
                   const uint8_t *ciphertext, size_t ciphertext_len,
                   void *user_data)
{
    (void) user_data;

    const auto mode = aes_mode_for_signal(cipher);
    if (!output || !mode || !key || !iv || !ciphertext)
        return SG_ERR_INVAL;

    gcry_cipher_hd_t cipher_raw = nullptr;
    if (gcry_cipher_open(&cipher_raw, GCRY_CIPHER_AES256, *mode, 0) != 0)
        return SG_ERR_UNKNOWN;
    unique_gcry_cipher handle {cipher_raw};

    if (gcry_cipher_setkey(handle.get(), key, key_len) != 0)
        return SG_ERR_UNKNOWN;
    if ((*mode == GCRY_CIPHER_MODE_CTR && gcry_cipher_setctr(handle.get(), iv, iv_len) != 0)
        || (*mode == GCRY_CIPHER_MODE_CBC && gcry_cipher_setiv(handle.get(), iv, iv_len) != 0))
    {
        return SG_ERR_UNKNOWN;
    }

    std::vector<uint8_t> out(ciphertext, ciphertext + ciphertext_len);
    if (gcry_cipher_decrypt(handle.get(), out.data(), out.size(), out.data(), out.size()) != 0)
        return SG_ERR_UNKNOWN;

    if (cipher == SG_CIPHER_AES_CBC_PKCS5)
    {
        const auto unpadded = pkcs7_unpad(out);
        if (!unpadded)
            return SG_ERR_UNKNOWN;
        *output = signal_buffer_create(reinterpret_cast<const uint8_t *>(unpadded->data()), unpadded->size());
    }
    else
    {
        *output = signal_buffer_create(out.data(), out.size());
    }

    return *output ? SG_SUCCESS : SG_ERR_NOMEM;
}

void crypto_lock(void *user_data)
{
    (void) user_data;
}

void crypto_unlock(void *user_data)
{
    (void) user_data;
}

[[nodiscard]] auto pkcs7_unpad(const std::vector<std::uint8_t> &padded) -> std::optional<std::string>
{
    if (padded.empty())
        return std::nullopt;

    const std::uint8_t padding = padded.back();
    if (padding == 0 || padding > 16 || padding > padded.size())
        return std::nullopt;

    for (std::size_t index = padded.size() - padding; index < padded.size(); ++index)
    {
        if (padded[index] != padding)
            return std::nullopt;
    }

    return std::string {padded.begin(), padded.end() - padding};
}

[[nodiscard]] auto serialize_public_key(xmpp_ctx_t *context, ec_public_key *key) -> std::string
{
    signal_buffer *raw = nullptr;
    if (ec_public_key_serialize(&raw, key) != 0)
        return {};

    unique_signal_buffer buffer {raw};
    return base64_encode(context,
                         signal_buffer_const_data(buffer.get()),
                         signal_buffer_len(buffer.get()));
}

// ---------------------------------------------------------------------------
// Legacy OMEMO (eu.siacs.conversations.axolotl) crypto
// Uses AES-128-GCM with explicit 12-byte IV (transmitted in <iv> element).
// Transport key = AES-128 innerKey(16) || GCM authTag(16) = 32 bytes.
// ---------------------------------------------------------------------------

struct axolotl_omemo_payload {
    std::array<std::uint8_t, 16> key {};     // AES-128 key (Signal-encrypted alongside authtag)
    std::array<std::uint8_t, 12> iv {};      // GCM nonce (transmitted in <iv> element in header)
    std::array<std::uint8_t, 16> authtag {}; // GCM auth tag (packed into Signal key, not payload)
    std::vector<std::uint8_t> payload;       // AES-128-GCM ciphertext (auth tag stripped)
};

[[nodiscard]] auto axolotl_omemo_encrypt(std::string_view plaintext) -> std::optional<axolotl_omemo_payload>
{
    if (plaintext.empty())
        return std::nullopt;

    axolotl_omemo_payload result;
    gcry_randomize(result.key.data(), result.key.size(), GCRY_STRONG_RANDOM);
    gcry_randomize(result.iv.data(), result.iv.size(), GCRY_STRONG_RANDOM);

    gcry_cipher_hd_t cipher_raw = nullptr;
    // AES-128 = GCRY_CIPHER_AES (= GCRY_CIPHER_AES128)
    if (gcry_cipher_open(&cipher_raw, GCRY_CIPHER_AES, GCRY_CIPHER_MODE_GCM, 0) != 0)
        return std::nullopt;
    unique_gcry_cipher cipher {cipher_raw};

    if (gcry_cipher_setkey(cipher.get(), result.key.data(), result.key.size()) != 0
        || gcry_cipher_setiv(cipher.get(), result.iv.data(), result.iv.size()) != 0)
    {
        return std::nullopt;
    }

    std::vector<std::uint8_t> ciphertext(plaintext.size());
    if (gcry_cipher_encrypt(cipher.get(), ciphertext.data(), ciphertext.size(),
                            plaintext.data(), plaintext.size()) != 0)
    {
        return std::nullopt;
    }

    // Retrieve GCM authentication tag (16 bytes)
    if (gcry_cipher_gettag(cipher.get(), result.authtag.data(), result.authtag.size()) != 0)
        return std::nullopt;

    result.payload = std::move(ciphertext);
    return result;
}

// Decrypt a legacy OMEMO payload.  The auth tag is checked internally by GCM.
[[nodiscard]] auto axolotl_omemo_decrypt(const std::array<std::uint8_t, 16> &key,
                                        const std::array<std::uint8_t, 12> &iv,
                                        const std::array<std::uint8_t, 16> &authtag,
                                        const std::vector<std::uint8_t> &ciphertext)
    -> std::optional<std::string>
{
    if (ciphertext.empty())
        return std::nullopt;

    gcry_cipher_hd_t cipher_raw = nullptr;
    if (gcry_cipher_open(&cipher_raw, GCRY_CIPHER_AES, GCRY_CIPHER_MODE_GCM, 0) != 0)
        return std::nullopt;
    unique_gcry_cipher cipher {cipher_raw};

    if (gcry_cipher_setkey(cipher.get(), key.data(), key.size()) != 0
        || gcry_cipher_setiv(cipher.get(), iv.data(), iv.size()) != 0)
    {
        return std::nullopt;
    }

    std::vector<std::uint8_t> plaintext(ciphertext.size());
    if (gcry_cipher_decrypt(cipher.get(), plaintext.data(), plaintext.size(),
                            ciphertext.data(), ciphertext.size()) != 0)
    {
        return std::nullopt;
    }

    // Verify the GCM authentication tag
    if (gcry_cipher_checktag(cipher.get(), authtag.data(), authtag.size()) != 0)
    {
        weechat_printf(nullptr, "%somemo: legacy OMEMO payload GCM authentication failed",
                       weechat_prefix("error"));
        return std::nullopt;
    }

    return std::string(plaintext.begin(), plaintext.end());
}

// Signal-encrypt the legacy transport key bundle: innerKey(16) || authTag(16) = 32 bytes.
[[nodiscard]] auto encrypt_axolotl_transport_key(omemo &self, std::string_view jid,
                                                std::uint32_t remote_device_id,
                                                const axolotl_omemo_payload &ep)
    -> std::optional<std::pair<std::vector<std::uint8_t>, bool>>
{
    OMEMO_ASSERT(self.context, "signal context must be initialized");
    OMEMO_ASSERT(self.store_context, "signal store context must be initialized");
    OMEMO_ASSERT(!jid.empty(), "peer jid must be non-empty");
    OMEMO_ASSERT(remote_device_id != 0, "peer device id must be non-zero");

    // Per Conversations: Signal-encrypt innerKey(16) || authTag(16) = 32 bytes
    std::array<std::uint8_t, 32> bundle {};
    std::copy_n(ep.key.begin(), 16, bundle.begin());
    std::copy_n(ep.authtag.begin(), 16, bundle.begin() + 16);

    auto address = make_signal_address(jid, static_cast<std::int32_t>(remote_device_id));

    session_cipher *cipher_raw = nullptr;
    if (session_cipher_create(&cipher_raw, self.store_context, &address.address, self.context) != 0)
        return std::nullopt;
    libsignal::unique_session_cipher cipher {cipher_raw};
    // Legacy eu.siacs.conversations.axolotl uses Signal wire-format version 3
    // (CIPHERTEXT_CURRENT_VERSION). Using CIPHERTEXT_OMEMO_VERSION (4) here
    // causes the session to be established at v4, which ConverseJS and other
    // legacy clients cannot handle: they send v3 messages back, which libsignal
    // then rejects with "Message version 3, but session version 4".
    session_cipher_set_version(cipher.get(), CIPHERTEXT_CURRENT_VERSION);

    ciphertext_message *message_raw = nullptr;
    if (session_cipher_encrypt(cipher.get(), bundle.data(), bundle.size(), &message_raw) != 0)
        return std::nullopt;
    libsignal::unique_ciphertext_message message {message_raw};

    const signal_buffer *serialized = ciphertext_message_get_serialized(message.get());
    if (!serialized)
        return std::nullopt;

    std::vector<std::uint8_t> output(signal_buffer_const_data(serialized),
                                     signal_buffer_const_data(serialized) + signal_buffer_len(serialized));
    return std::pair<std::vector<std::uint8_t>, bool> {
        std::move(output),
        ciphertext_message_get_type(message.get()) == CIPHERTEXT_PREKEY_TYPE,
    };
}

// Signal-decrypt a legacy OMEMO transport key bundle → {innerKey(16), authTag(16)}.
// If the message was already decrypted (SG_ERR_DUPLICATE_MESSAGE), *out_is_duplicate
// is set to true and nullopt is returned.  Callers should silently skip duplicates
// rather than treating them as failures (matches Gajim DuplicateMessageException→NodeProcessed).
[[nodiscard]] auto decrypt_axolotl_transport_key(omemo &self, std::string_view jid,
                                                std::uint32_t remote_device_id,
                                                const std::vector<std::uint8_t> &serialized,
                                                bool is_prekey,
                                                std::optional<std::uint32_t> *out_used_prekey_id = nullptr,
                                                std::optional<std::uint32_t> *out_message_counter = nullptr,
                                                bool *out_is_duplicate = nullptr)
    -> std::optional<std::pair<std::array<std::uint8_t, 16>, std::array<std::uint8_t, 16>>>
{
    OMEMO_ASSERT(self.context, "signal context required");
    OMEMO_ASSERT(self.store_context, "signal store context required");
    OMEMO_ASSERT(!jid.empty(), "peer jid required");
    OMEMO_ASSERT(remote_device_id != 0, "peer device id required");
    OMEMO_ASSERT(!serialized.empty(), "serialized message required");

    if (out_is_duplicate)
        *out_is_duplicate = false;

    auto address = make_signal_address(jid, static_cast<std::int32_t>(remote_device_id));

    session_cipher *cipher_raw = nullptr;
    if (int rc = session_cipher_create(&cipher_raw, self.store_context, &address.address, self.context); rc != 0)
    {
        weechat_printf(nullptr, "%somemo: (legacy) session_cipher_create failed for %.*s/%u: rc=%d",
                       weechat_prefix("error"),
                       static_cast<int>(jid.size()), jid.data(),
                       remote_device_id, rc);
        return std::nullopt;
    }
    libsignal::unique_session_cipher cipher {cipher_raw};
    // Legacy eu.siacs.conversations.axolotl uses Signal wire-format version 3.
    // Do NOT set CIPHERTEXT_OMEMO_VERSION (4) here — that would cause a v4
    // session to be established on our side while the peer sends v3 messages,
    // resulting in "Message version 3, but session version 4" and MAC failures.
    session_cipher_set_version(cipher.get(), CIPHERTEXT_CURRENT_VERSION);

    std::uint32_t registration_id = 0;
    signal_protocol_identity_get_local_registration_id(self.store_context, &registration_id);

    signal_buffer *plaintext_raw = nullptr;
    int result = SG_ERR_INVAL;
    if (is_prekey)
    {
        pre_key_signal_message *message_raw = nullptr;
        int rc = pre_key_signal_message_deserialize_omemo(
            &message_raw, serialized.data(), serialized.size(), registration_id, self.context);
        if (rc != 0)
            rc = pre_key_signal_message_deserialize(&message_raw, serialized.data(), serialized.size(), self.context);
        if (rc != 0)
        {
            weechat_printf(nullptr, "%somemo: (legacy) pre_key_signal_message_deserialize failed for %.*s/%u: rc=%d",
                           weechat_prefix("error"),
                           static_cast<int>(jid.size()), jid.data(),
                           remote_device_id, rc);
            return std::nullopt;
        }
        libsignal::object<pre_key_signal_message> message {message_raw};
        if (out_used_prekey_id && pre_key_signal_message_has_pre_key_id(message.get()))
            *out_used_prekey_id = pre_key_signal_message_get_pre_key_id(message.get());
        // Capture the ratchet counter for heartbeat logic (XEP-0384 §6).
        if (out_message_counter)
        {
            const signal_message *inner_sm = pre_key_signal_message_get_signal_message(message.get());
            if (inner_sm)
                *out_message_counter = signal_message_get_counter(inner_sm);
        }
        result = session_cipher_decrypt_pre_key_signal_message(cipher.get(), message.get(), nullptr, &plaintext_raw);

        // If prekey decryption fails but a session already exists, the prekey
        // was consumed on a previous delivery (e.g. MAM replay consumed it live,
        // or this is a repeated MAM fetch of the same archived stanza).
        // Fall back to decrypting the embedded SignalMessage — if the ratchet
        // position still matches, this recovers the plaintext.  If the ratchet
        // has advanced past this message (rc=-1001) it is irrecoverable; we
        // return nullopt so the caller can silently discard it per XEP-0384 §6.
        if (result != 0 && plaintext_raw == nullptr)
        {
            weechat_printf(nullptr, "%somemo: (legacy) prekey decrypt failed for %.*s/%u: rc=%d; trying inner SignalMessage",
                           weechat_prefix("error"),
                           static_cast<int>(jid.size()), jid.data(),
                           remote_device_id, result);
            signal_message *inner = pre_key_signal_message_get_signal_message(message.get());
            if (inner)
            {
                result = session_cipher_decrypt_signal_message(cipher.get(), inner, nullptr, &plaintext_raw);
            }
        }
    }
    else
    {
        signal_message *message_raw = nullptr;
        int rc = signal_message_deserialize_omemo(&message_raw, serialized.data(), serialized.size(), self.context);
        if (rc != 0)
            rc = signal_message_deserialize(&message_raw, serialized.data(), serialized.size(), self.context);
        if (rc != 0)
        {
            weechat_printf(nullptr, "%somemo: (legacy) signal_message_deserialize failed for %.*s/%u: rc=%d",
                           weechat_prefix("error"),
                           static_cast<int>(jid.size()), jid.data(),
                           remote_device_id, rc);
            return std::nullopt;
        }
        libsignal::object<signal_message> message {message_raw};
        // Capture the ratchet counter for heartbeat logic (XEP-0384 §6).
        if (out_message_counter)
            *out_message_counter = signal_message_get_counter(message.get());
        result = session_cipher_decrypt_signal_message(cipher.get(), message.get(), nullptr, &plaintext_raw);
    }

    if (result != 0 || !plaintext_raw)
    {
        if (result == SG_ERR_DUPLICATE_MESSAGE)
        {
            // The Signal library already decrypted this message (live delivery
            // advanced the ratchet past this counter).  This is normal during
            // MAM replay of messages that were received live before reconnect.
            // Match Gajim's DuplicateMessageException → NodeProcessed behaviour:
            // tell the caller to skip silently.
            XDEBUG("omemo: (legacy) duplicate message for {}/{} — already decrypted",
                   jid, remote_device_id);
            if (out_is_duplicate)
                *out_is_duplicate = true;
            return std::nullopt;
        }
        weechat_printf(nullptr, "%somemo: (legacy) session_cipher_decrypt failed for %.*s/%u: rc=%d",
                       weechat_prefix("error"),
                       static_cast<int>(jid.size()), jid.data(),
                       remote_device_id, result);
        return std::nullopt;
    }

    std::unique_ptr<signal_buffer, decltype(&signal_buffer_bzero_free)> plaintext(
        plaintext_raw, signal_buffer_bzero_free);
    if (signal_buffer_len(plaintext.get()) != 32)
    {
        weechat_printf(nullptr, "%somemo: (legacy) decrypted transport key has wrong length %zu (expected 32)",
                       weechat_prefix("error"), signal_buffer_len(plaintext.get()));
        return std::nullopt;
    }

    std::array<std::uint8_t, 16> inner_key {};
    std::array<std::uint8_t, 16> auth_tag {};
    std::copy_n(signal_buffer_const_data(plaintext.get()), 16, inner_key.begin());
    std::copy_n(signal_buffer_const_data(plaintext.get()) + 16, 16, auth_tag.begin());
    return std::pair<std::array<std::uint8_t, 16>, std::array<std::uint8_t, 16>> {inner_key, auth_tag};
}

void store_bytes(omemo &self, std::string_view key, const std::uint8_t *data, std::size_t size)
{
    auto transaction = lmdb::txn::begin(self.db_env);
    self.dbi.omemo.put(transaction,
                       key,
                       std::string_view {reinterpret_cast<const char *>(data), size});
    transaction.commit();
}

[[nodiscard]] auto load_bytes(omemo &self, std::string_view key) -> std::optional<std::vector<std::uint8_t>>
{
    auto transaction = lmdb::txn::begin(self.db_env, nullptr, MDB_RDONLY);
    std::string_view value;
    if (!self.dbi.omemo.get(transaction, key, value))
        return std::nullopt;

    const auto *begin = reinterpret_cast<const std::uint8_t *>(value.data());
    return std::vector<std::uint8_t> {begin, begin + value.size()};
}

[[nodiscard]] auto serialize_bundle(const bundle_metadata &bundle) -> std::string
{
    std::vector<std::string> entries;
    entries.push_back(fmt::format("spk_id={}", bundle.signed_pre_key_id));
    entries.push_back(fmt::format("spk={}", bundle.signed_pre_key));
    entries.push_back(fmt::format("spks={}", bundle.signed_pre_key_signature));
    entries.push_back(fmt::format("ik={}", bundle.identity_key));

    for (const auto &[id, key] : bundle.prekeys)
        entries.push_back(fmt::format("pk={},{}", id, key));

    return join(entries, "\n");
}

[[nodiscard]] auto deserialize_bundle(std::string_view serialized) -> bundle_metadata
{
    bundle_metadata bundle;

    for (const auto &line : split(serialized, '\n'))
    {
        if (line.rfind("spk_id=", 0) == 0)
            bundle.signed_pre_key_id = line.substr(7);
        else if (line.rfind("spk=", 0) == 0)
            bundle.signed_pre_key = line.substr(4);
