        return std::nullopt;
    unique_gcry_mac mac {mac_raw};

    if (gcry_mac_setkey(mac.get(), key.data(), key.size()) != 0
        || gcry_mac_write(mac.get(), data.data(), data.size()) != 0)
    {
        return std::nullopt;
    }

    std::array<std::uint8_t, 32> output {};
    size_t output_size = output.size();
    if (gcry_mac_read(mac.get(), output.data(), &output_size) != 0 || output_size != output.size())
        return std::nullopt;
    return output;
}

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
    delete static_cast<mac_context *>(hmac_context);
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
    delete static_cast<digest_context *>(digest_context_ptr);
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

[[nodiscard]] auto hkdf_sha256(std::span<const std::uint8_t> ikm,
                               std::span<const std::uint8_t> salt,
                               std::span<const std::uint8_t> info,
                               std::size_t output_size) -> std::optional<std::vector<std::uint8_t>>
{
    const std::array<std::uint8_t, 32> zero_salt {};
    const auto effective_salt = salt.empty() ? std::span<const std::uint8_t>(zero_salt) : salt;
    const auto prk = hmac_sha256(effective_salt, ikm);
    if (!prk)
        return std::nullopt;

    std::vector<std::uint8_t> okm;
    okm.reserve(output_size);
    std::vector<std::uint8_t> previous;
    std::uint8_t counter = 1;

    while (okm.size() < output_size)
    {
        std::vector<std::uint8_t> input;
        input.reserve(previous.size() + info.size() + 1);
        input.insert(input.end(), previous.begin(), previous.end());
        input.insert(input.end(), info.begin(), info.end());
        input.push_back(counter);

        const auto block = hmac_sha256(*prk, input);
        if (!block)
            return std::nullopt;

        previous.assign(block->begin(), block->end());
        const auto to_copy = std::min(previous.size(), output_size - okm.size());
        okm.insert(okm.end(), previous.begin(), previous.begin() + static_cast<std::ptrdiff_t>(to_copy));
        ++counter;
    }

    return okm;
}

[[nodiscard]] auto derive_omemo2_keys(const std::array<std::uint8_t, 32> &key)
    -> std::optional<omemo2_keys>
{
    static_assert(std::tuple_size_v<std::array<std::uint8_t, 32>> == 32);
    static_assert(std::tuple_size_v<decltype(omemo2_keys {}.encryption)> == 32);
    static_assert(std::tuple_size_v<decltype(omemo2_keys {}.authentication)> == 32);
    static_assert(std::tuple_size_v<decltype(omemo2_keys {}.iv)> == 16);

    static constexpr std::array<std::uint8_t, 32> salt {};
    static constexpr std::array<std::uint8_t, 13> info {
        'O','M','E','M','O',' ','P','a','y','l','o','a','d'
    };

    const auto okm = hkdf_sha256(key, salt, info, 80);
    if (!okm || okm->size() != 80)
        return std::nullopt;

    omemo2_keys derived;
    std::copy_n(okm->begin(), 32, derived.encryption.begin());
    std::copy_n(okm->begin() + 32, 32, derived.authentication.begin());
    std::copy_n(okm->begin() + 64, 16, derived.iv.begin());
    return derived;
}

[[nodiscard]] auto omemo2_encrypt(std::string_view plaintext) -> std::optional<omemo2_payload>
{
    omemo2_payload result;
    OMEMO_ASSERT(!plaintext.empty(), "payload encryption requires non-empty plaintext");
    gcry_randomize(result.key.data(), result.key.size(), GCRY_STRONG_RANDOM);

    const auto derived = derive_omemo2_keys(result.key);
    if (!derived)
        return std::nullopt;

    gcry_cipher_hd_t cipher_raw = nullptr;
    if (gcry_cipher_open(&cipher_raw, GCRY_CIPHER_AES256, GCRY_CIPHER_MODE_CBC, 0) != 0)
        return std::nullopt;
    unique_gcry_cipher cipher {cipher_raw};

    if (gcry_cipher_setkey(cipher.get(), derived->encryption.data(), derived->encryption.size()) != 0
        || gcry_cipher_setiv(cipher.get(), derived->iv.data(), derived->iv.size()) != 0)
    {
        return std::nullopt;
    }

    auto padded = pkcs7_pad(plaintext, 16);
    std::vector<std::uint8_t> ciphertext(padded.size());
    if (gcry_cipher_encrypt(cipher.get(), ciphertext.data(), ciphertext.size(),
                            padded.data(), padded.size()) != 0)
    {
        return std::nullopt;
    }

    gcry_mac_hd_t mac_raw = nullptr;
    if (gcry_mac_open(&mac_raw, GCRY_MAC_HMAC_SHA256, 0, nullptr) != 0)
        return std::nullopt;
    unique_gcry_mac mac {mac_raw};

    if (gcry_mac_setkey(mac.get(), derived->authentication.data(), derived->authentication.size()) != 0
        || gcry_mac_write(mac.get(), ciphertext.data(), ciphertext.size()) != 0)
    {
        return std::nullopt;
    }

    // Per XEP-0384 §4.4: transport key bundle = key(32) || truncated_HMAC(16).
    // Store the truncated HMAC in result.hmac; it will be appended to result.key
    // when building the Signal-encrypted transport key bytes in encrypt_transport_key().
    std::array<std::uint8_t, 32> full_mac {};
    size_t full_mac_len = full_mac.size();
    if (gcry_mac_read(mac.get(), full_mac.data(), &full_mac_len) != 0 || full_mac_len < 16)
        return std::nullopt;
    std::copy_n(full_mac.begin(), 16, result.hmac.begin());

    // Payload = ciphertext only (IV is derived from key via HKDF, not transmitted).
    result.payload = std::move(ciphertext);
    return result;
}

