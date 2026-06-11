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

[[nodiscard]] auto aes_mode_for_signal(int cipher) -> std::expected<int, std::string>
{
    switch (cipher)
    {
        case SG_CIPHER_AES_CTR_NOPADDING:
            return GCRY_CIPHER_MODE_CTR;
        case SG_CIPHER_AES_CBC_PKCS5:
            return GCRY_CIPHER_MODE_CBC;
        default:
            return std::unexpected("unknown cipher mode");
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

    auto *context = make_signal_ctx<mac_context>();
    context->handle.reset(mac_raw);
    if (gcry_mac_setkey(context->handle.get(), key, key_len) != 0)
        return SG_ERR_UNKNOWN;

    *hmac_context = static_cast<void *>(context);
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
    free_signal_ctx<mac_context>(hmac_context);
}

int crypto_sha512_init(void **digest_context_ptr, void *user_data)
{
    (void) user_data;

    if (!digest_context_ptr)
        return SG_ERR_INVAL;

    auto *context = make_signal_ctx<digest_context>();
    if (gcry_md_open(&context->handle, GCRY_MD_SHA512, 0) != 0)
        return SG_ERR_UNKNOWN;

    *digest_context_ptr = static_cast<void *>(context);
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
    free_signal_ctx<digest_context>(digest_context_ptr);
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

    std::span<uint8_t> out_span = out;
    if (gcry_cipher_encrypt(handle.get(), out_span.data(), out_span.size(), out_span.data(), out_span.size()) != 0)
        return SG_ERR_UNKNOWN;

    *output = signal_buffer_create(out_span.data(), out_span.size());
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
    std::span<uint8_t> out_span = out;
    if (gcry_cipher_decrypt(handle.get(), out_span.data(), out_span.size(), out_span.data(), out_span.size()) != 0)
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
        *output = signal_buffer_create(out_span.data(), out_span.size());
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

[[nodiscard]] auto pkcs7_unpad(const std::vector<std::uint8_t> &padded) -> std::expected<std::string, std::string>
{
    if (padded.empty())
        return std::unexpected("pkcs7: empty input");

    const std::uint8_t padding = padded.back();
    if (padding == 0 || padding > 16 || padding > padded.size())
        return std::unexpected(fmt::format("pkcs7: invalid padding byte {}", padding));

    for (std::size_t index = padded.size() - padding; index < padded.size(); ++index)
    {
        if (padded[index] != padding)
            return std::unexpected("pkcs7: padding bytes inconsistent");
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
                         std::span<const unsigned char>(
                             signal_buffer_const_data(buffer.get()),
                             signal_buffer_len(buffer.get())));
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

namespace {

// AES-128-GCM is opened once per thread; setkey/setiv run per message.
[[nodiscard]] auto omemo_aes128_gcm_cipher() -> std::expected<gcry_cipher_hd_t, std::string>
{
    thread_local gcry_cipher_hd_t handle = nullptr;
    if (!handle)
    {
        if (gcry_cipher_open(&handle, GCRY_CIPHER_AES, GCRY_CIPHER_MODE_GCM, 0) != 0)
            return std::unexpected("cipher open failed");
    }
    else if (gcry_cipher_reset(handle) != 0)
    {
        return std::unexpected("cipher reset failed");
    }
    return handle;
}

} // namespace

[[maybe_unused]] [[nodiscard]] auto axolotl_omemo_encrypt(std::string_view plaintext) -> std::expected<axolotl_omemo_payload, std::string>
{
    if (plaintext.empty())
        return std::unexpected("empty plaintext");

    axolotl_omemo_payload result;
    auto key_span = std::span(result.key);
    auto iv_span  = std::span(result.iv);
    gcry_randomize(key_span.data(), key_span.size(), GCRY_STRONG_RANDOM);
    gcry_randomize(iv_span.data(), iv_span.size(), GCRY_STRONG_RANDOM);

    auto cipher = omemo_aes128_gcm_cipher();
    if (!cipher)
        return std::unexpected(cipher.error());

    if (gcry_cipher_setkey(*cipher, key_span.data(), key_span.size()) != 0
        || gcry_cipher_setiv(*cipher, iv_span.data(), iv_span.size()) != 0)
    {
        return std::unexpected("cipher key/iv failed");
    }

    std::vector<std::uint8_t> ciphertext(plaintext.size());
    std::span<std::uint8_t> ct_span = ciphertext;
    if (gcry_cipher_encrypt(*cipher, ct_span.data(), ct_span.size(),
                            plaintext.data(), plaintext.size()) != 0)
    {
        return std::unexpected("encrypt failed");
    }

    // Retrieve GCM authentication tag (16 bytes)
    auto authtag_span = std::span(result.authtag);
    if (gcry_cipher_gettag(*cipher, authtag_span.data(), authtag_span.size()) != 0)
        return std::unexpected("gettag failed");

    result.payload = std::move(ciphertext);  // (span not needed after this point)
    return result;
}

// Decrypt a legacy OMEMO payload.  The auth tag is checked internally by GCM.
[[maybe_unused]] [[nodiscard]] auto axolotl_omemo_decrypt(const std::array<std::uint8_t, 16> &key,
                                        const std::array<std::uint8_t, 12> &iv,
                                        const std::array<std::uint8_t, 16> &authtag,
                                        const std::vector<std::uint8_t> &ciphertext)
    -> std::expected<std::string, std::string>
{
    if (ciphertext.empty())
        return std::unexpected("empty ciphertext");

    auto cipher = omemo_aes128_gcm_cipher();
    if (!cipher)
        return std::unexpected(cipher.error());

    auto key_span = std::span(key);
    auto iv_span  = std::span(iv);
    if (gcry_cipher_setkey(*cipher, key_span.data(), key_span.size()) != 0
        || gcry_cipher_setiv(*cipher, iv_span.data(), iv_span.size()) != 0)
    {
        return std::unexpected("cipher key/iv failed");
    }

    std::vector<std::uint8_t> plaintext(ciphertext.size());
    std::span<std::uint8_t> pt_span = plaintext;
    if (gcry_cipher_decrypt(*cipher, pt_span.data(), pt_span.size(),
                            ciphertext.data(), ciphertext.size()) != 0)
    {
        return std::unexpected("decrypt failed");
    }

    // Verify the GCM authentication tag
    auto authtag_span = std::span(authtag);
    if (gcry_cipher_checktag(*cipher, authtag_span.data(), authtag_span.size()) != 0)
    {
        print_error(nullptr, "omemo: legacy OMEMO payload GCM authentication failed");
        return std::unexpected("auth tag failed");
    }

    return std::string(plaintext.begin(), plaintext.end());
}

struct scoped_session_cipher {
    signal_address_view address;
    libsignal::unique_session_cipher cipher;

    [[nodiscard]] static std::optional<scoped_session_cipher> create(omemo &self,
                                                                     std::string_view jid,
                                                                     std::uint32_t device_id)
    {
        scoped_session_cipher scoped;
        scoped.address = make_signal_address(jid, static_cast<std::int32_t>(device_id));
        session_cipher *cipher_raw = nullptr;
        if (session_cipher_create(&cipher_raw, self.store_context, &scoped.address.address, self.context) != 0)
            return std::nullopt;
        scoped.cipher = libsignal::unique_session_cipher {cipher_raw};
        session_cipher_set_version(scoped.cipher.get(), CIPHERTEXT_CURRENT_VERSION);
        return scoped;
    }

    [[nodiscard]] session_cipher *get() const { return cipher.get(); }
};

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

    const omemo_lmdb_write_scope write_scope {self};

    // Per Conversations: Signal-encrypt innerKey(16) || authTag(16) = 32 bytes
    std::array<std::uint8_t, 32> bundle {};
    auto bundle_span = std::span(bundle);
    std::ranges::copy(ep.key, bundle_span.begin());
    std::ranges::copy(ep.authtag, bundle_span.begin() + 16);

    const auto scoped_cipher = scoped_session_cipher::create(self, jid, remote_device_id);
    if (!scoped_cipher)
        return std::nullopt;

    ciphertext_message *message_raw = nullptr;
    if (session_cipher_encrypt(scoped_cipher->get(), bundle_span.data(), bundle_span.size(), &message_raw) != 0)
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
[[maybe_unused]] [[nodiscard]] auto decrypt_axolotl_transport_key(omemo &self, std::string_view jid,
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

    XDEBUG("omemo: (legacy) transport-key decrypt enter {}/{} prekey={} bytes={}",
           jid, remote_device_id, is_prekey, serialized.size());

    const omemo_lmdb_write_scope write_scope {self};

    const auto scoped_cipher = scoped_session_cipher::create(self, jid, remote_device_id);
    if (!scoped_cipher)
    {
        print_error(nullptr,
                    fmt::format("omemo: (legacy) session_cipher_create failed for {}/{}",
                                jid, remote_device_id));
        return std::nullopt;
    }
    session_cipher *cipher = scoped_cipher->get();
    XDEBUG("omemo: (legacy) session_cipher ready for {}/{}", jid, remote_device_id);

    std::uint32_t registration_id = 0;
    signal_protocol_identity_get_local_registration_id(self.store_context, &registration_id);

    signal_buffer *plaintext_raw = nullptr;
    int result = SG_ERR_INVAL;
    std::span<const uint8_t> ser_span = serialized;
    if (is_prekey)
    {
        pre_key_signal_message *message_raw = nullptr;
        int rc = pre_key_signal_message_deserialize_omemo(
            &message_raw, ser_span.data(), ser_span.size(), registration_id, self.context);
        if (rc != 0)
            rc = pre_key_signal_message_deserialize(&message_raw, ser_span.data(), ser_span.size(), self.context);
        if (rc != 0)
        {
            print_error(nullptr,
                        fmt::format("omemo: (legacy) pre_key_signal_message_deserialize failed for {}/{}: rc={}",
                                    jid, remote_device_id, rc));
            return std::nullopt;
        }
        libsignal::object<pre_key_signal_message> message {message_raw};
        XDEBUG("omemo: (legacy) PreKeySignalMessage deserialized for {}/{}", jid, remote_device_id);
        if (out_used_prekey_id && pre_key_signal_message_has_pre_key_id(message.get()))
            *out_used_prekey_id = pre_key_signal_message_get_pre_key_id(message.get());
        // Capture the ratchet counter for heartbeat logic (XEP-0384 §6).
        if (out_message_counter)
        {
            const signal_message *inner_sm = pre_key_signal_message_get_signal_message(message.get());
            if (inner_sm)
                *out_message_counter = signal_message_get_counter(inner_sm);
        }

        signal_message *inner = pre_key_signal_message_get_signal_message(message.get());
        const bool session_exists =
            signal_protocol_session_contains_session(self.store_context,
                                                     &scoped_cipher->address.address) == 1;

        // When a session already exists (typical MAM replay), try the embedded
        // SignalMessage first.  PreKeySignalMessage decrypt can stall or fail on
        // consumed one-time prekeys from archived stanzas.
        if (session_exists && inner)
        {
            XDEBUG("omemo: (legacy) trying inner SignalMessage first for {}/{} (session exists)",
                   jid, remote_device_id);
            result = session_cipher_decrypt_signal_message(cipher, inner, nullptr, &plaintext_raw);
        }

        if (result != 0 && plaintext_raw == nullptr)
        {
            XDEBUG("omemo: (legacy) decrypting PreKeySignalMessage for {}/{}",
                   jid, remote_device_id);
            result = session_cipher_decrypt_pre_key_signal_message(cipher, message.get(), nullptr,
                                                                   &plaintext_raw);
        }

        // Prekey path failed but a session exists — fall back to inner SignalMessage
        // if we did not already try it above.
        if (result != 0 && plaintext_raw == nullptr && inner && !session_exists)
        {
            print_error(nullptr,
                        fmt::format("omemo: (legacy) prekey decrypt failed for {}/{}: rc={}; trying inner SignalMessage",
                                    jid, remote_device_id, result));
            result = session_cipher_decrypt_signal_message(cipher, inner, nullptr, &plaintext_raw);
        }
        else if (result != 0 && plaintext_raw == nullptr && inner && session_exists)
        {
            XDEBUG("omemo: (legacy) inner SignalMessage fallback failed for {}/{}: rc={}",
                   jid, remote_device_id, result);
        }
    }
    else
    {
        signal_message *message_raw = nullptr;
        int rc = signal_message_deserialize_omemo(&message_raw, ser_span.data(), ser_span.size(), self.context);
        if (rc != 0)
            rc = signal_message_deserialize(&message_raw, ser_span.data(), ser_span.size(), self.context);
        if (rc != 0)
        {
            print_error(nullptr,
                        fmt::format("omemo: (legacy) signal_message_deserialize failed for {}/{}: rc={}",
                                    jid, remote_device_id, rc));
            return std::nullopt;
        }
        libsignal::object<signal_message> message {message_raw};
        // Capture the ratchet counter for heartbeat logic (XEP-0384 §6).
        if (out_message_counter)
            *out_message_counter = signal_message_get_counter(message.get());
        result = session_cipher_decrypt_signal_message(cipher, message.get(), nullptr, &plaintext_raw);
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
        print_error(nullptr,
                    fmt::format("omemo: (legacy) session_cipher_decrypt failed for {}/{}: rc={}",
                                jid, remote_device_id, result));
        return std::nullopt;
    }

    std::unique_ptr<signal_buffer, decltype(&signal_buffer_bzero_free)> plaintext(
        plaintext_raw, signal_buffer_bzero_free);
    if (signal_buffer_len(plaintext.get()) != 32)
    {
        print_error(nullptr,
                    fmt::format("omemo: (legacy) decrypted transport key has wrong length {} (expected 32)",
                                signal_buffer_len(plaintext.get())));
        return std::nullopt;
    }

    std::array<std::uint8_t, 16> inner_key {};
    std::array<std::uint8_t, 16> auth_tag {};
    std::ranges::copy_n(signal_buffer_const_data(plaintext.get()), 16, inner_key.begin());
    std::ranges::copy_n(signal_buffer_const_data(plaintext.get()) + 16, 16, auth_tag.begin());
    return std::pair<std::array<std::uint8_t, 16>, std::array<std::uint8_t, 16>> {inner_key, auth_tag};
}

void store_bytes(omemo &self, std::string_view key, const std::uint8_t *data, std::size_t size)
{
    const auto value = std::string_view {reinterpret_cast<const char *>(data), size};
    if (self.lmdb_write_txn_)
    {
        self.dbi.omemo.put(*self.lmdb_write_txn_, key, value);
        return;
    }

    auto transaction = lmdb::txn::begin(self.db_env);
    self.dbi.omemo.put(transaction, key, value);
    transaction.commit();
}

[[nodiscard]] auto load_bytes(omemo &self, std::string_view key) -> std::optional<std::vector<std::uint8_t>>
{
    std::string_view value;
    if (self.lmdb_write_txn_)
    {
        if (!self.dbi.omemo.get(*self.lmdb_write_txn_, key, value))
            return std::nullopt;
        const auto *begin = reinterpret_cast<const std::uint8_t *>(value.data());
        return std::vector<std::uint8_t> {begin, begin + value.size()};
    }
    if (self.lmdb_read_txn_)
    {
        if (!self.dbi.omemo.get(*self.lmdb_read_txn_, key, value))
            return std::nullopt;
        const auto *begin = reinterpret_cast<const std::uint8_t *>(value.data());
        return std::vector<std::uint8_t> {begin, begin + value.size()};
    }

    auto transaction = lmdb::txn::begin(self.db_env, nullptr, MDB_RDONLY);
    if (!self.dbi.omemo.get(transaction, key, value))
        return std::nullopt;
    // Copy while |transaction| is still open — value points into the mmap.
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
