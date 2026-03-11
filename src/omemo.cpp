// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <algorithm>
#include <cassert>
#include <array>
#include <charconv>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/core.h>
#include <gcrypt.h>
#include <key_helper.h>
#include <lmdb++.h>
#include <signal_protocol.h>
#include <session_builder.h>
#include <session_cipher.h>
#include <session_record.h>
#include <session_pre_key.h>
#include <strophe.h>
#include <weechat/weechat-plugin.h>

#include "account.hh"
#include "gcrypt.hh"
#include "omemo.hh"
#include "plugin.hh"
#include "strophe.hh"
#include "xmpp/ns.hh"
#include "xmpp/stanza.hh"

#ifndef NDEBUG
#define OMEMO_ASSERT(condition, message)                                                     \
    do {                                                                                     \
        if (!(condition)) {                                                                  \
            std::fprintf(stderr, "OMEMO assertion failed: %s (%s:%d): %s\n",             \
                         #condition, __FILE__, __LINE__, message);                           \
            assert(condition);                                                               \
        }                                                                                    \
    } while (0)
#else
#define OMEMO_ASSERT(condition, message) do { (void) sizeof(condition); } while (0)
#endif

namespace {

using namespace weechat::xmpp;

constexpr std::string_view kOmemoNs = "urn:xmpp:omemo:2";
constexpr std::string_view kDevicesNode = "urn:xmpp:omemo:2:devices";
constexpr std::string_view kBundlesNode = "urn:xmpp:omemo:2:bundles";
constexpr std::string_view kDeviceIdKey = "device_id";
constexpr std::string_view kRegistrationIdKey = "registration_id";
constexpr std::string_view kIdentityPublicKey = "identity:public";
constexpr std::string_view kIdentityPrivateKey = "identity:private";
constexpr std::string_view kSignedPreKeyId = "signed_pre_key:id";
constexpr std::string_view kSignedPreKeyRecord = "signed_pre_key:record";
constexpr std::string_view kSignedPreKeyPublic = "signed_pre_key:public";
constexpr std::string_view kSignedPreKeySignature = "signed_pre_key:signature";
constexpr std::string_view kPrekeys = "prekeys";
constexpr std::string_view kOmemoPayloadInfo = "OMEMO Payload";
constexpr std::uint32_t kPreKeyStart = 1;
constexpr std::uint32_t kPreKeyCount = 100;

struct bundle_metadata {
    std::string signed_pre_key_id;
    std::string signed_pre_key;
    std::string signed_pre_key_signature;
    std::string identity_key;
    std::vector<std::pair<std::string, std::string>> prekeys;
};

struct signal_store_state {
    signal_protocol_identity_key_store identity {};
    signal_protocol_pre_key_store pre_key {};
    signal_protocol_signed_pre_key_store signed_pre_key {};
    signal_protocol_session_store session {};
    signal_protocol_sender_key_store sender_key {};
    signal_crypto_provider crypto {};
};

struct signal_address_view {
    std::string name;
    signal_protocol_address address {};
};

std::unordered_map<omemo *, std::unique_ptr<signal_store_state>> g_signal_store_states;

[[nodiscard]] auto make_signal_address(std::string_view jid, std::int32_t device_id)
    -> signal_address_view;

[[nodiscard]] auto pkcs7_unpad(const std::vector<std::uint8_t> &padded) -> std::optional<std::string>;

struct signal_buffer_deleter {
    void operator()(signal_buffer *buffer) const noexcept
    {
        if (buffer)
            signal_buffer_free(buffer);
    }
};

using unique_signal_buffer = std::unique_ptr<signal_buffer, signal_buffer_deleter>;

struct pre_key_list_deleter {
    void operator()(signal_protocol_key_helper_pre_key_list_node *node) const noexcept
    {
        if (node)
            signal_protocol_key_helper_key_list_free(node);
    }
};

using unique_pre_key_list =
    std::unique_ptr<signal_protocol_key_helper_pre_key_list_node, pre_key_list_deleter>;

struct gcry_cipher_deleter {
    void operator()(gcry_cipher_hd_t handle) const noexcept
    {
        if (handle)
            gcry_cipher_close(handle);
    }
};

using unique_gcry_cipher = std::unique_ptr<std::remove_pointer_t<gcry_cipher_hd_t>, gcry_cipher_deleter>;

struct gcry_mac_deleter {
    void operator()(gcry_mac_hd_t handle) const noexcept
    {
        if (handle)
            gcry_mac_close(handle);
    }
};

using unique_gcry_mac = std::unique_ptr<std::remove_pointer_t<gcry_mac_hd_t>, gcry_mac_deleter>;

struct omemo2_payload {
    // 32-byte random message key
    std::array<std::uint8_t, 32> key {};
    // 16-byte truncated HMAC-SHA-256 of ciphertext (transport-key bundle = key || hmac)
    std::array<std::uint8_t, 16> hmac {};
    // AES-256-CBC ciphertext only (IV is derived from key via HKDF, not transmitted)
    std::vector<std::uint8_t> payload;
};

struct omemo2_keys {
    std::array<std::uint8_t, 32> encryption {};
    std::array<std::uint8_t, 32> authentication {};
    std::array<std::uint8_t, 16> iv {};
};

struct free_deleter {
    void operator()(char *ptr) const noexcept
    {
        free(ptr);
    }
};

using c_string = std::unique_ptr<char, free_deleter>;

[[nodiscard]] auto eval_path(const std::string &expression) -> std::string
{
    c_string value {
        weechat_string_eval_expression(expression.c_str(), nullptr, nullptr, nullptr),
    };
    return value ? std::string {value.get()} : std::string {};
}

[[nodiscard]] auto make_db_path(std::string_view account_name) -> std::string
{
    return eval_path(fmt::format("${{weechat_data_dir}}/xmpp/omemo_{}.db", account_name));
}

void request_devicelist(weechat::account &account, std::string_view jid)
{
    xmpp_stanza_t *children[2] = {nullptr, nullptr};
    children[0] = stanza__iq_pubsub_items(*account.context, nullptr, kDevicesNode.data());
    children[0] = stanza__iq_pubsub(*account.context, nullptr, children,
                                    with_noop("http://jabber.org/protocol/pubsub"));

    xmpp_string_guard uuid_g(*account.context, xmpp_uuid_gen(*account.context));
    const char *uuid = uuid_g.ptr;
    children[0] = stanza__iq(*account.context, nullptr, children, nullptr, uuid,
                             std::string {jid}.c_str(), account.jid().data(), "get");
    if (uuid)
        account.omemo.pending_iq_jid[uuid] = std::string {jid};

    account.connection.send(children[0]);
    xmpp_stanza_release(children[0]);
}

void request_bundle(weechat::account &account, std::string_view jid, std::uint32_t device_id)
{
    const auto key = std::make_pair(std::string {jid}, device_id);
    if (account.omemo.pending_bundle_fetch.count(key))
        return;

    const auto item_id = fmt::format("{}", device_id);
    xmpp_stanza_t *item_stanza =
        stanza__iq_pubsub_items_item(*account.context, nullptr, with_noop(item_id.c_str()));
    // item must be a child of items, not a sibling: <items><item id="N"/></items>
    xmpp_stanza_t *items_stanza =
        stanza__iq_pubsub_items(*account.context, nullptr, kBundlesNode.data());
    xmpp_stanza_add_child(items_stanza, item_stanza);
    xmpp_stanza_release(item_stanza);
    xmpp_stanza_t *pubsub_children[] = {items_stanza, nullptr};
    xmpp_stanza_t *pubsub_stanza =
        stanza__iq_pubsub(*account.context, nullptr, pubsub_children,
                          with_noop("http://jabber.org/protocol/pubsub"));

    xmpp_string_guard uuid_g(*account.context, xmpp_uuid_gen(*account.context));
    const char *uuid = uuid_g.ptr;
    xmpp_stanza_t *iq_children[] = {pubsub_stanza, nullptr};
    xmpp_stanza_t *iq_stanza =
        stanza__iq(*account.context, nullptr, iq_children, nullptr, uuid,
                   std::string {jid}.c_str(), account.jid().data(), "get");
    if (uuid)
        account.omemo.pending_iq_jid[uuid] = std::string {jid};
    account.omemo.pending_bundle_fetch.insert(key);

    account.connection.send(iq_stanza);
    xmpp_stanza_release(iq_stanza);
}

void print_info(t_gui_buffer *buffer, std::string_view message)
{
    weechat_printf(buffer, "%s%s", weechat_prefix("network"), std::string {message}.c_str());
}

void print_error(t_gui_buffer *buffer, std::string_view message)
{
    weechat_printf(buffer, "%s%s", weechat_prefix("error"), std::string {message}.c_str());
}

[[nodiscard]] auto split(std::string_view input, char separator) -> std::vector<std::string>
{
    std::vector<std::string> parts;
    std::string current;

    for (const char ch : input)
    {
        if (ch == separator)
        {
            if (!current.empty())
            {
                parts.push_back(current);
                current.clear();
            }
            continue;
        }

        current.push_back(ch);
    }

    if (!current.empty())
        parts.push_back(current);

    return parts;
}

[[nodiscard]] auto join(const std::vector<std::string> &values, std::string_view separator) -> std::string
{
    std::ostringstream stream;

    for (std::size_t index = 0; index < values.size(); ++index)
    {
        if (index != 0)
            stream << separator;
        stream << values[index];
    }

    return stream.str();
}

[[nodiscard]] auto random_device_id() -> std::uint32_t
{
    std::random_device random_device;
    std::mt19937 generator {random_device()};
    std::uniform_int_distribution<std::uint32_t> distribution {1, 0x7fffffffU};
    return distribution(generator);
}

[[nodiscard]] auto parse_uint32(std::string_view value) -> std::optional<std::uint32_t>
{
    std::uint32_t parsed = 0;
    const auto *begin = value.data();
    const auto *end = value.data() + value.size();
    const auto [ptr, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc {} || ptr != end)
        return std::nullopt;
    return parsed;
}

[[nodiscard]] auto key_for_devicelist(std::string_view jid) -> std::string
{
    return fmt::format("devicelist:{}", jid);
}

[[nodiscard]] auto key_for_bundle(std::string_view jid, std::uint32_t device_id) -> std::string
{
    return fmt::format("bundle:{}:{}", jid, device_id);
}

[[nodiscard]] auto key_for_session(std::string_view jid, std::uint32_t device_id) -> std::string
{
    return fmt::format("session:{}:{}", jid, device_id);
}

[[nodiscard]] auto key_for_identity(std::string_view jid, std::int32_t device_id) -> std::string
{
    return fmt::format("identity:{}:{}", jid, device_id);
}

[[nodiscard]] auto key_for_prekey_record(std::uint32_t id) -> std::string
{
    return fmt::format("prekey:{}", id);
}

[[nodiscard]] auto key_for_signed_prekey_record(std::uint32_t id) -> std::string
{
    return fmt::format("signed-prekey:{}", id);
}

[[nodiscard]] auto key_for_sender_key(std::string_view group,
                                      std::string_view jid,
                                      std::int32_t device_id) -> std::string
{
    return fmt::format("senderkey:{}:{}:{}", group, jid, device_id);
}

[[nodiscard]] auto stanza_text(xmpp_stanza_t *stanza) -> std::string
{
    if (!stanza)
        return {};

    xmpp_string_guard text {xmpp_stanza_get_context(stanza), xmpp_stanza_get_text(stanza)};
    return text ? text.str() : std::string {};
}

[[nodiscard]] auto base64_encode(xmpp_ctx_t *context, const unsigned char *data, std::size_t size)
    -> std::string
{
    xmpp_string_guard encoded {context, xmpp_base64_encode(context, data, size)};
    return encoded ? encoded.str() : std::string {};
}

[[nodiscard]] auto base64_encode_raw(const std::uint8_t *data, std::size_t size) -> std::string
{
    if (!data || size == 0)
        return {};

    const int encoded_size = 4 * static_cast<int>((size + 2) / 3) + 1;
    std::string encoded(static_cast<std::size_t>(encoded_size), '\0');
    const int written = weechat_string_base_encode("64",
        reinterpret_cast<const char *>(data), static_cast<int>(size), encoded.data());
    if (written <= 0)
        return {};
    encoded.resize(static_cast<std::size_t>(written));
    return encoded;
}

[[nodiscard]] auto base64_decode(xmpp_ctx_t *context, std::string_view encoded)
    -> std::vector<std::uint8_t>
{
    unsigned char *decoded = nullptr;
    size_t decoded_size = 0;
    xmpp_base64_decode_bin(context, encoded.data(), encoded.size(), &decoded, &decoded_size);
    if (!decoded || decoded_size == 0)
        return {};

    std::vector<std::uint8_t> result(decoded, decoded + decoded_size);
    xmpp_free(context, decoded);
    return result;
}

[[nodiscard]] auto utc_timestamp_now() -> std::string
{
    const std::time_t now = std::time(nullptr);
    std::tm utc {};
    gmtime_r(&now, &utc);

    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

[[nodiscard]] auto xml_escape(std::string_view text) -> std::string
{
    std::string escaped;
    escaped.reserve(text.size());
    for (const char ch : text)
    {
        switch (ch)
        {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '\"': escaped += "&quot;"; break;
            case '\'': escaped += "&apos;"; break;
            default: escaped.push_back(ch); break;
        }
    }
    return escaped;
}

[[nodiscard]] auto sce_wrap(xmpp_ctx_t *context, weechat::account &account,
                            std::string_view plaintext) -> std::string
{
    const char *bound = xmpp_conn_get_bound_jid(account.connection);
    xmpp_string_guard bare_guard {context, bound ? xmpp_jid_bare(context, bound) : nullptr};
    const std::string from = bare_guard ? bare_guard.str() : std::string {};

    return fmt::format(
        "<envelope xmlns='urn:xmpp:sce:1'><content><body xmlns='jabber:client'>{}</body></content><time stamp='{}'/><from jid='{}'/><rpad/></envelope>",
        xml_escape(plaintext),
        utc_timestamp_now(),
        xml_escape(from));
}

[[nodiscard]] auto pkcs7_pad(std::string_view plaintext, std::size_t block_size)
    -> std::vector<std::uint8_t>
{
    std::vector<std::uint8_t> output(plaintext.begin(), plaintext.end());
    const std::size_t actual_padding = (output.size() % block_size == 0) ? block_size : (block_size - (output.size() % block_size));
    output.insert(output.end(), actual_padding, static_cast<std::uint8_t>(actual_padding));
    return output;
}

[[nodiscard]] auto hmac_sha256(std::span<const std::uint8_t> key,
                               std::span<const std::uint8_t> data) -> std::optional<std::array<std::uint8_t, 32>>
{
    gcry_mac_hd_t mac_raw = nullptr;
    if (gcry_mac_open(&mac_raw, GCRY_MAC_HMAC_SHA256, 0, nullptr) != 0)
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

    signal_buffer *plaintext_raw = nullptr;
    int result = SG_ERR_INVAL;
    if (is_prekey)
    {
        pre_key_signal_message *message_raw = nullptr;
        if (int rc = pre_key_signal_message_deserialize(&message_raw, serialized.data(), serialized.size(), self.context); rc != 0)
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
        if (int rc = signal_message_deserialize(&message_raw, serialized.data(), serialized.size(), self.context); rc != 0)
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

    for (const auto &entry : split(*prekeys_serialized, ';'))
    {
        const auto separator = entry.find(',');
        if (separator == std::string::npos)
            continue;
        bundle.prekeys.emplace_back(entry.substr(0, separator), entry.substr(separator + 1));
    }

    if (bundle.prekeys.empty())
        return std::nullopt;

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
        libsignal::public_key public_key(public_data->data(), public_data->size(), self.context);
        libsignal::private_key private_key(private_data->data(), private_data->size(), self.context);
        self.identity = libsignal::identity_key_pair(*public_key, *private_key);
        return;
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

void ensure_prekeys(omemo &self, xmpp_ctx_t *context)
{
    OMEMO_ASSERT(self.context, "signal context must exist before generating prekeys");
    OMEMO_ASSERT(context != nullptr, "xmpp context must exist before serializing prekeys");

    if (load_string(self, kSignedPreKeyRecord) && load_string(self, kPrekeys))
        return;

    session_signed_pre_key *signed_pre_key_raw = nullptr;
    const auto now_ms = static_cast<std::uint64_t>(std::time(nullptr)) * 1000ULL;
    if (signal_protocol_key_helper_generate_signed_pre_key(
            &signed_pre_key_raw, self.identity, 1, now_ms, self.context) != 0)
    {
        return;
    }

    libsignal::object<session_signed_pre_key> signed_pre_key {signed_pre_key_raw};
    signal_buffer *signed_pre_key_record_raw = nullptr;
    if (session_signed_pre_key_serialize(&signed_pre_key_record_raw, signed_pre_key.get()) != 0)
        return;

    unique_signal_buffer signed_pre_key_record {signed_pre_key_record_raw};
    ec_key_pair *signed_pre_key_pair = session_signed_pre_key_get_key_pair(signed_pre_key.get());
    ec_public_key *signed_pre_key_public = ec_key_pair_get_public(signed_pre_key_pair);

    signal_buffer *signed_pre_key_public_raw = nullptr;
    if (ec_public_key_serialize(&signed_pre_key_public_raw, signed_pre_key_public) != 0)
        return;
    unique_signal_buffer signed_pre_key_public_buffer {signed_pre_key_public_raw};

    store_string(self, kSignedPreKeyId, "1");
    store_bytes(self,
                kSignedPreKeyRecord,
                signal_buffer_const_data(signed_pre_key_record.get()),
                signal_buffer_len(signed_pre_key_record.get()));
    store_bytes(self,
                key_for_signed_prekey_record(1),
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

    signal_protocol_key_helper_pre_key_list_node *pre_key_head_raw = nullptr;
    if (signal_protocol_key_helper_generate_pre_keys(
            &pre_key_head_raw, kPreKeyStart, kPreKeyCount, self.context) != 0)
    {
        return;
    }

    unique_pre_key_list pre_key_head {pre_key_head_raw};
    std::vector<std::string> serialized_prekeys;
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

        serialized_prekeys.push_back(fmt::format(
            "{},{}",
            session_pre_key_get_id(pre_key),
            serialize_public_key(context, pre_key_public)));
    }

    store_string(self, kPrekeys, join(serialized_prekeys, ";"));
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

[[nodiscard]] auto deserialize_public_key(std::string_view encoded, xmpp_ctx_t *context,
                                          signal_context *signal_context_ptr)
    -> std::optional<libsignal::public_key>
{
    unsigned char *decoded = nullptr;
    size_t decoded_size = 0;
    xmpp_base64_decode_bin(context, encoded.data(), encoded.size(), &decoded, &decoded_size);
    if (!decoded || decoded_size == 0)
        return std::nullopt;

    struct xmpp_bin_guard {
        xmpp_ctx_t *context;
        unsigned char *data;
        ~xmpp_bin_guard() { if (data) xmpp_free(context, data); }
    } guard {context, decoded};

    return libsignal::public_key(decoded, decoded_size, signal_context_ptr);
}

[[nodiscard]] auto establish_session_from_bundle(omemo &self, xmpp_ctx_t *context,
                                                 std::string_view jid,
                                                 std::uint32_t remote_device_id)
    -> bool
{
    OMEMO_ASSERT(self.context, "signal context must exist before building a session from a bundle");
    OMEMO_ASSERT(self.store_context, "signal store context must exist before building a session from a bundle");
    OMEMO_ASSERT(context != nullptr, "xmpp context must exist before decoding bundle keys");
    OMEMO_ASSERT(!jid.empty(), "peer jid must be present when building a session from a bundle");
    OMEMO_ASSERT(remote_device_id != 0, "peer device id must be non-zero when building a session from a bundle");

    const auto bundle = load_bundle(self, jid, remote_device_id);
    if (!bundle || bundle->prekeys.empty())
        return false;

    const auto signed_pre_key_id = parse_uint32(bundle->signed_pre_key_id).value_or(0);
    const auto pre_key_id = parse_uint32(bundle->prekeys.front().first).value_or(0);
    if (signed_pre_key_id == 0 || pre_key_id == 0)
        return false;

    auto identity_key = deserialize_public_key(bundle->identity_key, context, self.context);
    auto signed_pre_key = deserialize_public_key(bundle->signed_pre_key, context, self.context);
    auto one_time_pre_key = deserialize_public_key(bundle->prekeys.front().second, context, self.context);
    if (!identity_key || !signed_pre_key || !one_time_pre_key)
        return false;

    auto signature = base64_decode(context, bundle->signed_pre_key_signature);
    if (signature.empty())
        return false;

    auto address = make_signal_address(jid, static_cast<std::int32_t>(remote_device_id));
    constexpr std::uint32_t fallback_registration_id = 1;
    libsignal::pre_key_bundle pre_key_bundle(
        fallback_registration_id,
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
    catch (const std::exception &)
    {
        return false;
    }
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

    store_bytes(*self, key_for_identity(signal_address_name(address), address->device_id), key_data, key_len);
    return SG_SUCCESS;
}

int identity_is_trusted(const signal_protocol_address *address, std::uint8_t *key_data,
                        std::size_t key_len, void *user_data)
{
    auto *self = static_cast<omemo *>(user_data);
    if (!self || !address || !key_data)
        return SG_ERR_INVAL;

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
    {
        transaction.commit();
        return SG_SUCCESS;
    }

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
        return device_ids;

    xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(items, "item");
    if (!item)
        return device_ids;

    xmpp_stanza_t *devices = xmpp_stanza_get_child_by_name_and_ns(
        item, "devices", kOmemoNs.data());
    if (!devices)
        return device_ids;

    for (xmpp_stanza_t *device = xmpp_stanza_get_children(devices);
         device;
         device = xmpp_stanza_get_next(device))
    {
        const char *name = xmpp_stanza_get_name(device);
        if (!name || weechat_strcasecmp(name, "device") != 0)
            continue;

        const char *id = xmpp_stanza_get_id(device);
        if (!id || !*id)
            continue;

        const auto parsed_id = parse_uint32(id);
        if (!parsed_id || *parsed_id == 0)
            continue;

        device_ids.emplace_back(id);
    }

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
        const char *name = xmpp_stanza_get_name(child);
        if (!name)
            continue;

        if (weechat_strcasecmp(name, "spk") == 0)
        {
            if (const char *id = xmpp_stanza_get_attribute(child, "id"))
                bundle.signed_pre_key_id = id;
            bundle.signed_pre_key = stanza_text(child);
        }
        else if (weechat_strcasecmp(name, "spks") == 0)
        {
            bundle.signed_pre_key_signature = stanza_text(child);
        }
        else if (weechat_strcasecmp(name, "ik") == 0)
        {
            bundle.identity_key = stanza_text(child);
        }
        else if (weechat_strcasecmp(name, "prekeys") == 0)
        {
            for (xmpp_stanza_t *prekey = xmpp_stanza_get_children(child);
                 prekey;
                 prekey = xmpp_stanza_get_next(prekey))
            {
                const char *prekey_name = xmpp_stanza_get_name(prekey);
                if (!prekey_name || weechat_strcasecmp(prekey_name, "pk") != 0)
                    continue;

                const char *id = xmpp_stanza_get_attribute(prekey, "id");
                if (!id)
                    continue;

                bundle.prekeys.emplace_back(id, stanza_text(prekey));
            }
        }
    }

    if (bundle.signed_pre_key_id.empty() || bundle.signed_pre_key.empty()
        || bundle.signed_pre_key_signature.empty() || bundle.identity_key.empty())
    {
        return std::nullopt;
    }

    return bundle;
}

void signal_log_emit(int level, const char *message, std::size_t length, void *user_data)
{
    (void) user_data;

    const char *prefix = weechat_prefix(level <= SG_LOG_WARNING ? "error" : "network");
    std::string text {message, length};
    weechat_printf(nullptr, "%somemo: %s", prefix, text.c_str());
}

} // namespace

const char *OMEMO_ADVICE = "[OMEMO encrypted message (XEP-0384)]";

weechat::xmpp::omemo::~omemo()
{
    g_signal_store_states.erase(this);
    // Explicit reset order: store_context before identity (store may reference
    // identity internally), then context last (used by all signal objects).
    // This is belt-and-suspenders over the compiler-generated reverse-declaration
    // order which would destroy identity before store_context.
    store_context = {};
    identity = {};
    context = {};
}

xmpp_stanza_t *weechat::xmpp::omemo::get_bundle(xmpp_ctx_t *context, char *from, char *to)
{
    (void) from;
    (void) to;

    OMEMO_ASSERT(context != nullptr, "xmpp context must be present when publishing our OMEMO bundle");

    if (!*this)
        return nullptr;

    ensure_local_identity(*this);
    ensure_registration_id(*this);
    ensure_prekeys(*this, context);

    const auto bundle = make_local_bundle_metadata(*this);
    if (!bundle)
        return nullptr;

    xmpp_stanza_t *text = nullptr;
    xmpp_stanza_t *spk = nullptr;
    xmpp_stanza_t *spks = nullptr;
    xmpp_stanza_t *ik = nullptr;
    xmpp_stanza_t *prekeys = nullptr;
    xmpp_stanza_t *bundle_stanza = nullptr;
    xmpp_stanza_t *item = nullptr;
    xmpp_stanza_t *publish = nullptr;
    xmpp_stanza_t *pubsub = nullptr;
    xmpp_stanza_t *iq = nullptr;

    text = xmpp_stanza_new(context);
    xmpp_stanza_set_text(text, bundle->signed_pre_key.c_str());
    xmpp_stanza_t *spk_children[] = {text, nullptr};
    spk = stanza__iq_pubsub_publish_item_bundle_signedPreKeyPublic(
        context, nullptr, spk_children, with_noop(bundle->signed_pre_key_id.c_str()));

    text = xmpp_stanza_new(context);
    xmpp_stanza_set_text(text, bundle->signed_pre_key_signature.c_str());
    xmpp_stanza_t *spks_children[] = {text, nullptr};
    spks = stanza__iq_pubsub_publish_item_bundle_signedPreKeySignature(context, nullptr, spks_children);

    text = xmpp_stanza_new(context);
    xmpp_stanza_set_text(text, bundle->identity_key.c_str());
    xmpp_stanza_t *ik_children[] = {text, nullptr};
    ik = stanza__iq_pubsub_publish_item_bundle_identityKey(context, nullptr, ik_children);

    std::vector<xmpp_stanza_t *> prekey_stanzas;
    prekey_stanzas.reserve(bundle->prekeys.size() + 1);
    for (const auto &[id, key] : bundle->prekeys)
    {
        text = xmpp_stanza_new(context);
        xmpp_stanza_set_text(text, key.c_str());
        xmpp_stanza_t *pk_children[] = {text, nullptr};
        prekey_stanzas.push_back(
            stanza__iq_pubsub_publish_item_bundle_prekeys_preKeyPublic(
                context, nullptr, pk_children, with_noop(id.c_str())));
    }
    prekey_stanzas.push_back(nullptr);
    prekeys = stanza__iq_pubsub_publish_item_bundle_prekeys(context, nullptr, prekey_stanzas.data());

    xmpp_stanza_t *bundle_children[] = {spk, spks, ik, prekeys, nullptr};
    bundle_stanza = stanza__iq_pubsub_publish_item_bundle(context, nullptr, bundle_children,
                                                          with_noop(kOmemoNs.data()));
    xmpp_stanza_t *item_children[] = {bundle_stanza, nullptr};
    const auto item_id = fmt::format("{}", device_id);
    item = stanza__iq_pubsub_publish_item(context, nullptr, item_children, with_noop(item_id.c_str()));
    xmpp_stanza_t *publish_children[] = {item, nullptr};
    publish = stanza__iq_pubsub_publish(context, nullptr, publish_children, with_noop(kBundlesNode.data()));
    xmpp_stanza_t *pubsub_children[] = {publish, nullptr};
    pubsub = stanza__iq_pubsub(context, nullptr, pubsub_children, with_noop("http://jabber.org/protocol/pubsub"));

    // Add publish-options (access_model=open) so the server allows contacts
    // to fetch our bundle.  Without this some servers default to a restricted
    // access model and silently reject the publish or make the bundle invisible.
    {
        auto make_field = [&](const char *var, const char *val, const char *type_attr = nullptr) {
            xmpp_stanza_t *field = xmpp_stanza_new(context);
            xmpp_stanza_set_name(field, "field");
            xmpp_stanza_set_attribute(field, "var", var);
            if (type_attr) xmpp_stanza_set_attribute(field, "type", type_attr);
            xmpp_stanza_t *value = xmpp_stanza_new(context);
            xmpp_stanza_set_name(value, "value");
            xmpp_stanza_t *txt = xmpp_stanza_new(context);
            xmpp_stanza_set_text(txt, val);
            xmpp_stanza_add_child(value, txt);
            xmpp_stanza_release(txt);
            xmpp_stanza_add_child(field, value);
            xmpp_stanza_release(value);
            return field;
        };

        xmpp_stanza_t *x = xmpp_stanza_new(context);
        xmpp_stanza_set_name(x, "x");
        xmpp_stanza_set_ns(x, "jabber:x:data");
        xmpp_stanza_set_attribute(x, "type", "submit");

        xmpp_stanza_t *f1 = make_field("FORM_TYPE",
            "http://jabber.org/protocol/pubsub#publish-options", "hidden");
        xmpp_stanza_t *f2 = make_field("pubsub#max_items", "max");
        xmpp_stanza_t *f3 = make_field("pubsub#access_model", "open");
        xmpp_stanza_t *f4 = make_field("pubsub#persist_items", "true");

        xmpp_stanza_add_child(x, f1); xmpp_stanza_release(f1);
        xmpp_stanza_add_child(x, f2); xmpp_stanza_release(f2);
        xmpp_stanza_add_child(x, f3); xmpp_stanza_release(f3);
        xmpp_stanza_add_child(x, f4); xmpp_stanza_release(f4);

        xmpp_stanza_t *publish_options = xmpp_stanza_new(context);
        xmpp_stanza_set_name(publish_options, "publish-options");
        xmpp_stanza_add_child(publish_options, x);
        xmpp_stanza_release(x);

        xmpp_stanza_add_child(pubsub, publish_options);
        xmpp_stanza_release(publish_options);
    }

    xmpp_stanza_t *iq_children[] = {pubsub, nullptr};
    iq = stanza__iq(context, nullptr, iq_children, nullptr, "omemo-bundle", from, to, "set");

    return iq;
}

void weechat::xmpp::omemo::init(struct t_gui_buffer *buffer, const char *account_name)
{
    try
    {
        OMEMO_ASSERT(account_name != nullptr, "OMEMO init requires a non-null account name");

        // Clear in-flight state from any previous session to prevent stale
        // pending_bundle_fetch entries blocking bundle re-fetches on reconnect.
        pending_bundle_fetch.clear();
        pending_key_transport.clear();
        pending_iq_jid.clear();
        pending_configure_retry.clear();

        gcrypt::check_version();

        db_path = make_db_path(account_name ? account_name : "default");
        ensure_db_open(*this);

        if (const auto stored_device_id = load_string(*this, kDeviceIdKey))
        {
            device_id = parse_uint32(*stored_device_id).value_or(random_device_id());
        }
        else
        {
            device_id = random_device_id();
            store_string(*this, kDeviceIdKey, fmt::format("{}", device_id));
        }

        context.create(nullptr);
        context.set_log_function(signal_log_emit);
        store_context.create(context);

        auto store_state = std::make_unique<signal_store_state>();

        store_state->crypto.random_func = crypto_random;
        store_state->crypto.hmac_sha256_init_func = crypto_hmac_sha256_init;
        store_state->crypto.hmac_sha256_update_func = crypto_hmac_sha256_update;
        store_state->crypto.hmac_sha256_final_func = crypto_hmac_sha256_final;
        store_state->crypto.hmac_sha256_cleanup_func = crypto_hmac_sha256_cleanup;
        store_state->crypto.sha512_digest_init_func = crypto_sha512_init;
        store_state->crypto.sha512_digest_update_func = crypto_sha512_update;
        store_state->crypto.sha512_digest_final_func = crypto_sha512_final;
        store_state->crypto.sha512_digest_cleanup_func = crypto_sha512_cleanup;
        store_state->crypto.encrypt_func = crypto_encrypt;
        store_state->crypto.decrypt_func = crypto_decrypt;
        store_state->crypto.user_data = this;

        context.set_crypto_provider(&store_state->crypto);
        context.set_locking_functions(crypto_lock, crypto_unlock);

        store_state->identity.get_identity_key_pair = identity_get_key_pair;
        store_state->identity.get_local_registration_id = identity_get_local_registration_id;
        store_state->identity.save_identity = identity_save;
        store_state->identity.is_trusted_identity = identity_is_trusted;
        store_state->identity.user_data = this;

        store_state->pre_key.load_pre_key = pre_key_load;
        store_state->pre_key.store_pre_key = pre_key_store_record;
        store_state->pre_key.contains_pre_key = pre_key_contains;
        store_state->pre_key.remove_pre_key = pre_key_remove;
        store_state->pre_key.user_data = this;

        store_state->signed_pre_key.load_signed_pre_key = signed_pre_key_load;
        store_state->signed_pre_key.store_signed_pre_key = signed_pre_key_store_record;
        store_state->signed_pre_key.contains_signed_pre_key = signed_pre_key_contains;
        store_state->signed_pre_key.remove_signed_pre_key = signed_pre_key_remove;
        store_state->signed_pre_key.user_data = this;

        store_state->session.load_session_func = session_load;
        store_state->session.get_sub_device_sessions_func = session_get_sub_devices;
        store_state->session.store_session_func = session_store_record;
        store_state->session.contains_session_func = session_contains;
        store_state->session.delete_session_func = session_delete;
        store_state->session.delete_all_sessions_func = session_delete_all;
        store_state->session.user_data = this;

        store_state->sender_key.store_sender_key = sender_key_store_record;
        store_state->sender_key.load_sender_key = sender_key_load;
        store_state->sender_key.user_data = this;

        store_context.set_identity_key_store(&store_state->identity);
        store_context.set_pre_key_store(&store_state->pre_key);
        store_context.set_signed_pre_key_store(&store_state->signed_pre_key);
        store_context.set_session_store(&store_state->session);
        store_context.set_sender_key_store(&store_state->sender_key);
        g_signal_store_states[this] = std::move(store_state);

        ensure_local_identity(*this);
        ensure_registration_id(*this);

        print_info(buffer, fmt::format(
            "OMEMO initialized for account '{}' (device {}).",
            account_name ? account_name : "?", device_id));
    }
    catch (const std::exception &exception)
    {
        context = {};
        store_context = {};
        identity = {};
        device_id = 0;
        db_env = nullptr;
        dbi.omemo = 0;
        print_error(buffer, fmt::format("OMEMO init failed: {}", exception.what()));
    }
}

void weechat::xmpp::omemo::request_devicelist(weechat::account &account, std::string_view jid)
{
    ::request_devicelist(account, jid);
}

void weechat::xmpp::omemo::handle_devicelist(const char *jid, xmpp_stanza_t *items)
{
    if (!db_env || !jid)
        return;

    const auto devices = extract_devices_from_items(items);
    store_string(*this, key_for_devicelist(jid), join(devices, ";"));
}

// Build and send an OMEMO:2 KeyTransportElement to `peer_jid` for `device_id`.
// A key-transport message establishes the Signal session from our side without
// sending any plaintext body.  Per XEP-0384 §7.3, it is a <message> stanza
// carrying <encrypted> with a <header> (keys) but NO <payload> element.
// This allows the remote party to learn our device_id and start encrypting
// future messages to us.
static void send_key_transport(omemo &self,
                               weechat::account &account,
                               struct t_gui_buffer *buffer,
                               const char *peer_jid,
                               std::uint32_t remote_device_id)
{
    if (!self || !peer_jid)
        return;

    // Generate a fresh random 48-byte transport key bundle (key32 || hmac16).
    // We use omemo2_encrypt on a dummy single-byte plaintext just to get a
    // properly-derived key; the resulting payload is discarded.
    const auto ep = omemo2_encrypt(std::string_view("\x00", 1));
    if (!ep)
    {
        print_error(buffer, fmt::format(
            "OMEMO: key-transport key generation failed for {}/{}",
            peer_jid, remote_device_id));
        return;
    }

    const auto transport = encrypt_transport_key(self, peer_jid, remote_device_id, *ep);
    if (!transport)
    {
        print_error(buffer, fmt::format(
            "OMEMO: key-transport encrypt failed for {}/{}",
            peer_jid, remote_device_id));
        return;
    }

    const auto encoded_transport = base64_encode(*account.context,
                                                 transport->first.data(),
                                                 transport->first.size());

    // Build <encrypted xmlns='urn:xmpp:omemo:2'>
    xmpp_stanza_t *encrypted = xmpp_stanza_new(*account.context);
    xmpp_stanza_set_name(encrypted, "encrypted");
    xmpp_stanza_set_ns(encrypted, kOmemoNs.data());

    // <header sid='our_device_id'>
    xmpp_stanza_t *header = xmpp_stanza_new(*account.context);
    xmpp_stanza_set_name(header, "header");
    xmpp_stanza_set_attribute(header, "sid", fmt::format("{}", self.device_id).c_str());

    // <keys jid='peer_jid'><key rid='remote_device_id' [kex='true']>...</key></keys>
    xmpp_stanza_t *keys = xmpp_stanza_new(*account.context);
    xmpp_stanza_set_name(keys, "keys");
    xmpp_stanza_set_attribute(keys, "jid", peer_jid);

    xmpp_stanza_t *key_elem = xmpp_stanza_new(*account.context);
    xmpp_stanza_set_name(key_elem, "key");
    xmpp_stanza_set_attribute(key_elem, "rid", fmt::format("{}", remote_device_id).c_str());
    if (transport->second)
        xmpp_stanza_set_attribute(key_elem, "kex", "true");

    xmpp_stanza_t *key_text = xmpp_stanza_new(*account.context);
    xmpp_stanza_set_text(key_text, encoded_transport.c_str());
    xmpp_stanza_add_child(key_elem, key_text);
    xmpp_stanza_release(key_text);

    xmpp_stanza_add_child(keys, key_elem);
    xmpp_stanza_release(key_elem);
    xmpp_stanza_add_child(header, keys);
    xmpp_stanza_release(keys);
    xmpp_stanza_add_child(encrypted, header);
    xmpp_stanza_release(header);

    // Wrap in a <message type='chat' to='peer_jid'>
    xmpp_stanza_t *message = xmpp_message_new(*account.context, "chat", peer_jid, nullptr);
    xmpp_stanza_add_child(message, encrypted);
    xmpp_stanza_release(encrypted);

    // Add <store xmlns='urn:xmpp:hints'/> so the server archives it and
    // Conversations can pick up our session even if it's offline.
    xmpp_stanza_t *store_hint = xmpp_stanza_new(*account.context);
    xmpp_stanza_set_name(store_hint, "store");
    xmpp_stanza_set_ns(store_hint, "urn:xmpp:hints");
    xmpp_stanza_add_child(message, store_hint);
    xmpp_stanza_release(store_hint);

    print_info(buffer, fmt::format(
        "OMEMO: sending key-transport to {}/{} (kex={})",
        peer_jid, remote_device_id, transport->second ? "true" : "false"));

    account.connection.send(message);
    xmpp_stanza_release(message);
}

void weechat::xmpp::omemo::handle_bundle(weechat::account *account,
                                         struct t_gui_buffer *buffer,
                                         const char *jid,
                                         std::uint32_t remote_device_id,
                                         xmpp_stanza_t *items)
{
    pending_bundle_fetch.erase({jid ? jid : "", remote_device_id});
    const bool needs_key_transport = pending_key_transport.count({jid ? jid : "", remote_device_id}) > 0;
    pending_key_transport.erase({jid ? jid : "", remote_device_id});

    // Do not attempt X3DH session establishment with our own device.
    // Building a session with ourselves would fail in libsignal (we can't
    // be both initiator and responder), and the resulting exception thrown
    // through a C libstrophe callback would corrupt the stack.
    const bool is_own_device = (account && jid && account->jid() == jid)
                                && (remote_device_id == device_id);

    if (db_env && jid)
    {
        if (const auto bundle = extract_bundle_from_items(items))
        {
            store_bundle(*this, jid, remote_device_id, *bundle);
            if (!is_own_device)
            {
                const bool had_session_before = has_session(jid, remote_device_id);
                try
                {
                    (void)establish_session_from_bundle(
                        *this, account ? *account->context : nullptr, jid, remote_device_id);
                }
                catch (const std::exception &ex)
                {
                    if (buffer)
                        print_error(buffer, fmt::format(
                            "OMEMO session setup failed for {}/{}: {}",
                            jid, remote_device_id, ex.what()));
                }
                const bool session_is_fresh = !had_session_before && has_session(jid, remote_device_id);
                if ((session_is_fresh || needs_key_transport) && account && buffer)
                {
                    send_key_transport(*this, *account, buffer, jid, remote_device_id);
                }
            }
        }
    }

    if (buffer && jid)
    {
        const auto bundle = db_env ? load_bundle(*this, jid, remote_device_id) : std::nullopt;
        const auto prekey_count = bundle ? bundle->prekeys.size() : 0U;
        if (!is_own_device)
            print_info(buffer, fmt::format(
                "OMEMO received bundle for {}/{} ({} prekeys).",
                jid, remote_device_id, prekey_count));
    }

    // After successfully building a session with a remote contact's device,
    // auto-enable OMEMO on the corresponding PM channel if it has no transport
    // set yet. This ensures that the next message the user sends is encrypted
    // even if they haven't manually run /omemo on.
    if (!is_own_device && account && jid && has_session(jid, remote_device_id))
    {
        auto ch_it = account->channels.find(jid);
        if (ch_it != account->channels.end())
        {
            auto &ch = ch_it->second;
            if (ch.type == weechat::channel::chat_type::PM
                && !ch.omemo.enabled
                && ch.transport == weechat::channel::transport::PLAIN)
            {
                weechat_printf(ch.buffer,
                               "%sAuto-enabling OMEMO (OMEMO session established with %s)",
                               weechat_prefix("network"), jid);
                ch.omemo.enabled = 1;
                ch.set_transport(weechat::channel::transport::OMEMO, 0);
            }
        }

    }
}

bool weechat::xmpp::omemo::has_session(const char *jid, std::uint32_t remote_device_id)
{
    OMEMO_ASSERT(jid != nullptr, "session lookup requires a non-null jid");

    if (!db_env || !jid)
        return false;

    auto address = make_signal_address(jid, static_cast<std::int32_t>(remote_device_id));
    return signal_protocol_session_contains_session(store_context, &address.address) == 1;
}

char *weechat::xmpp::omemo::decode(weechat::account *account,
                                   struct t_gui_buffer *buffer,
                                   const char *jid,
                                   xmpp_stanza_t *encrypted)
{
    OMEMO_ASSERT(account != nullptr, "OMEMO decode requires a valid account");
    OMEMO_ASSERT(jid != nullptr, "OMEMO decode requires a peer jid");
    OMEMO_ASSERT(encrypted != nullptr, "OMEMO decode requires an encrypted stanza");

    if (!account || !jid || !encrypted)
    {
        print_error(buffer, "OMEMO decode received invalid input.");
        return nullptr;
    }

    xmpp_stanza_t *header = xmpp_stanza_get_child_by_name(encrypted, "header");
    xmpp_stanza_t *payload_stanza = xmpp_stanza_get_child_by_name(encrypted, "payload");
    if (!header || !payload_stanza)
    {
        print_error(buffer, "OMEMO message is missing header or payload.");
        return nullptr;
    }

    const auto sender_device_id = parse_uint32(
        xmpp_stanza_get_attribute(header, "sid")
            ? xmpp_stanza_get_attribute(header, "sid")
            : "");
    if (!sender_device_id)
    {
        print_error(buffer, "OMEMO message header is missing a valid sender sid.");
        return nullptr;
    }

    const auto payload_text = stanza_text(payload_stanza);
    const auto payload = base64_decode(*account->context, payload_text);
    if (payload.empty())
    {
        print_error(buffer, "OMEMO payload is empty or invalid base64.");
        return nullptr;
    }

    print_info(buffer, fmt::format("OMEMO decode: sender {} device {} payload-bytes={}",
                                   jid, *sender_device_id, payload.size()));

    std::optional<std::pair<std::array<std::uint8_t, 32>, std::array<std::uint8_t, 16>>> transport_key;
    std::optional<std::uint32_t> used_prekey_id;
    bool found_keys_elem = false;
    bool found_key_for_us = false;
    for (xmpp_stanza_t *child = xmpp_stanza_get_children(header);
         child && !transport_key;
         child = xmpp_stanza_get_next(child))
    {
        const char *name = xmpp_stanza_get_name(child);
        if (!name || weechat_strcasecmp(name, "keys") != 0)
            continue;
        found_keys_elem = true;

        const char *keys_jid = xmpp_stanza_get_attribute(child, "jid");
        print_info(buffer, fmt::format("OMEMO decode: <keys jid='{}'> element found",
                                       keys_jid ? keys_jid : "(none)"));

        for (xmpp_stanza_t *key_stanza = xmpp_stanza_get_children(child);
             key_stanza && !transport_key;
             key_stanza = xmpp_stanza_get_next(key_stanza))
        {
            const char *key_name = xmpp_stanza_get_name(key_stanza);
            if (!key_name || weechat_strcasecmp(key_name, "key") != 0)
                continue;

            const char *rid = xmpp_stanza_get_attribute(key_stanza, "rid");
            const auto rid_val = rid ? parse_uint32(rid).value_or(0) : 0;
            print_info(buffer, fmt::format("OMEMO decode:   <key rid='{}'> (our device_id={})",
                                           rid ? rid : "(null)", device_id));
            if (rid_val != device_id)
                continue;

            found_key_for_us = true;
            const bool is_prekey = xmpp_stanza_get_attribute(key_stanza, "kex") != nullptr;
            const auto serialized = base64_decode(*account->context, stanza_text(key_stanza));
            print_info(buffer, fmt::format("OMEMO decode:   found key for us: is_prekey={} serialized-bytes={}",
                                           is_prekey, serialized.size()));
            if (serialized.empty())
            {
                print_error(buffer, "OMEMO key element for our device has empty/invalid base64.");
                continue;
            }

            transport_key = decrypt_transport_key(*this, jid, *sender_device_id, serialized, is_prekey,
                                                  is_prekey ? &used_prekey_id : nullptr);
            if (!transport_key)
                print_error(buffer, "OMEMO Signal decryption of transport key failed.");
        }
    }

    if (!transport_key)
    {
        if (!found_keys_elem)
            print_error(buffer, "OMEMO message has no <keys> element in header.");
        else if (!found_key_for_us)
        {
            print_error(buffer, fmt::format(
                "OMEMO message has no key for our device {} (sender did not encrypt for us).",
                device_id));
            // The sender does not (yet) know our device_id.  Request their bundle
            // so we can establish a session from our side, then send a
            // KeyTransportElement that will teach them to include us in future
            // messages.  pending_key_transport is checked in handle_bundle().
            if (account && sender_device_id)
            {
                const auto key = std::make_pair(std::string{jid}, *sender_device_id);
                if (!pending_key_transport.count(key))
                {
                    pending_key_transport.insert(key);
                    request_bundle(*account, jid, *sender_device_id);
                    print_info(buffer, fmt::format(
                        "OMEMO: requested bundle for {}/{} to establish session "
                        "and send key-transport.",
                        jid, *sender_device_id));
                }
            }
        }
        else
            print_error(buffer, "OMEMO transport key decryption failed.");
        return nullptr;
    }

    const auto decrypted_xml = omemo2_decrypt(transport_key->first, transport_key->second, payload);
    if (!decrypted_xml)
    {
        print_error(buffer, "OMEMO payload decryption failed.");
        return nullptr;
    }

    print_info(buffer, fmt::format("OMEMO decode: decrypted SCE payload size={}",
                                   decrypted_xml->size()));

    const auto body = sce_unwrap(*account->context, *decrypted_xml);
    if (!body)
    {
        print_error(buffer, "OMEMO SCE envelope unwrap failed.");
        return nullptr;
    }

    // Per XEP-0384 §7.3 and Signal protocol best practice: after successfully
    // decrypting a PreKeySignalMessage (first message from a device), replace
    // the consumed pre-key with a fresh one of the same ID and republish the
    // bundle so contacts always have fresh pre-keys available.
    if (used_prekey_id && account)
    {
        if (replace_used_prekey(*this, *account->context, *used_prekey_id))
        {
            print_info(buffer, fmt::format(
                "OMEMO: replaced consumed pre-key {} — republishing bundle",
                *used_prekey_id));
            xmpp_stanza_t *bundle_stanza = get_bundle(*account->context, nullptr, nullptr);
            if (bundle_stanza)
            {
                account->connection.send(bundle_stanza);
                xmpp_stanza_release(bundle_stanza);
            }
        }
        else
        {
            print_error(buffer, fmt::format(
                "OMEMO: failed to replace consumed pre-key {} (non-fatal)",
                *used_prekey_id));
        }
    }

    return strdup(body->c_str());
}

xmpp_stanza_t *weechat::xmpp::omemo::encode(weechat::account *account,
                                            struct t_gui_buffer *buffer,
                                            const char *jid,
                                            const char *unencrypted)
{
    OMEMO_ASSERT(account != nullptr, "OMEMO encode requires a valid account");
    OMEMO_ASSERT(jid != nullptr, "OMEMO encode requires a peer jid");
    OMEMO_ASSERT(unencrypted != nullptr, "OMEMO encode requires plaintext input");

    if (!*this)
    {
        print_error(buffer, "OMEMO is not initialized.");
        return nullptr;
    }

    if (!account || !jid || !unencrypted)
    {
        print_error(buffer, "OMEMO encode received invalid input.");
        return nullptr;
    }

    ensure_local_identity(*this);
    ensure_registration_id(*this);
    ensure_prekeys(*this, *account->context);

    const auto devicelist = load_string(*this, key_for_devicelist(jid));
    if (!devicelist || devicelist->empty())
    {
        request_devicelist(*account, jid);
        print_error(buffer, fmt::format(
            "OMEMO has no known device list for {}. Requested the contact's OMEMO devices; retry after they arrive.", jid));
        return nullptr;
    }

    const auto sce = sce_wrap(*account->context, *account, unencrypted);
    const auto encrypted_payload = omemo2_encrypt(sce);
    if (!encrypted_payload)
    {
        print_error(buffer, "OMEMO payload encryption failed.");
        return nullptr;
    }

    xmpp_stanza_t *encrypted = xmpp_stanza_new(*account->context);
    xmpp_stanza_set_name(encrypted, "encrypted");
    xmpp_stanza_set_ns(encrypted, kOmemoNs.data());

    xmpp_stanza_t *header = xmpp_stanza_new(*account->context);
    xmpp_stanza_set_name(header, "header");
    xmpp_stanza_set_attribute(header, "sid", fmt::format("{}", device_id).c_str());

    // Helper: build a <keys jid='target_jid'> element, encrypting the transport
    // key for each device in device_list_str (semicolon-separated device IDs).
    // Returns {stanza, added_count}. Stanza is nullptr if none could be added.
    auto build_keys_elem = [&](const char *target_jid,
                               const std::string &device_list_str,
                               int &out_count) -> xmpp_stanza_t *
    {
        xmpp_stanza_t *keys = xmpp_stanza_new(*account->context);
        xmpp_stanza_set_name(keys, "keys");
        xmpp_stanza_set_attribute(keys, "jid", target_jid);
        out_count = 0;

        for (const auto &dev : split(device_list_str, ';'))
        {
            const auto remote_device_id = parse_uint32(dev);
            if (!remote_device_id || *remote_device_id == 0)
                continue;

            if (!has_session(target_jid, *remote_device_id))
            {
                if (!establish_session_from_bundle(*this, *account->context, target_jid, *remote_device_id))
                {
                    request_bundle(*account, target_jid, *remote_device_id);
                    print_info(buffer, fmt::format(
                        "OMEMO has no usable bundle/session yet for {}/{}; requested bundle fetch.",
                        target_jid, *remote_device_id));
                    continue;
                }
            }

            const auto transport = encrypt_transport_key(*this, target_jid, *remote_device_id, *encrypted_payload);
            if (!transport)
            {
                print_info(buffer, fmt::format(
                    "OMEMO failed to encrypt for device {}/{}.", target_jid, *remote_device_id));
                continue;
            }

            const auto encoded_transport = base64_encode(*account->context,
                                                         transport->first.data(),
                                                         transport->first.size());

            xmpp_stanza_t *key_stanza = xmpp_stanza_new(*account->context);
            xmpp_stanza_set_name(key_stanza, "key");
            xmpp_stanza_set_attribute(key_stanza, "rid", fmt::format("{}", *remote_device_id).c_str());
            if (transport->second)
                xmpp_stanza_set_attribute(key_stanza, "kex", "true");

            xmpp_stanza_t *key_text = xmpp_stanza_new(*account->context);
            xmpp_stanza_set_text(key_text, encoded_transport.c_str());
            xmpp_stanza_add_child(key_stanza, key_text);
            xmpp_stanza_release(key_text);

            xmpp_stanza_add_child(keys, key_stanza);
            xmpp_stanza_release(key_stanza);
            ++out_count;
        }

        if (out_count == 0)
        {
            xmpp_stanza_release(keys);
            return nullptr;
        }
        return keys;
    };

    bool added_any_key = false;

    // Encrypt for recipient devices
    {
        int count = 0;
        xmpp_stanza_t *keys = build_keys_elem(jid, *devicelist, count);
        if (keys)
        {
            xmpp_stanza_add_child(header, keys);
            xmpp_stanza_release(keys);
            added_any_key = true;
        }
    }

    // Encrypt for own devices (other than the current one), per XEP-0384 §7.2.
    // Own devices allow the message to be readable on our other devices and
    // prevents Conversations from flagging this as "not encrypted for sender".
    {
        xmpp_string_guard own_bare_g(*account->context,
            xmpp_jid_bare(*account->context, account->jid().data()));
        const std::string own_jid = own_bare_g ? own_bare_g.str() : std::string(account->jid());

        // Only add own-keys element if recipient is not already our own JID
        if (own_jid != jid)
        {
            const auto own_devicelist = load_string(*this, key_for_devicelist(own_jid));
            if (own_devicelist && !own_devicelist->empty())
            {
                int count = 0;
                xmpp_stanza_t *own_keys = build_keys_elem(own_jid.c_str(), *own_devicelist, count);
                if (own_keys)
                {
                    xmpp_stanza_add_child(header, own_keys);
                    xmpp_stanza_release(own_keys);
                }
            }
        }
    }

    if (!added_any_key)
    {
        xmpp_stanza_release(header);
        xmpp_stanza_release(encrypted);
        print_error(buffer, fmt::format("OMEMO could not encrypt for any known device of {}.", jid));
        return nullptr;
    }

    xmpp_stanza_t *payload = xmpp_stanza_new(*account->context);
    xmpp_stanza_set_name(payload, "payload");
    const auto encoded_payload = base64_encode(*account->context,
                                               encrypted_payload->payload.data(),
                                               encrypted_payload->payload.size());
    xmpp_stanza_t *payload_text = xmpp_stanza_new(*account->context);
    xmpp_stanza_set_text(payload_text, encoded_payload.c_str());
    xmpp_stanza_add_child(payload, payload_text);
    xmpp_stanza_release(payload_text);

    xmpp_stanza_add_child(encrypted, header);
    xmpp_stanza_release(header);
    xmpp_stanza_add_child(encrypted, payload);
    xmpp_stanza_release(payload);
    return encrypted;

}

void weechat::xmpp::omemo::show_fingerprint(struct t_gui_buffer *buffer, const char *jid)
{
    if (jid)
    {
        const auto devicelist = db_env ? load_string(*this, key_for_devicelist(jid)) : std::nullopt;
        const auto device_count = devicelist ? split(*devicelist, ';').size() : 0U;
        print_info(buffer, fmt::format(
            "OMEMO: {} known device(s) for {}; peer fingerprints are not implemented yet.",
            device_count, jid));
        return;
    }

    if (device_id == 0)
    {
        print_error(buffer, "OMEMO is not initialized.");
        return;
    }

    print_info(buffer, fmt::format(
        "OMEMO active; fingerprint generation is not implemented yet (device {}).",
        device_id));
}

void weechat::xmpp::omemo::distrust_jid(struct t_gui_buffer *buffer, const char *jid)
{
    if (!db_env || !jid)
        return;

    remove_prefixed_keys(*this, fmt::format("identity_key:{}", jid));
    remove_prefixed_keys(*this, fmt::format("bundle:{}:", jid));
    remove_prefixed_keys(*this, fmt::format("session:{}:", jid));
    print_info(buffer, fmt::format("Removed stored OMEMO data for {}.", jid));
}

void weechat::xmpp::omemo::show_devices(struct t_gui_buffer *buffer, const char *jid)
{
    if (!db_env || !jid)
    {
        print_error(buffer, "OMEMO device list is unavailable.");
        return;
    }

    const auto value = load_string(*this, key_for_devicelist(jid));
    if (!value || value->empty())
    {
        print_info(buffer, fmt::format("No OMEMO devices known for {}.", jid));
        return;
    }

    print_info(buffer, fmt::format("OMEMO devices for {}:", jid));
    for (const auto &device : split(*value, ';'))
        print_info(buffer, fmt::format("  {}", device));
}

void weechat::xmpp::omemo::show_status(struct t_gui_buffer *buffer,
                                       const char *account_name,
                                       const char *channel_name,
                                       int channel_omemo_enabled)
{
    print_info(buffer, fmt::format("OMEMO status for account {}:", account_name ? account_name : "?"));
    print_info(buffer, fmt::format("  initialized: {}", *this ? "yes" : "no"));
    print_info(buffer, fmt::format("  device id: {}", device_id));
    print_info(buffer, fmt::format("  database: {}", db_path.empty() ? "(none)" : db_path));
    print_info(buffer, fmt::format("  channel: {} ({})",
        channel_name ? channel_name : "(none)",
        channel_omemo_enabled ? "enabled" : "disabled"));
    print_info(buffer, fmt::format("  pubsub nodes: {}, {}", kDevicesNode, kBundlesNode));
    print_info(buffer, "  note: fingerprint/status reporting is still incomplete.");
}