[[nodiscard]] auto encrypt_transport_key(omemo &self, std::string_view jid,
                                         std::uint32_t remote_device_id,
                                         const omemo2_payload &ep)
    -> std::optional<std::pair<std::vector<std::uint8_t>, bool>>
{
    OMEMO_ASSERT(self.context, "signal context must be initialized before encrypting transport keys");
    OMEMO_ASSERT(self.store_context, "signal store context must be initialized before encrypting transport keys");
    OMEMO_ASSERT(!jid.empty(), "peer jid must be present when encrypting transport keys");
    OMEMO_ASSERT(remote_device_id != 0, "peer device id must be non-zero when encrypting transport keys");

    // Per XEP-0384 §4.4: Signal-encrypt the bundle key(32) || truncated_HMAC(16) = 48 bytes.
    std::array<std::uint8_t, 48> bundle {};
    std::copy_n(ep.key.begin(), 32, bundle.begin());
    std::copy_n(ep.hmac.begin(), 16, bundle.begin() + 32);

    auto address = make_signal_address(jid, static_cast<std::int32_t>(remote_device_id));

    session_cipher *cipher_raw = nullptr;
    if (session_cipher_create(&cipher_raw, self.store_context, &address.address, self.context) != 0)
        return std::nullopt;
    libsignal::unique_session_cipher cipher {cipher_raw};

    // Serialize transport messages in OMEMO wire format.
    session_cipher_set_version(cipher.get(), CIPHERTEXT_OMEMO_VERSION);

    ciphertext_message *message_raw = nullptr;
    if (session_cipher_encrypt(cipher.get(), bundle.data(), bundle.size(), &message_raw) != 0)
        return std::nullopt;
    libsignal::unique_ciphertext_message message {message_raw};

    // ciphertext_message_get_serialized() returns a non-owning pointer into the
    // message's internal buffer — do NOT wrap in unique_signal_buffer (double-free).
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

// Returns {key(32), hmac(16)} as per XEP-0384 §4.4: the Double Ratchet decrypts
// key(32) || truncated_HMAC(16) = 48 bytes from the <key> element.
[[nodiscard]] auto decrypt_transport_key(omemo &self, std::string_view jid,
                                         std::uint32_t remote_device_id,
                                         const std::vector<std::uint8_t> &serialized,
                                         bool is_prekey,
                                         std::optional<std::uint32_t> *out_used_prekey_id = nullptr)
    -> std::optional<std::pair<std::array<std::uint8_t, 32>, std::array<std::uint8_t, 16>>>
{
    OMEMO_ASSERT(self.context, "signal context must be initialized before decrypting transport keys");
    OMEMO_ASSERT(self.store_context, "signal store context must be initialized before decrypting transport keys");
    OMEMO_ASSERT(!jid.empty(), "peer jid must be present when decrypting transport keys");
    OMEMO_ASSERT(remote_device_id != 0, "peer device id must be non-zero when decrypting transport keys");
    OMEMO_ASSERT(!serialized.empty(), "serialized Signal message must be non-empty");

    auto address = make_signal_address(jid, static_cast<std::int32_t>(remote_device_id));

    session_cipher *cipher_raw = nullptr;
    if (int rc = session_cipher_create(&cipher_raw, self.store_context, &address.address, self.context); rc != 0)
    {
        weechat_printf(nullptr, "%somemo: session_cipher_create failed for %.*s/%u: rc=%d",
                       weechat_prefix("error"),
                       static_cast<int>(jid.size()), jid.data(),
                       remote_device_id, rc);
        return std::nullopt;
    }
    libsignal::unique_session_cipher cipher {cipher_raw};

    // Ensure this cipher expects OMEMO wire-format Signal messages.
    session_cipher_set_version(cipher.get(), CIPHERTEXT_OMEMO_VERSION);

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
            weechat_printf(nullptr, "%somemo: pre_key_signal_message_deserialize failed for %.*s/%u: rc=%d",
                           weechat_prefix("error"),
                           static_cast<int>(jid.size()), jid.data(),
                           remote_device_id, rc);
            return std::nullopt;
        }
        libsignal::object<pre_key_signal_message> message {message_raw};
        // Capture the pre-key ID before decryption so we can replace it afterwards.
        if (out_used_prekey_id && pre_key_signal_message_has_pre_key_id(message.get()))
            *out_used_prekey_id = pre_key_signal_message_get_pre_key_id(message.get());
        result = session_cipher_decrypt_pre_key_signal_message(cipher.get(), message.get(), nullptr, &plaintext_raw);
    }
    else
    {
        signal_message *message_raw = nullptr;
        int rc = signal_message_deserialize_omemo(&message_raw, serialized.data(), serialized.size(), self.context);
        if (rc != 0)
            rc = signal_message_deserialize(&message_raw, serialized.data(), serialized.size(), self.context);
        if (rc != 0)
        {
            weechat_printf(nullptr, "%somemo: signal_message_deserialize failed for %.*s/%u: rc=%d",
                           weechat_prefix("error"),
                           static_cast<int>(jid.size()), jid.data(),
                           remote_device_id, rc);
            return std::nullopt;
        }
        libsignal::object<signal_message> message {message_raw};
        result = session_cipher_decrypt_signal_message(cipher.get(), message.get(), nullptr, &plaintext_raw);
    }

    if (result != 0 || !plaintext_raw)
    {
        weechat_printf(nullptr, "%somemo: session_cipher_decrypt failed for %.*s/%u: rc=%d is_prekey=%d",
                       weechat_prefix("error"),
                       static_cast<int>(jid.size()), jid.data(),
                       remote_device_id, result, (int)is_prekey);
        return std::nullopt;
    }

    std::unique_ptr<signal_buffer, decltype(&signal_buffer_bzero_free)> plaintext(
        plaintext_raw, signal_buffer_bzero_free);
    if (signal_buffer_len(plaintext.get()) != 48)
    {
        weechat_printf(nullptr, "%somemo: decrypted transport key has wrong length %zu (expected 48)",
                       weechat_prefix("error"), signal_buffer_len(plaintext.get()));
        return std::nullopt;
    }

    std::array<std::uint8_t, 32> key {};
    std::array<std::uint8_t, 16> hmac {};
    std::copy_n(signal_buffer_const_data(plaintext.get()), 32, key.begin());
    std::copy_n(signal_buffer_const_data(plaintext.get()) + 32, 16, hmac.begin());
    return std::pair<std::array<std::uint8_t, 32>, std::array<std::uint8_t, 16>> {key, hmac};
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

// Per XEP-0384 §4.5: key is the 32-byte random key; hmac is the 16-byte truncated
// HMAC received alongside the key in the transport key bundle.
// payload is ciphertext only (IV is re-derived from key via HKDF, not transmitted).
[[nodiscard]] auto omemo2_decrypt(const std::array<std::uint8_t, 32> &key,
                                  const std::array<std::uint8_t, 16> &received_hmac,
                                  const std::vector<std::uint8_t> &payload)
    -> std::optional<std::string>
{
    if (payload.empty())
        return std::nullopt;

    const auto derived = derive_omemo2_keys(key);
    if (!derived)
        return std::nullopt;

    // Verify the HMAC of the ciphertext using the authentication key.
    gcry_mac_hd_t mac_raw = nullptr;
    if (gcry_mac_open(&mac_raw, GCRY_MAC_HMAC_SHA256, 0, nullptr) != 0)
        return std::nullopt;
    unique_gcry_mac mac {mac_raw};

    if (gcry_mac_setkey(mac.get(), derived->authentication.data(), derived->authentication.size()) != 0
        || gcry_mac_write(mac.get(), payload.data(), payload.size()) != 0)
    {
        return std::nullopt;
    }

    std::array<std::uint8_t, 32> full_mac {};
    size_t full_mac_len = full_mac.size();
    if (gcry_mac_read(mac.get(), full_mac.data(), &full_mac_len) != 0 || full_mac_len < 16)
        return std::nullopt;
    if (!std::equal(received_hmac.begin(), received_hmac.end(), full_mac.begin()))
    {
        weechat_printf(nullptr, "%somemo: OMEMO payload MAC verification failed",
                       weechat_prefix("error"));
        return std::nullopt;
    }

    gcry_cipher_hd_t cipher_raw = nullptr;
    if (gcry_cipher_open(&cipher_raw, GCRY_CIPHER_AES256, GCRY_CIPHER_MODE_CBC, 0) != 0)
        return std::nullopt;
    unique_gcry_cipher cipher {cipher_raw};

    // IV is derived from the key via HKDF (not transmitted in the payload).
    if (gcry_cipher_setkey(cipher.get(), derived->encryption.data(), derived->encryption.size()) != 0
        || gcry_cipher_setiv(cipher.get(), derived->iv.data(), derived->iv.size()) != 0)
    {
        return std::nullopt;
    }

    std::vector<std::uint8_t> plaintext(payload.begin(), payload.end());
    if (gcry_cipher_decrypt(cipher.get(), plaintext.data(), plaintext.size(),
                            plaintext.data(), plaintext.size()) != 0)
    {
        return std::nullopt;
    }

    return pkcs7_unpad(plaintext);
}

[[nodiscard]] auto sce_unwrap(xmpp_ctx_t *context, std::string_view xml) -> std::optional<std::string>
{
    using stanza_guard = std::unique_ptr<xmpp_stanza_t, decltype(&xmpp_stanza_release)>;
    stanza_guard stanza(xmpp_stanza_new_from_string(context, std::string {xml}.c_str()),
                        xmpp_stanza_release);
    if (!stanza)
        return std::nullopt;

    xmpp_stanza_t *content = xmpp_stanza_get_child_by_name(stanza.get(), "content");
    if (!content)
        return std::nullopt;

    xmpp_stanza_t *body = xmpp_stanza_get_child_by_name_and_ns(content, "body", "jabber:client");
    if (!body)
        return std::nullopt;

    const auto text = stanza_text(body);
    if (text.empty())
        return std::nullopt;
    return text;
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

struct legacy_omemo_payload {
    std::array<std::uint8_t, 16> key {};     // AES-128 key (Signal-encrypted alongside authtag)
    std::array<std::uint8_t, 12> iv {};      // GCM nonce (transmitted in <iv> element in header)
    std::array<std::uint8_t, 16> authtag {}; // GCM auth tag (packed into Signal key, not payload)
    std::vector<std::uint8_t> payload;       // AES-128-GCM ciphertext (auth tag stripped)
};

[[nodiscard]] auto legacy_omemo_encrypt(std::string_view plaintext) -> std::optional<legacy_omemo_payload>
{
    if (plaintext.empty())
        return std::nullopt;

    legacy_omemo_payload result;
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
[[nodiscard]] auto legacy_omemo_decrypt(const std::array<std::uint8_t, 16> &key,
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
[[nodiscard]] auto encrypt_legacy_transport_key(omemo &self, std::string_view jid,
                                                std::uint32_t remote_device_id,
                                                const legacy_omemo_payload &ep)
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
    session_cipher_set_version(cipher.get(), CIPHERTEXT_OMEMO_VERSION);

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
[[nodiscard]] auto decrypt_legacy_transport_key(omemo &self, std::string_view jid,
                                                std::uint32_t remote_device_id,
                                                const std::vector<std::uint8_t> &serialized,
                                                bool is_prekey,
                                                std::optional<std::uint32_t> *out_used_prekey_id = nullptr)
    -> std::optional<std::pair<std::array<std::uint8_t, 16>, std::array<std::uint8_t, 16>>>
{
    OMEMO_ASSERT(self.context, "signal context required");
    OMEMO_ASSERT(self.store_context, "signal store context required");
    OMEMO_ASSERT(!jid.empty(), "peer jid required");
    OMEMO_ASSERT(remote_device_id != 0, "peer device id required");
    OMEMO_ASSERT(!serialized.empty(), "serialized message required");

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
    session_cipher_set_version(cipher.get(), CIPHERTEXT_OMEMO_VERSION);

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
        result = session_cipher_decrypt_pre_key_signal_message(cipher.get(), message.get(), nullptr, &plaintext_raw);
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
        result = session_cipher_decrypt_signal_message(cipher.get(), message.get(), nullptr, &plaintext_raw);
    }

    if (result != 0 || !plaintext_raw)
    {
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
