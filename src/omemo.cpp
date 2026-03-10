// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <fmt/core.h>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <string_view>
#include <stdlib.h>
#include <stdint.h>
#include <sys/param.h>
#include <time.h>
#include <math.h>
#include <limits.h>
#include <optional>
#include <ranges>
#include <filesystem>
#include <strophe.h>
#include "strophe.hh"
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "xmpp/stanza.hh"
#include "account.hh"
#include "omemo.hh"
#include "gcrypt.hh"
#include "util.hh"

// Compatibility wrapper for weechat string_base_encode/decode API changes
// ----------------------------------------------------------------------------
// WeeChat 4.3.0 (May 2024) changed the API for base64 encoding/decoding:
//   - Old API (<4.3.0): weechat_string_base_encode(64, ...)      [int parameter]
//   - New API (>=4.3.0): weechat_string_base_encode("64", ...)   [const char* parameter]
//
// Since weechat functions are macros wrapping function pointers, we cannot use
// compile-time detection. Instead, manually set the parameter based on your WeeChat version:
//
// For WeeChat >= 4.3.0 (Arch, recent distros): Use "64" (string)
// For WeeChat < 4.3.0 (Ubuntu 24.04, older distros): Use 64 (int)
//
// Change the #if 1 to #if 0 below to switch between versions:
#if 1  // Set to 0 for WeeChat < 4.3.0
    #define WEECHAT_BASE64 "64"
#else
    #define WEECHAT_BASE64 64
#endif

namespace weechat_compat {
    inline int base64_encode(const char *from, int length, char *to) {
        return weechat_string_base_encode(WEECHAT_BASE64, from, length, to);
    }
    
    inline int base64_decode(const char *from, char *to) {
        return weechat_string_base_decode(WEECHAT_BASE64, from, to);
    }
}

using namespace weechat::xmpp;
using t_omemo = omemo;

// RAII owner for weechat_string_dyn_alloc / weechat_string_dyn_free buffers.
// Owns a char** handle; frees it (and the inner string) on destruction.
struct dyn_string_deleter { void operator()(char **p) const { weechat_string_dyn_free(p, 1); } };
using dyn_string_ptr = std::unique_ptr<char*, dyn_string_deleter>;

#define mdb_val_str(s) { \
    .mv_size = strlen(s), .mv_data = (char*)s \
}

#define mdb_val_intptr(i) { \
    .mv_size = sizeof(*i), .mv_data = i \
}

#define mdb_val_sizeof(t) { \
    .mv_size = sizeof(t), .mv_data = NULL \
}

#define PRE_KEY_START 1
#define PRE_KEY_COUNT 100

#define AES_KEY_SIZE (16)
#define AES_IV_SIZE (12)

const char *OMEMO_ADVICE = "[OMEMO encrypted message (XEP-0384)]";

size_t base64_decode(const char *buffer, size_t length, uint8_t **result)
{
    *result = (uint8_t*)calloc(length + 1, sizeof(uint8_t));
    return weechat_compat::base64_decode(buffer, (char*)*result);
}

size_t base64_encode(const uint8_t *buffer, size_t length, char **result)
{
    *result = (char*)calloc(length * 2, sizeof(char));
    return weechat_compat::base64_encode((char*)buffer, length, *result);
}

std::vector<std::uint8_t> base64_decode(std::string_view buffer)
{
    auto result = std::make_unique<std::uint8_t[]>(buffer.size() + 1);
    return std::vector<std::uint8_t>(result.get(), result.get() + weechat_compat::base64_decode(buffer.data(), (char*)result.get()));
}

std::string base64_encode(std::vector<std::uint8_t> buffer)
{
    auto result = std::make_unique<char[]>(buffer.size() * 2);
    return std::string(result.get(), result.get() + weechat_compat::base64_encode((char*)buffer.data(), buffer.size(), result.get()));
}

int aes_decrypt(const uint8_t *ciphertext, size_t ciphertext_len,
                uint8_t *key, uint8_t *iv, uint8_t *tag, size_t tag_len,
                uint8_t **plaintext, size_t *plaintext_len)
{
    gcry_cipher_hd_t cipher = NULL;
    if (gcry_cipher_open(&cipher, GCRY_CIPHER_AES128,
                GCRY_CIPHER_MODE_GCM, GCRY_CIPHER_SECURE)) goto cleanup;
    if (gcry_cipher_setkey(cipher, key, AES_KEY_SIZE)) goto cleanup;
    if (gcry_cipher_setiv(cipher, iv, AES_IV_SIZE)) goto cleanup;
    *plaintext_len = ciphertext_len;
    *plaintext = (uint8_t*)malloc((sizeof(uint8_t) * *plaintext_len) + 1);
    if (gcry_cipher_decrypt(cipher, *plaintext, *plaintext_len,
                            ciphertext, ciphertext_len)) goto cleanup;
    if (gcry_cipher_checktag(cipher, tag, tag_len)) goto cleanup;
    gcry_cipher_close(cipher);
    return 1;
cleanup:
    if (*plaintext) {
        free(*plaintext);
        *plaintext = NULL;
    }
    gcry_cipher_close(cipher);
    return 0;
}

int aes_encrypt(const uint8_t *plaintext, size_t plaintext_len,
                uint8_t **key, uint8_t **iv, uint8_t **tag, size_t *tag_len,
                uint8_t **ciphertext, size_t *ciphertext_len)
{
    *tag_len = 16;
    *tag = (uint8_t*)calloc(*tag_len, sizeof(uint8_t));
    *iv = (uint8_t*)gcry_random_bytes(AES_IV_SIZE, GCRY_STRONG_RANDOM);
    *key = (uint8_t*)gcry_random_bytes(AES_KEY_SIZE, GCRY_STRONG_RANDOM);

    gcry_cipher_hd_t cipher = NULL;
    if (gcry_cipher_open(&cipher, GCRY_CIPHER_AES128,
                GCRY_CIPHER_MODE_GCM, GCRY_CIPHER_SECURE)) goto cleanup;
    if (gcry_cipher_setkey(cipher, *key, AES_KEY_SIZE)) goto cleanup;
    if (gcry_cipher_setiv(cipher, *iv, AES_IV_SIZE)) goto cleanup;
    *ciphertext_len = plaintext_len;
    *ciphertext = (uint8_t*)malloc((sizeof(uint8_t) * *ciphertext_len) + 1);
    if (gcry_cipher_encrypt(cipher, *ciphertext, *ciphertext_len,
                            plaintext, plaintext_len)) goto cleanup;
    if (gcry_cipher_gettag(cipher, *tag, *tag_len)) goto cleanup;
    gcry_cipher_close(cipher);
    return 1;
cleanup:
    gcry_cipher_close(cipher);
    if (*tag)        { free(*tag);        *tag = nullptr; }
    if (*iv)         { gcry_free(*iv);    *iv = nullptr; }
    if (*key)        { gcry_free(*key);   *key = nullptr; }
    if (*ciphertext) { free(*ciphertext); *ciphertext = nullptr; }
    return 0;
}

void signal_protocol_address_free(signal_protocol_address* ptr) {
    if (!ptr)
        return;
    if (ptr->name) {
        free((void*)ptr->name);
    }
    return free(ptr);
}

void signal_protocol_address_set_name(signal_protocol_address* self, const char* name) {
    if (!self)
        return;
    if (!name)
        return;
    char* n = (char*)malloc(strlen(name)+1);
    memcpy(n, name, strlen(name));
    n[strlen(name)] = 0;
    if (self->name) {
        free((void*)self->name);
    }
    self->name = n;
    self->name_len = strlen(n);
}

char* signal_protocol_address_get_name(signal_protocol_address* self) {
    if (!self)
        return NULL;
    if (!self->name)
        return 0;
    char* res = (char*)malloc(sizeof(char) * (self->name_len + 1));
    memcpy(res, self->name, self->name_len);
    res[self->name_len] = 0;
    return res;
}

int32_t signal_protocol_address_get_device_id(signal_protocol_address* self) {
    if (!self)
        return -1;
    return self->device_id;
}

void signal_protocol_address_set_device_id(signal_protocol_address* self, int32_t device_id) {
    if (!self)
        return;
    self->device_id = device_id;
}

signal_protocol_address* signal_protocol_address_new(const char* name, int32_t device_id) {
    if (!name)
        return NULL;
    signal_protocol_address* address = (signal_protocol_address*)malloc(sizeof(signal_protocol_address));
    address->device_id = -1;
    address->name = NULL;
    signal_protocol_address_set_name(address, name);
    signal_protocol_address_set_device_id(address, device_id);
    return address;
}

int aes_cipher(int cipher, size_t key_len, int* algo, int* mode) {
    switch (key_len) {
        case 16:
            *algo = GCRY_CIPHER_AES128;
            break;
        case 24:
            *algo = GCRY_CIPHER_AES192;
            break;
        case 32:
            *algo = GCRY_CIPHER_AES256;
            break;
        default:
            return SG_ERR_UNKNOWN;
    }
    switch (cipher) {
        case SG_CIPHER_AES_CBC_PKCS5:
            *mode = GCRY_CIPHER_MODE_CBC;
            break;
        case SG_CIPHER_AES_CTR_NOPADDING:
            *mode = GCRY_CIPHER_MODE_CTR;
            break;
        default:
            return SG_ERR_UNKNOWN;
    }
    return SG_SUCCESS;
}

void lock_function(void *user_data)
{
    (void) user_data;
}

void unlock_function(void *user_data)
{
    (void) user_data;
}

int cp_randomize(uint8_t *data, size_t len) {
    gcry_randomize(data, len, GCRY_STRONG_RANDOM);
    return SG_SUCCESS;
}

int cp_random_generator(uint8_t *data, size_t len, void *) {
    gcry_randomize(data, len, GCRY_STRONG_RANDOM);
    return SG_SUCCESS;
}

int cp_hmac_sha256_init(void **hmac_context, const uint8_t *key, size_t key_len, void *) {
    gcry_mac_hd_t* ctx = (gcry_mac_hd_t*)malloc(sizeof(gcry_mac_hd_t));
    if (!ctx) return SG_ERR_NOMEM;

    if (gcry_mac_open(ctx, GCRY_MAC_HMAC_SHA256, 0, 0)) {
        free(ctx);
        return SG_ERR_UNKNOWN;
    }

    if (gcry_mac_setkey(*ctx, key, key_len)) {
        free(ctx);
        return SG_ERR_UNKNOWN;
    }

    *hmac_context = ctx;

    return SG_SUCCESS;
}

int cp_hmac_sha256_update(void *hmac_context, const uint8_t *data, size_t data_len, void *) {
    gcry_mac_hd_t* ctx = (gcry_mac_hd_t*)hmac_context;

    if (gcry_mac_write(*ctx, data, data_len)) return SG_ERR_UNKNOWN;

    return SG_SUCCESS;
}

int cp_hmac_sha256_final(void *hmac_context, struct signal_buffer **output, void *) {
    size_t len = gcry_mac_get_algo_maclen(GCRY_MAC_HMAC_SHA256);
    auto md = std::unique_ptr<uint8_t[]>(new uint8_t[len]);
    gcry_mac_hd_t* ctx = (gcry_mac_hd_t*)hmac_context;

    if (gcry_mac_read(*ctx, md.get(), &len)) return SG_ERR_UNKNOWN;

    struct signal_buffer *output_buffer = signal_buffer_create(md.get(), len);
    if (!output_buffer) return SG_ERR_NOMEM;

    *output = output_buffer;

    return SG_SUCCESS;
}

void cp_hmac_sha256_cleanup(void *hmac_context, void *) {
    gcry_mac_hd_t* ctx = (gcry_mac_hd_t*)hmac_context;
    if (ctx) {
        gcry_mac_close(*ctx);
        free(ctx);
    }
}

int cp_sha512_digest_init(void **digest_context, void *) {
    auto ctx = std::make_unique<gcry_md_hd_t>();
    if (!ctx) return SG_ERR_NOMEM;

    if (gcry_md_open(ctx.get(), GCRY_MD_SHA512, 0)) {
        return SG_ERR_UNKNOWN;
    }

    *digest_context = ctx.release();

    return SG_SUCCESS;
}

int cp_sha512_digest_update(void *digest_context, const uint8_t *data, size_t data_len, void *) {
    gcry_md_hd_t* ctx = (gcry_md_hd_t*)digest_context;

    gcry_md_write(*ctx, data, data_len);

    return SG_SUCCESS;
}

int cp_sha512_digest_final(void *digest_context, struct signal_buffer **output, void *) {
    size_t len = gcry_md_get_algo_dlen(GCRY_MD_SHA512);
    gcry_md_hd_t* ctx = (gcry_md_hd_t*)digest_context;

    uint8_t* md = gcry_md_read(*ctx, GCRY_MD_SHA512);
    if (!md) return SG_ERR_UNKNOWN;

    gcry_md_reset(*ctx);

    struct signal_buffer *output_buffer = signal_buffer_create(md, len);
    // Note: md is an internal gcrypt pointer from gcry_md_read() - must NOT be free()'d
    if (!output_buffer) return SG_ERR_NOMEM;

    *output = output_buffer;

    return SG_SUCCESS;
}

void cp_sha512_digest_cleanup(void *digest_context, void *) {
    gcry_md_hd_t* ctx = static_cast<gcry_md_hd_t*>(digest_context);
    if (ctx) {
        gcry_md_close(*ctx);
        delete ctx;
    }
}

int cp_encrypt(struct signal_buffer **output,
        int cipher,
        const uint8_t *key, size_t key_len,
        const uint8_t *iv, size_t iv_len,
        const uint8_t *plaintext, size_t plaintext_len,
        void *) {
    int algo, mode, error_code = SG_ERR_UNKNOWN;
    if (aes_cipher(cipher, key_len, &algo, &mode)) return SG_ERR_INVAL;

    gcry_cipher_hd_t ctx = {0};

    if (gcry_cipher_open(&ctx, algo, mode, 0)) return SG_ERR_NOMEM;

    signal_buffer* padded = 0;
    signal_buffer* out_buf = 0;
    goto no_error;
error:
    gcry_cipher_close(ctx);
    if (padded != 0) {
        signal_buffer_bzero_free(padded);
    }
    if (out_buf != 0) {
        signal_buffer_free(out_buf);
    }
    return error_code;
no_error:

    if (gcry_cipher_setkey(ctx, key, key_len)) goto error;

    uint8_t tag_len = 0, pad_len = 0;
    switch (cipher) {
        case SG_CIPHER_AES_CBC_PKCS5:
            if (gcry_cipher_setiv(ctx, iv, iv_len)) goto error;
            pad_len = 16 - (plaintext_len % 16);
            if (pad_len == 0) pad_len = 16;
            break;
        case SG_CIPHER_AES_CTR_NOPADDING:
            if (gcry_cipher_setctr(ctx, iv, iv_len)) goto error;
            break;
        default:
            return SG_ERR_UNKNOWN;
    }

    size_t padded_len = plaintext_len + pad_len;
    padded = signal_buffer_alloc(padded_len);
    if (padded == 0) {
        error_code = SG_ERR_NOMEM;
        goto error;
    }

    memset(signal_buffer_data(padded) + plaintext_len, pad_len, pad_len);
    memcpy(signal_buffer_data(padded), plaintext, plaintext_len);

    out_buf = signal_buffer_alloc(padded_len + tag_len);
    if (out_buf == 0) {
        error_code = SG_ERR_NOMEM;
        goto error;
    }

    if (gcry_cipher_encrypt(ctx, signal_buffer_data(out_buf), padded_len, signal_buffer_data(padded), padded_len)) goto error;

    if (tag_len > 0) {
        if (gcry_cipher_gettag(ctx, signal_buffer_data(out_buf) + padded_len, tag_len)) goto error;
    }

    *output = out_buf;
    out_buf = 0;

    signal_buffer_bzero_free(padded);
    padded = 0;

    gcry_cipher_close(ctx);
    return SG_SUCCESS;
}

int cp_decrypt(struct signal_buffer **output,
        int cipher,
        const uint8_t *key, size_t key_len,
        const uint8_t *iv, size_t iv_len,
        const uint8_t *ciphertext, size_t ciphertext_len,
        void *) {
    int algo, mode, error_code = SG_ERR_UNKNOWN;
    *output = 0;
    if (aes_cipher(cipher, key_len, &algo, &mode)) return SG_ERR_INVAL;
    if (ciphertext_len == 0) return SG_ERR_INVAL;

    gcry_cipher_hd_t ctx = {0};

    if (gcry_cipher_open(&ctx, algo, mode, 0)) return SG_ERR_NOMEM;

    signal_buffer* out_buf = 0;
    goto no_error;
error:
    gcry_cipher_close(ctx);
    if (out_buf != 0) {
        signal_buffer_bzero_free(out_buf);
    }
    return error_code;
no_error:

    if (gcry_cipher_setkey(ctx, key, key_len)) goto error;

    uint8_t tag_len = 0, pkcs_pad = 0;
    switch (cipher) {
        case SG_CIPHER_AES_CBC_PKCS5:
            if (gcry_cipher_setiv(ctx, iv, iv_len)) goto error;
            pkcs_pad = 1;
            break;
        case SG_CIPHER_AES_CTR_NOPADDING:
            if (gcry_cipher_setctr(ctx, iv, iv_len)) goto error;
            break;
        default:
            goto error;
    }

    size_t padded_len = ciphertext_len - tag_len;
    out_buf = signal_buffer_alloc(padded_len);
    if (out_buf == 0) {
        error_code = SG_ERR_NOMEM;
        goto error;
    }

    if (gcry_cipher_decrypt(ctx, signal_buffer_data(out_buf), signal_buffer_len(out_buf), ciphertext, padded_len)) goto error;

    if (tag_len > 0) {
        if (gcry_cipher_checktag(ctx, ciphertext + padded_len, tag_len)) goto error;
    }

    if (pkcs_pad) {
        uint8_t pad_len = signal_buffer_data(out_buf)[padded_len - 1];
        if (pad_len > 16 || pad_len > padded_len) goto error;
        *output = signal_buffer_create(signal_buffer_data(out_buf), padded_len - pad_len);
        signal_buffer_bzero_free(out_buf);
        out_buf = 0;
    } else {
        *output = out_buf;
        out_buf = 0;
    }

    gcry_cipher_close(ctx);
    return SG_SUCCESS;
}

int iks_get_identity_key_pair(struct signal_buffer **public_data, signal_buffer **private_data, void *user_data)
{
    auto omemo = reinterpret_cast<t_omemo*>(user_data);
    MDB_txn *transaction = NULL;
    MDB_val k_local_private_key = mdb_val_str("local_private_key");
    MDB_val k_local_public_key = mdb_val_str("local_public_key");
    MDB_val v_local_private_key, v_local_public_key;

    if (mdb_txn_begin(omemo->db_env, NULL, MDB_RDONLY, &transaction)) {
        weechat_printf(NULL, "%sxmpp: failed to open lmdb transaction",
                       weechat_prefix("error"));
        return -1;
    }

    if (!mdb_get(transaction, omemo->dbi.omemo,
                 &k_local_private_key, &v_local_private_key) &&
        !mdb_get(transaction, omemo->dbi.omemo,
                 &k_local_public_key, &v_local_public_key))
    {
        *private_data = signal_buffer_create((const uint8_t*)v_local_private_key.mv_data, v_local_private_key.mv_size);
        *public_data = signal_buffer_create((const uint8_t*)v_local_public_key.mv_data, v_local_public_key.mv_size);

        if (mdb_txn_commit(transaction)) {
            weechat_printf(NULL, "%sxmpp: failed to write lmdb transaction",
                           weechat_prefix("error"));
            goto cleanup;
        };
    }
    else
    {
        auto identity = libsignal::identity_key_pair::generate(omemo->context);

        // Use raw pointers directly — do NOT wrap in RAII types here.
        // get_private()/get_public() return borrowed pointers owned by the
        // identity pair (refcount NOT incremented).  Wrapping them in a
        // libsignal::private_key / public_key RAII object would call
        // ec_*_destroy (SIGNAL_UNREF) on destruction, dropping the refcount
        // to 0 and freeing the structs while the identity pair still holds
        // dangling pointers — causing SIGSEGV in ~omemo().
        ec_private_key_serialize(private_data,
                ratchet_identity_key_pair_get_private(identity));
        ec_public_key_serialize(public_data,
                ratchet_identity_key_pair_get_public(identity));

        v_local_private_key.mv_data = signal_buffer_data(*private_data);
        v_local_private_key.mv_size = signal_buffer_len(*private_data);
        v_local_public_key.mv_data = signal_buffer_data(*public_data);
        v_local_public_key.mv_size = signal_buffer_len(*public_data);

        mdb_txn_abort(transaction);
        if (mdb_txn_begin(omemo->db_env, NULL, 0, &transaction)) {
            weechat_printf(NULL, "%sxmpp: failed to open lmdb transaction",
                           weechat_prefix("error"));
            return -1;
        }

        if (mdb_put(transaction, omemo->dbi.omemo,
                    &k_local_private_key, &v_local_private_key, MDB_NOOVERWRITE) ||
            mdb_put(transaction, omemo->dbi.omemo,
                    &k_local_public_key, &v_local_public_key, MDB_NOOVERWRITE))
        {
            weechat_printf(NULL, "%sxmpp: failed to write lmdb value",
                           weechat_prefix("error"));
            goto cleanup;
        };

        if (mdb_txn_commit(transaction)) {
            weechat_printf(NULL, "%sxmpp: failed to write lmdb transaction",
                           weechat_prefix("error"));
            goto cleanup;
        };

        *private_data = signal_buffer_create((const uint8_t*)v_local_private_key.mv_data,
                v_local_private_key.mv_size);
        *public_data = signal_buffer_create((const uint8_t*)v_local_public_key.mv_data,
                v_local_public_key.mv_size);

        omemo->identity = std::move(identity);
    }

    return 0;
cleanup:
    mdb_txn_abort(transaction);
    return -1;
}

int iks_get_local_registration_id(void *user_data, uint32_t *registration_id)
{
    auto omemo = reinterpret_cast<t_omemo*>(user_data);
    MDB_txn *transaction = NULL;
    MDB_val k_local_registration_id = mdb_val_str("local_registration_id");
    MDB_val v_local_registration_id = mdb_val_sizeof(uint32_t);

    // Return the local client's registration ID
    if (mdb_txn_begin(omemo->db_env, NULL, MDB_RDONLY, &transaction)) {
        weechat_printf(NULL, "%sxmpp: failed to open lmdb transaction",
                       weechat_prefix("error"));
        return -1;
    }

    if (!mdb_get(transaction, omemo->dbi.omemo,
                 &k_local_registration_id,
                 &v_local_registration_id))
    {
        *registration_id = *(uint32_t*)v_local_registration_id.mv_data;

        if (mdb_txn_commit(transaction)) {
            weechat_printf(NULL, "%sxmpp: failed to read lmdb transaction",
                           weechat_prefix("error"));
            goto cleanup;
        };
    }
    else
    {
        uint32_t generated_id;
        signal_protocol_key_helper_generate_registration_id(
            &generated_id, 0, omemo->context);
        v_local_registration_id.mv_data = &generated_id;

        mdb_txn_abort(transaction);
        if (mdb_txn_begin(omemo->db_env, NULL, 0, &transaction)) {
            weechat_printf(NULL, "%sxmpp: failed to open lmdb transaction",
                           weechat_prefix("error"));
            return -1;
        }

        if (mdb_put(transaction, omemo->dbi.omemo,
                    &k_local_registration_id,
                    &v_local_registration_id, MDB_NOOVERWRITE))
        {
            weechat_printf(NULL, "%sxmpp: failed to write lmdb value",
                           weechat_prefix("error"));
            goto cleanup;
        };

        if (mdb_txn_commit(transaction)) {
            weechat_printf(NULL, "%sxmpp: failed to write lmdb transaction",
                           weechat_prefix("error"));
            goto cleanup;
        };

        *registration_id = generated_id;
    }

    return 0;
cleanup:
    mdb_txn_abort(transaction);
    return -1;
}

int iks_save_identity(const struct signal_protocol_address *address, uint8_t *key_data, size_t key_len, void *user_data)
{
    auto omemo = reinterpret_cast<t_omemo*>(user_data);
    MDB_txn *transaction = NULL;
    std::string k_str = fmt::format("identity_key_{}_{}", address->name, address->device_id);
    MDB_val k_identity_key = { .mv_size = k_str.size(), .mv_data = k_str.data() };
    MDB_val v_identity_key = {.mv_size = key_len, .mv_data = key_data};

    if (mdb_txn_begin(omemo->db_env, NULL, 0, &transaction)) {
        weechat_printf(NULL, "%sxmpp: failed to open lmdb transaction",
                       weechat_prefix("error"));
        return -1;
    }

    if (mdb_put(transaction, omemo->dbi.omemo, &k_identity_key,
                &v_identity_key, 0)) {
      weechat_printf(NULL, "%sxmpp: failed to write lmdb value",
                     weechat_prefix("error"));
      goto cleanup;
    };

    if (mdb_txn_commit(transaction)) {
      weechat_printf(NULL, "%sxmpp: failed to write lmdb transaction",
                     weechat_prefix("error"));
      goto cleanup;
    };

    return 0;
cleanup:
    mdb_txn_abort(transaction);
    return -1;
}

int iks_is_trusted_identity(const struct signal_protocol_address *address, uint8_t *key_data, size_t key_len, void *user_data)
{
    auto omemo = reinterpret_cast<t_omemo*>(user_data);
    MDB_txn *transaction = NULL;
    std::string k_str = fmt::format("identity_key_{}_{}", address->name, address->device_id);
    MDB_val k_identity_key = { .mv_size = k_str.size(), .mv_data = k_str.data() };
    MDB_val v_stored = { .mv_size = 0, .mv_data = NULL };

    // Read-only pass: check if we already have a key stored
    if (mdb_txn_begin(omemo->db_env, NULL, MDB_RDONLY, &transaction)) {
        weechat_printf(NULL, "%sxmpp: failed to open lmdb transaction",
                       weechat_prefix("error"));
        return -1;
    }

    int rc = mdb_get(transaction, omemo->dbi.omemo, &k_identity_key, &v_stored);
    mdb_txn_abort(transaction);
    transaction = NULL;

    if (rc == MDB_NOTFOUND) {
        // TOFU: no key stored yet — store this one and trust it
        MDB_val v_new = { .mv_size = key_len, .mv_data = key_data };
        if (mdb_txn_begin(omemo->db_env, NULL, 0, &transaction)) {
            weechat_printf(NULL, "%sxmpp: failed to open lmdb transaction",
                           weechat_prefix("error"));
            return -1;
        }
        if (mdb_put(transaction, omemo->dbi.omemo, &k_identity_key, &v_new, MDB_NOOVERWRITE)) {
            weechat_printf(NULL, "%sxmpp: failed to write lmdb value",
                           weechat_prefix("error"));
            mdb_txn_abort(transaction);
            return -1;
        }
        if (mdb_txn_commit(transaction)) {
            weechat_printf(NULL, "%sxmpp: failed to commit lmdb transaction",
                           weechat_prefix("error"));
            return -1;
        }
        return 1; // trusted (first-use)
    }

    if (rc != 0) {
        // Unexpected LMDB error
        weechat_printf(NULL, "%sxmpp: failed to read lmdb value (rc=%d)",
                       weechat_prefix("error"), rc);
        return -1;
    }

    // Key found — check if it matches
    if (v_stored.mv_size == key_len &&
        memcmp(v_stored.mv_data, key_data, key_len) == 0) {
        return 1; // trusted
    }

    // Key mismatch — possible key change / MITM attack
    weechat_printf(NULL,
        "%somemo: identity key CHANGED for %s device %u — possible key rotation or attack! "
        "Use /omemo trust %s to accept the new key.",
        weechat_prefix("error"), address->name, address->device_id, address->name);
    return 0; // untrusted
}

void iks_destroy_func(void *user_data)
{
    auto omemo = reinterpret_cast<t_omemo*>(user_data);
    (void) omemo;
    // Function called to perform cleanup when the data store context is being destroyed
}

int pks_store_pre_key(uint32_t pre_key_id, uint8_t *record, size_t record_len, void *user_data)
{
    auto omemo = reinterpret_cast<t_omemo*>(user_data);
    MDB_txn *transaction = NULL;
    std::string k_str = fmt::format("pre_key_{:<10}", pre_key_id);
    MDB_val k_pre_key = { .mv_size = k_str.size(), .mv_data = k_str.data() };
    MDB_val v_pre_key = {.mv_size = record_len, .mv_data = record};

    if (mdb_txn_begin(omemo->db_env, NULL, 0, &transaction)) {
        weechat_printf(NULL, "%sxmpp: failed to open lmdb transaction",
                       weechat_prefix("error"));
        return -1;
    }

    if (mdb_put(transaction, omemo->dbi.omemo, &k_pre_key,
                &v_pre_key, 0)) {
      weechat_printf(NULL, "%sxmpp: failed to write lmdb value",
                     weechat_prefix("error"));
      goto cleanup;
    };

    if (mdb_txn_commit(transaction)) {
      weechat_printf(NULL, "%sxmpp: failed to write lmdb transaction",
                     weechat_prefix("error"));
      goto cleanup;
    };

    return 0;
cleanup:
    mdb_txn_abort(transaction);
    return -1;
}

int pks_contains_pre_key(uint32_t pre_key_id, void *user_data)
{
    auto omemo = reinterpret_cast<t_omemo*>(user_data);
    MDB_txn *transaction = NULL;
    std::string k_str = fmt::format("pre_key_{:<10}", pre_key_id);
    MDB_val k_pre_key = { .mv_size = k_str.size(), .mv_data = k_str.data() };
    MDB_val v_pre_key;

    if (mdb_txn_begin(omemo->db_env, NULL, MDB_RDONLY, &transaction)) {
        weechat_printf(NULL, "%sxmpp: failed to open lmdb transaction",
                       weechat_prefix("error"));
        return -1;
    }

    if (mdb_get(transaction, omemo->dbi.omemo, &k_pre_key,
                &v_pre_key)) {
        weechat_printf(NULL, "%sxmpp: failed to read lmdb value",
                       weechat_prefix("error"));
        mdb_txn_abort(transaction);
        goto cleanup;
    };

    mdb_txn_abort(transaction);

    return 1;
cleanup:
    mdb_txn_abort(transaction);
    return 0;
}

uint32_t pks_get_count(t_omemo *omemo, int increment)
{
    uint32_t count = PRE_KEY_START;
    MDB_txn *transaction = NULL;
    MDB_val k_pre_key_idx = mdb_val_str("pre_key_idx");
    MDB_val v_pre_key_idx = mdb_val_intptr(&count);

    if (mdb_txn_begin(omemo->db_env, NULL, 0, &transaction)) {
        weechat_printf(NULL, "%sxmpp: failed to open lmdb transaction",
                       weechat_prefix("error"));
        return -1;
    }

    if (!mdb_get(transaction, omemo->dbi.omemo,
                 &k_pre_key_idx, &v_pre_key_idx))
    {
        if (increment)
            count += PRE_KEY_COUNT;
    }

    if (mdb_put(transaction, omemo->dbi.omemo,
                &k_pre_key_idx, &v_pre_key_idx, 0))
    {
        weechat_printf(NULL, "%sxmpp: failed to read lmdb value",
                       weechat_prefix("error"));
        goto cleanup;
    };

    if (mdb_txn_commit(transaction)) {
        weechat_printf(NULL, "%sxmpp: failed to write lmdb transaction",
                       weechat_prefix("error"));
        goto cleanup;
    };

    return count;
cleanup:
    mdb_txn_abort(transaction);
    return 0;
}

int pks_load_pre_key(struct signal_buffer **record, uint32_t pre_key_id, void *user_data)
{
    auto omemo = reinterpret_cast<t_omemo*>(user_data);
    MDB_txn *transaction = NULL;
    std::string k_str = fmt::format("pre_key_{:<10}", pre_key_id);
    MDB_val k_pre_key = { .mv_size = k_str.size(), .mv_data = k_str.data() };
    MDB_val v_pre_key;

    if (mdb_txn_begin(omemo->db_env, NULL, 0, &transaction)) {
        weechat_printf(NULL, "%sxmpp: failed to open lmdb transaction",
                       weechat_prefix("error"));
        return -1;
    }

    if (!mdb_get(transaction, omemo->dbi.omemo,
                 &k_pre_key, &v_pre_key))
    {
        *record = signal_buffer_create((const uint8_t*)v_pre_key.mv_data, v_pre_key.mv_size);

        if (mdb_txn_commit(transaction)) {
            weechat_printf(NULL, "%sxmpp: failed to close lmdb transaction",
                           weechat_prefix("error"));
            goto cleanup;
        };
    }
    else
    {
        mdb_txn_abort(transaction);

        signal_protocol_key_helper_pre_key_list_node *pre_keys_list;
        session_pre_key *pre_key = NULL;

        for (signal_protocol_key_helper_generate_pre_keys(&pre_keys_list,
                    pks_get_count(omemo, 1), PRE_KEY_COUNT,
                    omemo->context); pre_keys_list;
             pre_keys_list = signal_protocol_key_helper_key_list_next(pre_keys_list))
        {
            pre_key = signal_protocol_key_helper_key_list_element(pre_keys_list);
            uint32_t id = session_pre_key_get_id(pre_key);
            session_pre_key_serialize(record, pre_key);
            pks_store_pre_key(id, signal_buffer_data(*record),
                    signal_buffer_len(*record), user_data);
        }
        signal_protocol_key_helper_key_list_free(pre_keys_list);
    }

    return 0;
cleanup:
    mdb_txn_abort(transaction);
    return -1;
}

int pks_remove_pre_key(uint32_t pre_key_id, void *user_data)
{
    auto omemo = reinterpret_cast<t_omemo*>(user_data);
    MDB_txn *transaction = NULL;
    std::string k_str = fmt::format("pre_key_{:<10}", pre_key_id);
    MDB_val k_pre_key = { .mv_size = k_str.size(), .mv_data = k_str.data() };
    MDB_val v_pre_key;

    if (mdb_txn_begin(omemo->db_env, NULL, 0, &transaction)) {
        weechat_printf(NULL, "%sxmpp: failed to open lmdb transaction",
                       weechat_prefix("error"));
        return -1;
    }

    if (mdb_del(transaction, omemo->dbi.omemo, &k_pre_key,
                &v_pre_key)) {
      weechat_printf(NULL, "%sxmpp: failed to erase lmdb value",
                     weechat_prefix("error"));
      goto cleanup;
    };

    if (mdb_txn_commit(transaction)) {
        weechat_printf(NULL, "%sxmpp: failed to close lmdb transaction",
                       weechat_prefix("error"));
        goto cleanup;
    };

    // Flag that the bundle should be re-published since a pre-key was consumed
    {
        MDB_txn *flag_txn = NULL;
        if (mdb_txn_begin(omemo->db_env, NULL, 0, &flag_txn) == 0) {
            MDB_val k_flag = mdb_val_str("pre_key_repub_needed");
            MDB_val v_flag = mdb_val_str("1");
            mdb_put(flag_txn, omemo->dbi.omemo, &k_flag, &v_flag, 0);
            mdb_txn_commit(flag_txn);
        }
    }

    return 0;
cleanup:
    mdb_txn_abort(transaction);
    return -1;
}

void pks_destroy_func(void *user_data)
{
    auto omemo = reinterpret_cast<t_omemo*>(user_data);
    (void) omemo;
    // Function called to perform cleanup when the data store context is being destroyed
}

int spks_load_signed_pre_key(struct signal_buffer **record, uint32_t signed_pre_key_id, void *user_data)
{
    auto omemo = reinterpret_cast<t_omemo*>(user_data);
    MDB_txn *transaction = NULL;
    std::string k_str = fmt::format("signed_pre_key_{:<10}", signed_pre_key_id);
    MDB_val k_signed_pre_key = { .mv_size = k_str.size(), .mv_data = k_str.data() };
    MDB_val v_signed_pre_key;

    if (mdb_txn_begin(omemo->db_env, NULL, 0, &transaction)) {
        weechat_printf(NULL, "%sxmpp: failed to open lmdb transaction",
                       weechat_prefix("error"));
        return -1;
    }

    if (!mdb_get(transaction, omemo->dbi.omemo,
                 &k_signed_pre_key, &v_signed_pre_key))
    {
        *record = signal_buffer_create((const uint8_t*)v_signed_pre_key.mv_data, v_signed_pre_key.mv_size);

        if (mdb_txn_commit(transaction)) {
            weechat_printf(NULL, "%sxmpp: failed to close lmdb transaction",
                           weechat_prefix("error"));
            goto cleanup;
        };
    }
    else
    {
        session_signed_pre_key *signed_pre_key = NULL;
        struct signal_buffer *serialized_key = NULL;

        signal_protocol_key_helper_generate_signed_pre_key(&signed_pre_key, omemo->identity, signed_pre_key_id, time(NULL), omemo->context);
        session_signed_pre_key_serialize(&serialized_key, signed_pre_key);

        v_signed_pre_key.mv_data = signal_buffer_data(serialized_key);
        v_signed_pre_key.mv_size = signal_buffer_len(serialized_key);

        if (mdb_put(transaction, omemo->dbi.omemo,
                    &k_signed_pre_key, &v_signed_pre_key, MDB_NOOVERWRITE))
        {
            weechat_printf(NULL, "%sxmpp: failed to read lmdb value",
                           weechat_prefix("error"));
            goto cleanup;
        };

        if (mdb_txn_commit(transaction)) {
            weechat_printf(NULL, "%sxmpp: failed to write lmdb transaction",
                           weechat_prefix("error"));
            goto cleanup;
        };

        *record = serialized_key;
    }

    return 0;
cleanup:
    mdb_txn_abort(transaction);
    return -1;
}

int spks_store_signed_pre_key(uint32_t signed_pre_key_id, uint8_t *record, size_t record_len, void *user_data)
{
    auto omemo = reinterpret_cast<t_omemo*>(user_data);
    MDB_txn *transaction = NULL;
    std::string k_str = fmt::format("signed_pre_key_{:<10}", signed_pre_key_id);
    MDB_val k_signed_pre_key = { .mv_size = k_str.size(), .mv_data = k_str.data() };
    MDB_val v_signed_pre_key = {.mv_size = record_len, .mv_data = record};

    if (mdb_txn_begin(omemo->db_env, NULL, 0, &transaction)) {
        weechat_printf(NULL, "%sxmpp: failed to open lmdb transaction",
                       weechat_prefix("error"));
        return -1;
    }

    if (mdb_put(transaction, omemo->dbi.omemo, &k_signed_pre_key,
                &v_signed_pre_key, 0)) {
      weechat_printf(NULL, "%sxmpp: failed to write lmdb value",
                     weechat_prefix("error"));
      goto cleanup;
    };

    if (mdb_txn_commit(transaction)) {
      weechat_printf(NULL, "%sxmpp: failed to write lmdb transaction",
                     weechat_prefix("error"));
      goto cleanup;
    };

    return 0;
cleanup:
    mdb_txn_abort(transaction);
    return -1;
}

int spks_contains_signed_pre_key(uint32_t signed_pre_key_id, void *user_data)
{
    auto omemo = reinterpret_cast<t_omemo*>(user_data);
    MDB_txn *transaction = NULL;
    std::string k_str = fmt::format("signed_pre_key_{:<10}", signed_pre_key_id);
    MDB_val k_signed_pre_key = { .mv_size = k_str.size(), .mv_data = k_str.data() };
    MDB_val v_signed_pre_key;

    if (mdb_txn_begin(omemo->db_env, NULL, MDB_RDONLY, &transaction)) {
        weechat_printf(NULL, "%sxmpp: failed to open lmdb transaction",
                       weechat_prefix("error"));
        return -1;
    }

    if (mdb_get(transaction, omemo->dbi.omemo, &k_signed_pre_key,
                &v_signed_pre_key)) {
        mdb_txn_abort(transaction);
        goto cleanup;
    };

    mdb_txn_abort(transaction);

    return 1;
cleanup:
    mdb_txn_abort(transaction);
    return 0;
}

int spks_remove_signed_pre_key(uint32_t signed_pre_key_id, void *user_data)
{
    auto omemo = reinterpret_cast<t_omemo*>(user_data);
    MDB_txn *transaction = NULL;
    std::string k_str = fmt::format("signed_pre_key_{:<10}", signed_pre_key_id);
    MDB_val k_signed_pre_key = { .mv_size = k_str.size(), .mv_data = k_str.data() };
    MDB_val v_signed_pre_key;

    if (mdb_txn_begin(omemo->db_env, NULL, 0, &transaction)) {
        weechat_printf(NULL, "%sxmpp: failed to open lmdb transaction",
                       weechat_prefix("error"));
        return -1;
    }

    if (mdb_del(transaction, omemo->dbi.omemo, &k_signed_pre_key,
                &v_signed_pre_key)) {
      weechat_printf(NULL, "%sxmpp: failed to erase lmdb value",
                     weechat_prefix("error"));
      goto cleanup;
    };

    if (mdb_txn_commit(transaction)) {
        weechat_printf(NULL, "%sxmpp: failed to close lmdb transaction",
                       weechat_prefix("error"));
        goto cleanup;
    };

    return 0;
cleanup:
    mdb_txn_abort(transaction);
    return -1;
}

void spks_destroy_func(void *user_data)
{
    auto omemo = reinterpret_cast<t_omemo*>(user_data);
    (void) omemo;
    // Function called to perform cleanup when the data store context is being destroyed
}

int ss_load_session_func(struct signal_buffer **record, signal_buffer **user_record, const struct signal_protocol_address *address, void *user_data)
{
    auto omemo = reinterpret_cast<t_omemo*>(user_data);
    MDB_txn *transaction = NULL;
    (void) user_record;
    std::string k_session_str = fmt::format("session_{}_{}", address->device_id, address->name);
    MDB_val k_session = { .mv_size = k_session_str.size(), .mv_data = k_session_str.data() };
    MDB_val v_session;

    if (mdb_txn_begin(omemo->db_env, NULL, MDB_RDONLY, &transaction)) {
        weechat_printf(NULL, "%sxmpp: failed to open lmdb transaction",
                       weechat_prefix("error"));
        return -1;
    }

    if (mdb_get(transaction, omemo->dbi.omemo,
                &k_session, &v_session)/* ||
        mdb_get(transaction, omemo->dbi.omemo,
                 &k_user, &v_user)*/)
    {
        mdb_txn_abort(transaction);
        return 0;
    }

    *record = signal_buffer_create((const uint8_t*)v_session.mv_data, v_session.mv_size);
  //*user_record = signal_buffer_create(v_user.mv_data, v_user.mv_size);

    if (mdb_txn_commit(transaction)) {
        weechat_printf(NULL, "%sxmpp: failed to close lmdb transaction",
                       weechat_prefix("error"));
        goto cleanup;
    };

    return 1;
cleanup:
    mdb_txn_abort(transaction);
    return -1;
}

int ss_get_sub_device_sessions_func(signal_int_list **sessions, const char *name, size_t name_len, void *user_data)
{
    auto omemo = reinterpret_cast<t_omemo*>(user_data);
    (void) name_len;
    MDB_txn *transaction = NULL;
    std::string k_device_ids_str = fmt::format("device_ids_{}", name);
    MDB_val k_device_ids = { .mv_size = k_device_ids_str.size(), .mv_data = k_device_ids_str.data() };
    MDB_val v_device_ids;

    if (mdb_txn_begin(omemo->db_env, NULL, MDB_RDONLY, &transaction)) {
        weechat_printf(NULL, "%sxmpp: failed to open lmdb transaction",
                       weechat_prefix("error"));
        return -1;
    }

    if (!mdb_get(transaction, omemo->dbi.omemo,
                 &k_device_ids, &v_device_ids))
    {
        char **argv;
        int argc, i;
        signal_int_list *list = signal_int_list_alloc();

        if (!list) {
            goto cleanup;
        }

        argv = weechat_string_split((const char*)v_device_ids.mv_data, " ", NULL, 0, 0, &argc);
        if (mdb_txn_commit(transaction)) {
            weechat_printf(NULL, "%sxmpp: failed to close lmdb transaction",
                           weechat_prefix("error"));
            goto cleanup;
        };

        for (i = 0; i < argc; i++)
        {
            char* device_id = argv[i];

            signal_int_list_push_back(list, strtol(device_id, NULL, 10));
        }

        weechat_string_free_split(argv);

        *sessions = list;
        return argc;
    }
    else
    {
        mdb_txn_abort(transaction);
        return 0;
    }
cleanup:
    mdb_txn_abort(transaction);
    return -1;
}

int ss_store_session_func(const struct signal_protocol_address *address, uint8_t *record, size_t record_len, uint8_t *user_record, size_t user_record_len, void *user_data)
{
    auto omemo = reinterpret_cast<t_omemo*>(user_data);
    MDB_txn *transaction = NULL;
    std::string k_session_str = fmt::format("session_{}_{}", address->device_id, address->name);
    MDB_val k_session = { .mv_size = k_session_str.size(), .mv_data = k_session_str.data() };
    MDB_val v_session = {.mv_size = record_len, .mv_data = record};
    (void) user_record; (void) user_record_len;

    if (mdb_txn_begin(omemo->db_env, NULL, 0, &transaction)) {
        weechat_printf(NULL, "%sxmpp: failed to open lmdb transaction",
                       weechat_prefix("error"));
        return -1;
    }

    if (mdb_put(transaction, omemo->dbi.omemo,
                &k_session, &v_session, 0)/* ||
        mdb_put(transaction, omemo->dbi.omemo,
                &k_user, &v_user, 0)*/) {
      weechat_printf(NULL, "%sxmpp: failed to write lmdb value",
                     weechat_prefix("error"));
      goto cleanup;
    };

    if (mdb_txn_commit(transaction)) {
      weechat_printf(NULL, "%sxmpp: failed to write lmdb transaction",
                     weechat_prefix("error"));
      goto cleanup;
    };

    return 0;
cleanup:
    mdb_txn_abort(transaction);
    return -1;
}

int ss_contains_session_func(const struct signal_protocol_address *address, void *user_data)
{
    auto omemo = reinterpret_cast<t_omemo*>(user_data);
    MDB_txn *transaction = NULL;
    std::string k_session_str = fmt::format("session_{}_{}", address->device_id, address->name);
    MDB_val k_session = { .mv_size = k_session_str.size(), .mv_data = k_session_str.data() };
    MDB_val v_session;

    if (mdb_txn_begin(omemo->db_env, NULL, MDB_RDONLY, &transaction)) {
        weechat_printf(NULL, "%sxmpp: failed to open lmdb transaction",
                       weechat_prefix("error"));
        return 0;
    }

    if (mdb_get(transaction, omemo->dbi.omemo, &k_session, &v_session)) {
        mdb_txn_abort(transaction);
        return 0;
    };

    mdb_txn_abort(transaction);
    return 1;
}

int ss_delete_session_func(const struct signal_protocol_address *address, void *user_data)
{
    auto omemo = reinterpret_cast<t_omemo*>(user_data);
    MDB_txn *transaction = NULL;
    std::string k_session_str = fmt::format("session_{}_{}", address->device_id, address->name);
    MDB_val k_session = { .mv_size = k_session_str.size(), .mv_data = k_session_str.data() };
    MDB_val v_session;

    if (mdb_txn_begin(omemo->db_env, NULL, 0, &transaction)) {
        weechat_printf(NULL, "%sxmpp: failed to open lmdb transaction",
                       weechat_prefix("error"));
        return -1;
    }

    if (mdb_del(transaction, omemo->dbi.omemo, &k_session, &v_session)) {
        weechat_printf(NULL, "%sxmpp: failed to erase lmdb value",
                       weechat_prefix("error"));
        goto cleanup;
    };

    if (mdb_txn_commit(transaction)) {
        weechat_printf(NULL, "%sxmpp: failed to close lmdb transaction",
                       weechat_prefix("error"));
        goto cleanup;
    };

    return 1;
cleanup:
    mdb_txn_abort(transaction);
    return -1;
}

int ss_delete_all_sessions_func(const char *name, size_t name_len, void *user_data)
{
    signal_int_list *sessions;
    ss_get_sub_device_sessions_func(&sessions, name, name_len, user_data);

    int n = signal_int_list_size(sessions);
    for (int i = 0; i < n; i++)
    {
        struct signal_protocol_address address = {.name = name, .name_len = name_len,
            .device_id = signal_int_list_at(sessions, i)};
        ss_delete_session_func(&address, user_data);
    }
    signal_int_list_free(sessions);
    return -1;
}

void ss_destroy_func(void *user_data)
{
    auto omemo = reinterpret_cast<t_omemo*>(user_data);
    (void) omemo;
    // Function called to perform cleanup when the data store context is being destroyed
}

int sks_store_sender_key(const signal_protocol_sender_key_name *sender_key_name, uint8_t *record, size_t record_len, uint8_t *user_record, size_t user_record_len, void *user_data)
{
    auto omemo = reinterpret_cast<t_omemo*>(user_data);
    char *device_list = NULL;
    MDB_txn *transaction = NULL;
    std::string k_sender_key_str = fmt::format("sender_key_{}_{}_{}", sender_key_name->group_id,
                                               sender_key_name->sender.device_id,
                                               sender_key_name->sender.name);
    MDB_val k_sender_key = { .mv_size = k_sender_key_str.size(), .mv_data = k_sender_key_str.data() };
    MDB_val v_sender_key = {.mv_size = record_len, .mv_data = record};
    (void) user_record; (void) user_record_len;
    std::string k_device_ids_str = fmt::format("device_ids_{}", sender_key_name->sender.name);
    MDB_val k_device_ids = { .mv_size = k_device_ids_str.size(), .mv_data = k_device_ids_str.data() };
    MDB_val v_device_ids;

    if (mdb_txn_begin(omemo->db_env, NULL, 0, &transaction)) {
        weechat_printf(NULL, "%sxmpp: failed to open lmdb transaction",
                       weechat_prefix("error"));
        return -1;
    }

    if (!mdb_get(transaction, omemo->dbi.omemo,
                 &k_device_ids, &v_device_ids))
    {
        char **argv;
        int argc, i;

        argv = weechat_string_split((const char*)v_device_ids.mv_data, " ", NULL, 0, 0, &argc);
        for (i = 0; i < argc; i++)
        {
            char* device_id = argv[i];
            if (strtol(device_id, NULL, 10) == sender_key_name->sender.device_id) break;
        }

        weechat_string_free_split(argv);

        if (i == argc)
        {
            size_t device_list_len = strlen((const char*)v_device_ids.mv_data) + 1 + 10 + 1;
            device_list = (char*)malloc(sizeof(char) * device_list_len);
            snprintf(device_list, device_list_len, "%s %u",
                     (char*)v_device_ids.mv_data, sender_key_name->sender.device_id);
            v_device_ids.mv_data = device_list;
            v_device_ids.mv_size = strlen(device_list) + 1;
        }
    }
    else
    {
        device_list = (char*)malloc(sizeof(char) * (10 + 1));
        snprintf(device_list, 10 + 1, "%u", sender_key_name->sender.device_id);
        v_device_ids.mv_data = device_list;
        v_device_ids.mv_size = strlen(device_list) + 1;
    }

    if (mdb_put(transaction, omemo->dbi.omemo,
                &k_sender_key, &v_sender_key, 0)/* ||
        mdb_put(transaction, omemo->dbi.omemo,
                &k_user, &v_user, 0)*/ ||
        mdb_put(transaction, omemo->dbi.omemo,
                &k_device_ids, &v_device_ids, 0)) {
      weechat_printf(NULL, "%sxmpp: failed to write lmdb value",
                     weechat_prefix("error"));
      goto cleanup;
    };

    if (mdb_txn_commit(transaction)) {
      weechat_printf(NULL, "%sxmpp: failed to write lmdb transaction",
                     weechat_prefix("error"));
      goto cleanup;
    };
    free(device_list);

    return 0;
cleanup:
    free(device_list);
    mdb_txn_abort(transaction);
    return -1;
}

int sks_load_sender_key(struct signal_buffer **record, signal_buffer **user_record, const signal_protocol_sender_key_name *sender_key_name, void *user_data)
{
    auto omemo = reinterpret_cast<t_omemo*>(user_data);
    MDB_txn *transaction = NULL;
    std::string k_sender_key_str = fmt::format("sender_key_{}_{}_{}", sender_key_name->group_id,
                                               sender_key_name->sender.device_id,
                                               sender_key_name->sender.name);
    MDB_val k_sender_key = { .mv_size = k_sender_key_str.size(), .mv_data = k_sender_key_str.data() };
    MDB_val v_sender_key;
    (void) user_record;

    if (mdb_txn_begin(omemo->db_env, NULL, 0, &transaction)) {
        weechat_printf(NULL, "%sxmpp: failed to open lmdb transaction",
                       weechat_prefix("error"));
        return -1;
    }

    if (!mdb_get(transaction, omemo->dbi.omemo,
                &k_sender_key, &v_sender_key)/* &&
        !mdb_get(transaction, omemo->dbi.omemo,
                &k_user, &v_user)*/)
    {
        *record = signal_buffer_create((const uint8_t*)v_sender_key.mv_data, v_sender_key.mv_size);
      //*user_record = signal_buffer_create(v_user.mv_data, v_user.mv_size);

        if (mdb_txn_commit(transaction)) {
            weechat_printf(NULL, "%sxmpp: failed to close lmdb transaction",
                           weechat_prefix("error"));
            goto cleanup;
        };
    }
    else
    {
        goto cleanup;
    }

    return 0;
cleanup:
    mdb_txn_abort(transaction);
    return -1;
}

void sks_destroy_func(void *user_data)
{
    auto omemo = reinterpret_cast<t_omemo*>(user_data);
    (void) omemo;
    // Function called to perform cleanup when the data store context is being destroyed
}

int dls_store_devicelist(const char *jid, signal_int_list *devicelist, t_omemo *omemo)
{
    auto transaction = lmdb::txn::begin(omemo->db_env);
    std::string k_devicelist = fmt::format("devicelist_{}", jid);
    std::string v_devicelist;

    for (size_t i = 0; i < signal_int_list_size(devicelist); i++)
    {
        int device = signal_int_list_at(devicelist, i);
        std::string device_id = std::to_string(device);
        if (v_devicelist.size() > 0)
            v_devicelist += ";";
        v_devicelist += device_id;
    }

    omemo->dbi.omemo.put(transaction, lmdb::val{k_devicelist}, lmdb::val{v_devicelist});
  //omemo->dbi.omemo.put(wtxn, "fullname", std::string_view("J. Random Hacker"));
  //{
  //    auto cursor = lmdb::cursor::open(rtxn, dbi);
  //    std::string_view key, value;
  //    if (cursor.get(key, value, MDB_FIRST)) {
  //        do {
  //            std::cout << "key: " << key << "  value: " << value << std::endl;
  //        } while (cursor.get(key, value, MDB_NEXT));
  //    }
  //}

    transaction.commit();

    return 0;
}

int dls_load_devicelist(signal_int_list **devicelist, const char *jid, t_omemo *omemo)
{
    auto transaction = lmdb::txn::begin(omemo->db_env);
    std::string k_devicelist = fmt::format("devicelist_{}", jid);
    lmdb::val k{k_devicelist}, v{};
    std::string v_devicelist;
    if (omemo->dbi.omemo.get(transaction, k, v))
        v_devicelist.assign(v.data(), v.size());

    auto devices = std::string_view{v_devicelist}
        | std::ranges::views::split(';')
        | std::ranges::views::transform([](auto&& str) {
            return std::stoul(std::string(&*str.begin(), std::ranges::distance(str)));
        });

    *devicelist = signal_int_list_alloc();
    for (uint32_t&& device_id : devices)
    {
        signal_int_list_push_back(*devicelist, device_id);
    }

    transaction.commit();

    return 0;
}

int bks_store_bundle(struct signal_protocol_address *address,
        struct t_pre_key **pre_keys, struct t_pre_key **signed_pre_keys,
        const char *signature, const char *identity_key, t_omemo *omemo)
{
    size_t n_pre_keys = -1;
    while (pre_keys[++n_pre_keys] != NULL);
    auto pre_key_buffers = std::vector<std::string>();
    pre_key_buffers.reserve(n_pre_keys);
    for (auto pre_key : std::vector<struct t_pre_key*>(pre_keys, pre_keys + n_pre_keys))
    {
        pre_key_buffers.push_back(fmt::format("{},{}", pre_key->id, pre_key->public_key));
    }

    n_pre_keys = -1;
    while (signed_pre_keys[++n_pre_keys] != NULL);
    auto signed_pre_key_buffers = std::vector<std::string>();
    signed_pre_key_buffers.reserve(n_pre_keys);
    for (auto signed_pre_key : std::vector<struct t_pre_key*>(signed_pre_keys, signed_pre_keys + n_pre_keys))
    {
        signed_pre_key_buffers.push_back(fmt::format("{},{}", signed_pre_key->id, signed_pre_key->public_key));

        uint8_t *signing_key_raw = nullptr;
        size_t signing_key_len = base64_decode(identity_key,
                strlen(identity_key), &signing_key_raw);
        heap_buf signing_key_buf = make_heap_buf(signing_key_raw);
        libsignal::public_key signing_key(signing_key_raw,
                signing_key_len, omemo->context);

        uint8_t *signed_key_raw = nullptr;
        size_t signed_key_len = base64_decode(signed_pre_key->public_key,
                strlen(signed_pre_key->public_key), &signed_key_raw);
        heap_buf signed_key_buf = make_heap_buf(signed_key_raw);
        uint8_t *signature_raw = nullptr;
        size_t signature_len = base64_decode(signature,
                strlen(signature), &signature_raw);
        heap_buf signature_buf = make_heap_buf(signature_raw);
        int valid = curve_verify_signature(signing_key,
                signed_key_raw, signed_key_len,
                signature_raw, signature_len);
        if (valid <= 0) {
            weechat_printf(NULL, "%somemo: failed to validate ED25519 signature for %s:%u",
                           weechat_prefix("error"), address->name, address->device_id);
        }
    }

    std::string k_bundle_pk = fmt::format("bundle_pk_{}_{}", address->name, address->device_id);
    std::string k_bundle_sk = fmt::format("bundle_sk_{}_{}", address->name, address->device_id);
    std::string k_bundle_sg = fmt::format("bundle_sg_{}_{}", address->name, address->device_id);
    std::string k_bundle_ik = fmt::format("bundle_ik_{}_{}", address->name, address->device_id);

    std::string v_bundle_pk = std::accumulate(pre_key_buffers.begin(), pre_key_buffers.end(), std::string(""),
        [](const std::string& a, const std::string& b) { return a.empty() ? b : a + ";" + b; });
    std::string v_bundle_sk = std::accumulate(signed_pre_key_buffers.begin(), signed_pre_key_buffers.end(), std::string(""),
        [](const std::string& a, const std::string& b) { return a.empty() ? b : a + ";" + b; });
    auto transaction = lmdb::txn::begin(omemo->db_env);

    omemo->dbi.omemo.put(transaction, lmdb::val{k_bundle_pk}, lmdb::val{v_bundle_pk});
    omemo->dbi.omemo.put(transaction, lmdb::val{k_bundle_sk}, lmdb::val{v_bundle_sk});
    {
        lmdb::val k{k_bundle_sg}, v{signature, std::strlen(signature)};
        omemo->dbi.omemo.put(transaction, k, v);
    }
    {
        lmdb::val k{k_bundle_ik}, v{identity_key, std::strlen(identity_key)};
        omemo->dbi.omemo.put(transaction, k, v);
    }

    transaction.commit();

    return 0;
}

std::optional<libsignal::pre_key_bundle> bks_load_bundle(struct signal_protocol_address *address, t_omemo *omemo)
{
    std::string k_bundle_pk = fmt::format("bundle_pk_{}_{}", address->name, address->device_id);
    std::string k_bundle_sk = fmt::format("bundle_sk_{}_{}", address->name, address->device_id);
    std::string k_bundle_sg = fmt::format("bundle_sg_{}_{}", address->name, address->device_id);
    std::string k_bundle_ik = fmt::format("bundle_ik_{}_{}", address->name, address->device_id);

    std::string v_bundle_pk;
    std::string v_bundle_sk;
    std::string v_bundle_sg;
    std::string v_bundle_ik;

    auto transaction = lmdb::txn::begin(omemo->db_env);

    uint32_t pre_key_id = 0;
    uint32_t signed_pre_key_id = 0;
    uint8_t *sig_raw = nullptr; size_t sig_len;
    struct signal_buffer *signature;
    uint8_t *key_raw = nullptr; size_t key_len;

    {
        lmdb::val k{k_bundle_pk}, v{};
        if (omemo->dbi.omemo.get(transaction, k, v)) v_bundle_pk.assign(v.data(), v.size());
    }
    {
        lmdb::val k{k_bundle_sk}, v{};
        if (omemo->dbi.omemo.get(transaction, k, v)) v_bundle_sk.assign(v.data(), v.size());
    }
    {
        lmdb::val k{k_bundle_sg}, v{};
        if (omemo->dbi.omemo.get(transaction, k, v)) v_bundle_sg.assign(v.data(), v.size());
    }
    {
        lmdb::val k{k_bundle_ik}, v{};
        if (omemo->dbi.omemo.get(transaction, k, v)) v_bundle_ik.assign(v.data(), v.size());
    }

    auto r_bundle_pks = v_bundle_pk
        | std::ranges::views::split(';')
        | std::ranges::views::transform([](auto&& str) {
            return std::string_view(&*str.begin(), std::ranges::distance(str));
        });
    auto bundle_pks = std::vector<std::string>{r_bundle_pks.begin(), r_bundle_pks.end()};
    heap_buf pre_key_raw_buf{nullptr, free};
    if (bundle_pks.size() > 0)
    {
        std::istringstream iss(bundle_pks[rand() % bundle_pks.size()]);
        iss >> pre_key_id;
        char delim;
        iss.get(delim);
        if (delim != ',') throw std::runtime_error("Bundle parse failure");
        std::string key_data;
        iss >> key_data;
        key_len = base64_decode(key_data.data(), key_data.size(), &key_raw);
        pre_key_raw_buf = make_heap_buf(key_raw);
    }
    else
        return {};
    libsignal::public_key pre_key(key_raw, key_len, omemo->context);

    auto r_bundle_sks = v_bundle_sk
        | std::ranges::views::split(';')
        | std::ranges::views::transform([](auto&& str) {
            return std::string_view(&*str.begin(), std::ranges::distance(str));
        });
    auto bundle_sks = std::vector<std::string>{r_bundle_sks.begin(), r_bundle_sks.end()};
    heap_buf signed_key_raw_buf{nullptr, free};
    if (bundle_sks.size() > 0)
    {
        std::istringstream iss(bundle_sks[rand() % bundle_sks.size()]);
        iss >> signed_pre_key_id;
        char delim;
        iss.get(delim);
        if (delim != ',') throw std::runtime_error("Bundle parse failure");
        std::string key_data;
        iss >> key_data;
        key_len = base64_decode(key_data.data(), key_data.size(), &key_raw);
        signed_key_raw_buf = make_heap_buf(key_raw);
    }
    else
        return {};
    libsignal::public_key signed_pre_key(key_raw, key_len, omemo->context);

    sig_len = base64_decode(v_bundle_sg.data(), v_bundle_sg.size(), &sig_raw);
    heap_buf sig_raw_buf = make_heap_buf(sig_raw);
    signature = signal_buffer_create(sig_raw, sig_len);
    key_len = base64_decode(v_bundle_ik.data(), v_bundle_ik.size(), &key_raw);
    heap_buf ik_raw_buf = make_heap_buf(key_raw);
    libsignal::public_key identity_key(key_raw, key_len, omemo->context);

    libsignal::pre_key_bundle bundle((uint32_t)address->device_id, (int)address->device_id,
                                     (uint32_t)pre_key_id, *pre_key, (uint32_t)signed_pre_key_id, *signed_pre_key,
                                     (const uint8_t*)signal_buffer_data(signature), (size_t)signal_buffer_len(signature),
                                     *identity_key);

    transaction.commit();

    return bundle;
}

void log_emit_weechat(int level, const char *message, size_t len, void *user_data)
{
    struct t_gui_buffer *buffer = (struct t_gui_buffer*)user_data;

    static const char *log_level_name[5] = {"error", "warn", "notice", "info", "debug"};

    const char *tags = level < SG_LOG_DEBUG ? "no_log" : NULL;

    (void)buffer;
    weechat_printf_date_tags(
        NULL, 0, tags,
        _("%somemo (%s): %.*s"),
        weechat_prefix("network"),
        log_level_name[level], len, message);
}

xmpp_stanza_t *omemo::get_bundle(xmpp_ctx_t *context, char *from, char *to)
{
    auto omemo = this;

    auto children_buf = std::make_unique<xmpp_stanza_t*[]>(101);
    xmpp_stanza_t **children = children_buf.get();
    xmpp_stanza_t *parent = NULL;
    struct signal_buffer *record = NULL;
    ec_key_pair *keypair = NULL;
    ec_public_key *public_key = NULL;

    int num_keys = 0;
    for (uint32_t id = PRE_KEY_START;
            id < INT_MAX && num_keys < 100; id++)
    {
        if (pks_load_pre_key(&record, id, omemo) != 0) continue;
        else num_keys++;
        session_pre_key *pre_key = NULL;
        session_pre_key_deserialize(&pre_key, signal_buffer_data(record),
                signal_buffer_len(record), omemo->context);
        if (pre_key == 0) {
            weechat_printf(NULL, "%somemo: failed to deserialize pre_key %u in get_bundle, skipping",
                           weechat_prefix("error"), id);
            signal_buffer_free(record);
            continue;
        }
        signal_buffer_free(record);
        keypair = session_pre_key_get_key_pair(pre_key);
        public_key = ec_key_pair_get_public(keypair);
        ec_public_key_serialize(&record, public_key);
        char *data = NULL;
        base64_encode(signal_buffer_data(record),
                signal_buffer_len(record), &data);
        signal_buffer_free(record);
        if (pre_key) session_pre_key_destroy((signal_type_base*)pre_key);
        if (!data) {
            weechat_printf(NULL, "%somemo: base64_encode failed for pre_key %u, skipping",
                           weechat_prefix("error"), id);
            continue;
        }
        std::string id_str_s = fmt::format("{}", id);
        children[num_keys-1] = stanza__iq_pubsub_publish_item_bundle_prekeys_preKeyPublic(
                context, NULL, NULL, with_noop(id_str_s.c_str()));
        stanza__set_text(context, children[num_keys-1], with_free(data));
    }
    children[100] = NULL;

    children[3] = stanza__iq_pubsub_publish_item_bundle_prekeys(
            context, NULL, children);
    children[4] = NULL;

    // Signed pre-key rotation: check if current SPK is older than 7 days.
    // We store the current SPK ID in "signed_pre_key_current_id" and its creation
    // timestamp in "signed_pre_key_ts_<id>".
    static constexpr time_t SPK_MAX_AGE_SECS = 7 * 24 * 60 * 60;

    uint32_t current_spk_id = 1; // default: ID 1 (initial key)
    {
        auto txn = lmdb::txn::begin(omemo->db_env);
        lmdb::val k{"signed_pre_key_current_id"}, v{};
        if (omemo->dbi.omemo.get(txn, k, v) && v.size() > 0)
            current_spk_id = (uint32_t)std::stoul(std::string(v.data(), v.size()));
        txn.abort();
    }

    // Read creation timestamp for current SPK
    time_t spk_ts = 0;
    {
        auto txn = lmdb::txn::begin(omemo->db_env);
        std::string k_ts = fmt::format("signed_pre_key_ts_{}", current_spk_id);
        lmdb::val k{k_ts}, v{};
        if (omemo->dbi.omemo.get(txn, k, v) && v.size() > 0)
            spk_ts = (time_t)std::stoll(std::string(v.data(), v.size()));
        txn.abort();
    }

    time_t now = time(NULL);
    if (spk_ts == 0 || (now - spk_ts) > SPK_MAX_AGE_SECS)
    {
        // Generate a new signed pre-key with the next ID
        uint32_t new_spk_id = current_spk_id + 1;
        session_signed_pre_key *new_spk = NULL;
        int rc = signal_protocol_key_helper_generate_signed_pre_key(
                &new_spk, omemo->identity, new_spk_id, now, omemo->context);
        if (rc == 0 && new_spk)
        {
            struct signal_buffer *serialized = NULL;
            session_signed_pre_key_serialize(&serialized, new_spk);
            spks_store_signed_pre_key(new_spk_id,
                    signal_buffer_data(serialized), signal_buffer_len(serialized), omemo);
            signal_buffer_free(serialized);
            session_pre_key_destroy((signal_type_base*)new_spk);

            // Update current ID and timestamp in LMDB
            auto txn = lmdb::txn::begin(omemo->db_env);
            std::string v_new_id = std::to_string(new_spk_id);
            std::string v_new_ts = std::to_string((long long)now);
            std::string k_ts = fmt::format("signed_pre_key_ts_{}", new_spk_id);
            omemo->dbi.omemo.put(txn, lmdb::val{"signed_pre_key_current_id"}, lmdb::val{v_new_id});
            omemo->dbi.omemo.put(txn, lmdb::val{k_ts}, lmdb::val{v_new_ts});
            txn.commit();

            current_spk_id = new_spk_id;
            weechat_printf(NULL, "%somemo: rotated signed pre-key to ID %u",
                           weechat_prefix("network"), new_spk_id);
        }
        else
        {
            // If timestamp was just missing (first run), record it now
            auto txn = lmdb::txn::begin(omemo->db_env);
            std::string v_ts_str = std::to_string((long long)now);
            std::string k_ts = fmt::format("signed_pre_key_ts_{}", current_spk_id);
            std::string v_spk_id_str = std::to_string(current_spk_id);
            omemo->dbi.omemo.put(txn, lmdb::val{k_ts}, lmdb::val{v_ts_str});
            omemo->dbi.omemo.put(txn, lmdb::val{"signed_pre_key_current_id"}, lmdb::val{v_spk_id_str});
            txn.commit();
            if (rc != 0)
                weechat_printf(NULL, "%somemo: failed to generate new signed pre-key (rc=%d), keeping ID %u",
                               weechat_prefix("error"), rc, current_spk_id);
        }
    }

    spks_load_signed_pre_key(&record, current_spk_id, omemo);
    session_signed_pre_key *signed_pre_key;
    session_signed_pre_key_deserialize(&signed_pre_key,
            signal_buffer_data(record), signal_buffer_len(record),
            omemo->context);
    signal_buffer_free(record);
    uint32_t signed_pre_key_id = session_signed_pre_key_get_id(signed_pre_key);
    keypair = session_signed_pre_key_get_key_pair(signed_pre_key);
    public_key = ec_key_pair_get_public(keypair);
    ec_public_key_serialize(&record, public_key);
    char *signed_pre_key_public = NULL;
    base64_encode(signal_buffer_data(record), signal_buffer_len(record),
            &signed_pre_key_public);
    signal_buffer_free(record);
    std::string signed_pre_key_id_str_s = fmt::format("{}", signed_pre_key_id);
    char *signed_pre_key_id_str = signed_pre_key_id_str_s.data();
    children[0] = stanza__iq_pubsub_publish_item_bundle_signedPreKeyPublic(
            context, NULL, NULL, with_noop(signed_pre_key_id_str));
    stanza__set_text(context, children[0], with_free(signed_pre_key_public));

    const uint8_t *keysig = session_signed_pre_key_get_signature(signed_pre_key);
    size_t keysig_len = session_signed_pre_key_get_signature_len(signed_pre_key);
    char *signed_pre_key_signature = NULL;
    base64_encode(keysig, keysig_len, &signed_pre_key_signature);
    session_pre_key_destroy((signal_type_base*)signed_pre_key);
    children[1] = stanza__iq_pubsub_publish_item_bundle_signedPreKeySignature(
            context, NULL, NULL);
    stanza__set_text(context, children[1], with_free(signed_pre_key_signature));

    struct signal_buffer *private_key_buf = NULL;
    iks_get_identity_key_pair(&record, (signal_buffer**)&private_key_buf, omemo);
    signal_buffer_free(private_key_buf);
    char *identity_key = NULL;
    base64_encode(signal_buffer_data(record), signal_buffer_len(record),
            &identity_key);
    signal_buffer_free(record);
    children[2] = stanza__iq_pubsub_publish_item_bundle_identityKey(
            context, NULL, NULL);
    stanza__set_text(context, children[2], with_free(identity_key));

    children[0] = stanza__iq_pubsub_publish_item_bundle(
            context, NULL, children, with_noop("eu.siacs.conversations.axolotl"));
    children[1] = NULL;

    children[0] = stanza__iq_pubsub_publish_item(
            context, NULL, children, with_noop("current"));

    std::string bundle_node_s = fmt::format("eu.siacs.conversations.axolotl.bundles:{}", omemo->device_id);
    children[0] = stanza__iq_pubsub_publish(
            context, NULL, children, with_noop(bundle_node_s.c_str()));

    omemo->handle_bundle(from, omemo->device_id, children[0]);

    children[0] = stanza__iq_pubsub(
            context, NULL, children, with_noop("http://jabber.org/protocol/pubsub"));

    // Add publish-options: access_model=open + persist_items=true so servers
    // deliver PubSub notifications to contacts and allow them to fetch our bundle.
    {
        xmpp_stanza_t *pubsub = children[0];

        auto make_field = [&](const char *var, const char *val, const char *type = nullptr) {
            xmpp_stanza_t *field = xmpp_stanza_new(context);
            xmpp_stanza_set_name(field, "field");
            xmpp_stanza_set_attribute(field, "var", var);
            if (type) xmpp_stanza_set_attribute(field, "type", type);
            xmpp_stanza_t *value = xmpp_stanza_new(context);
            xmpp_stanza_set_name(value, "value");
            xmpp_stanza_t *text = xmpp_stanza_new(context);
            xmpp_stanza_set_text(text, val);
            xmpp_stanza_add_child(value, text);
            xmpp_stanza_release(text);
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
        xmpp_stanza_t *f2 = make_field("pubsub#persist_items", "true");
        xmpp_stanza_t *f3 = make_field("pubsub#access_model", "open");

        xmpp_stanza_add_child(x, f1); xmpp_stanza_release(f1);
        xmpp_stanza_add_child(x, f2); xmpp_stanza_release(f2);
        xmpp_stanza_add_child(x, f3); xmpp_stanza_release(f3);

        xmpp_stanza_t *publish_options = xmpp_stanza_new(context);
        xmpp_stanza_set_name(publish_options, "publish-options");
        xmpp_stanza_add_child(publish_options, x);
        xmpp_stanza_release(x);

        xmpp_stanza_add_child(pubsub, publish_options);
        xmpp_stanza_release(publish_options);
    }

    parent = stanza__iq(
        context, NULL, children, NULL, "announce2", from, to, "set");
    // children_buf auto-frees here
    return parent;
}

void omemo::init(struct t_gui_buffer *buffer, const char *account_name)
{
    gcrypt::check_version();

    const auto omemo = this;

    omemo->context.create(buffer);
    omemo->context.set_log_function(&log_emit_weechat);

    try {
        omemo->db_path = std::shared_ptr<char>(
            weechat_string_eval_expression("${weechat_data_dir}/xmpp/omemo.db",
                                           NULL, NULL, NULL),
            &free).get();
        std::filesystem::path path(omemo->db_path.data());
        std::filesystem::create_directories(
                std::filesystem::path(omemo->db_path.data()).parent_path());

        omemo->db_env = lmdb::env::create();
        omemo->db_env.set_max_dbs(50);
        omemo->db_env.set_mapsize((size_t)1048576 * 8000); // 8000MB map for valgrind
        omemo->db_env.open(omemo->db_path.data(), MDB_NOSUBDIR, 0664);

        lmdb::txn parentTransaction{nullptr};
        lmdb::txn transaction = lmdb::txn::begin(omemo->db_env, parentTransaction);

        std::string db_name = fmt::format("omemo_{}", account_name);
        omemo->dbi.omemo = lmdb::dbi::open(transaction, db_name.data(), MDB_CREATE);

        transaction.commit();
    } catch (const lmdb::error& ex) {
        auto format = fmt::format("%sxmpp: lmdb failure - {}", ex.what());
        weechat_printf(NULL, format.data(), weechat_prefix("error"));
        throw;
    }

    struct signal_crypto_provider crypto_provider = {
        .random_func = &cp_random_generator,
        .hmac_sha256_init_func = &cp_hmac_sha256_init,
        .hmac_sha256_update_func = &cp_hmac_sha256_update,
        .hmac_sha256_final_func = &cp_hmac_sha256_final,
        .hmac_sha256_cleanup_func = &cp_hmac_sha256_cleanup,
        .sha512_digest_init_func = &cp_sha512_digest_init,
        .sha512_digest_update_func = &cp_sha512_digest_update,
        .sha512_digest_final_func = &cp_sha512_digest_final,
        .sha512_digest_cleanup_func = &cp_sha512_digest_cleanup,
        .encrypt_func = &cp_encrypt,
        .decrypt_func = &cp_decrypt,
        .user_data = omemo,
    };

    omemo->context.set_crypto_provider(&crypto_provider);
    omemo->context.set_locking_functions(&lock_function, &unlock_function);

    omemo->store_context.create(omemo->context);

    struct signal_protocol_identity_key_store identity_key_store = {
        .get_identity_key_pair = &iks_get_identity_key_pair,
        .get_local_registration_id = &iks_get_local_registration_id,
        .save_identity = &iks_save_identity,
        .is_trusted_identity = &iks_is_trusted_identity,
        .destroy_func = &iks_destroy_func,
        .user_data = omemo,
    };

    omemo->store_context.set_identity_key_store(&identity_key_store);

    struct signal_protocol_pre_key_store pre_key_store = {
        .load_pre_key = &pks_load_pre_key,
        .store_pre_key = &pks_store_pre_key,
        .contains_pre_key = &pks_contains_pre_key,
        .remove_pre_key = &pks_remove_pre_key,
        .destroy_func = &pks_destroy_func,
        .user_data = omemo,
    };

    omemo->store_context.set_pre_key_store(&pre_key_store);

    struct signal_protocol_signed_pre_key_store signed_pre_key_store = {
        .load_signed_pre_key = &spks_load_signed_pre_key,
        .store_signed_pre_key = &spks_store_signed_pre_key,
        .contains_signed_pre_key = &spks_contains_signed_pre_key,
        .remove_signed_pre_key = &spks_remove_signed_pre_key,
        .destroy_func = &spks_destroy_func,
        .user_data = omemo,
    };

    omemo->store_context.set_signed_pre_key_store(&signed_pre_key_store);

    struct signal_protocol_session_store session_store = {
        .load_session_func = &ss_load_session_func,
        .get_sub_device_sessions_func = &ss_get_sub_device_sessions_func,
        .store_session_func = &ss_store_session_func,
        .contains_session_func = &ss_contains_session_func,
        .delete_session_func = &ss_delete_session_func,
        .delete_all_sessions_func = &ss_delete_all_sessions_func,
        .destroy_func = &ss_destroy_func,
        .user_data = omemo,
    };

    omemo->store_context.set_session_store(&session_store);

    struct signal_protocol_sender_key_store sender_key_store = {
        .store_sender_key = &sks_store_sender_key,
        .load_sender_key = &sks_load_sender_key,
        .destroy_func = &sks_destroy_func,
        .user_data = omemo,
    };

    omemo->store_context.set_sender_key_store(&sender_key_store);

    struct signal_buffer *public_data = nullptr, *private_data = nullptr;
    iks_get_local_registration_id(omemo, &omemo->device_id);
    if (!iks_get_identity_key_pair(&public_data, &private_data, omemo))
    {
        // Decode raw key pointers (refcount=1 each after decode).
        // ratchet_identity_key_pair_create() calls SIGNAL_REF on both,
        // bringing them to refcount=2.  We then SIGNAL_UNREF them back to 1
        // so that only the identity pair owns them.  Do NOT wrap them in
        // libsignal::public_key / private_key RAII here: those destructors
        // call ec_*_destroy = free() directly, bypassing the refcount, which
        // would free the keys while the pair still holds live pointers,
        // causing a SIGABRT in ratchet_identity_key_pair_destroy later.
        ec_public_key *pub_key = nullptr;
        ec_private_key *priv_key = nullptr;
        // public_data came from ec_public_key_serialize(), which stores the
        // full 33-byte serialized form: [0x05][32 raw bytes].
        // curve_decode_point() expects exactly this prefixed form — no +1 skip.
        curve_decode_point(&pub_key,
                signal_buffer_data(public_data),
                signal_buffer_len(public_data),
                omemo->context);
        curve_decode_private_point(&priv_key,
                signal_buffer_data(private_data),
                signal_buffer_len(private_data),
                omemo->context);
        signal_buffer_free(public_data);
        signal_buffer_free(private_data);
        if (pub_key && priv_key)
        {
            omemo->identity.create(pub_key, priv_key);
            // SIGNAL_UNREF: pair now owns both (refcount 2→1 each)
            SIGNAL_UNREF(pub_key);
            SIGNAL_UNREF(priv_key);
        }
        else
        {
            if (pub_key) SIGNAL_UNREF(pub_key);
            if (priv_key) SIGNAL_UNREF(priv_key);
            weechat_printf(buffer, "%somemo: failed to decode identity keys",
                           weechat_prefix("error"));
        }
    }
    weechat_printf(buffer, "%somemo: device = %d",
                   weechat_prefix("info"), omemo->device_id);
}

void omemo::handle_devicelist(const char *jid, xmpp_stanza_t *items)
{
    auto omemo = this;

    xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(items, "item");
    if (!item) return;
    xmpp_stanza_t *list = xmpp_stanza_get_child_by_name(item, "list");
    if (!list) return;
    signal_int_list *devicelist = signal_int_list_alloc();
    for (xmpp_stanza_t *device = xmpp_stanza_get_children(list);
         device; device = xmpp_stanza_get_next(device))
    {
        const char *name = xmpp_stanza_get_name(device);
        if (weechat_strcasecmp(name, "device") != 0)
            continue;

        const char *device_id = xmpp_stanza_get_id(device);
        if (!device_id)
            continue;

        signal_int_list_push_back(devicelist, strtol(device_id, NULL, 10));
    }
    if (dls_store_devicelist(jid, devicelist, omemo))
        weechat_printf(NULL, "%somemo: failed to handle devicelist (%s)",
                       weechat_prefix("error"), jid);
    signal_int_list_free(devicelist);
}

void omemo::handle_bundle(const char *jid, uint32_t device_id,
                          xmpp_stanza_t *items)
{
    auto omemo = this;
    xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(items, "item");
    if (!item) return;
    xmpp_stanza_t *bundle = xmpp_stanza_get_child_by_name(item, "bundle");
    if (!bundle) return;
    xmpp_stanza_t *signedprekey = xmpp_stanza_get_child_by_name(bundle, "signedPreKeyPublic");
    if (!signedprekey) return;
    xmpp_string_guard signed_pre_key_g(xmpp_stanza_get_context(signedprekey), xmpp_stanza_get_text(signedprekey));
    const char *signed_pre_key = signed_pre_key_g.ptr;
    if (!signed_pre_key) return;
    const char *signed_pre_key_id = xmpp_stanza_get_attribute(signedprekey, "signedPreKeyId");
    if (!signed_pre_key_id) return;
    xmpp_stanza_t *signature = xmpp_stanza_get_child_by_name(bundle, "signedPreKeySignature");
    if (!signature) return;
    xmpp_string_guard key_signature_g(xmpp_stanza_get_context(signature), xmpp_stanza_get_text(signature));
    const char *key_signature = key_signature_g.ptr;
    if (!key_signature) return;
    xmpp_stanza_t *identitykey = xmpp_stanza_get_child_by_name(bundle, "identityKey");
    if (!identitykey) return;
    xmpp_string_guard identity_key_g(xmpp_stanza_get_context(identitykey), xmpp_stanza_get_text(identitykey));
    const char *identity_key = identity_key_g.ptr;
    if (!identity_key) return;
    xmpp_stanza_t *prekeys = xmpp_stanza_get_child_by_name(bundle, "prekeys");
    if (!prekeys) return;

    int num_prekeys = 0;
    for (xmpp_stanza_t *prekey = xmpp_stanza_get_children(prekeys);
         prekey; prekey = xmpp_stanza_get_next(prekey))
        num_prekeys++;
    // Use vectors for RAII: no malloc/free, no null-terminator off-by-one bug.
    std::vector<t_pre_key> pre_key_storage;
    pre_key_storage.reserve(num_prekeys);
    std::vector<t_pre_key *> pre_keys_ptrs;
    pre_keys_ptrs.reserve(num_prekeys + 1); // +1 for null terminator

    char **format = weechat_string_dyn_alloc(256);
    weechat_string_dyn_concat(format, "omemo bundle %s/%u:\n%s..SPK %u: %s\n%3$s..SKS: %s\n%3$s..IK: %s", -1);
    std::vector<xmpp_string_guard> pre_key_guards;
    for (xmpp_stanza_t *prekey = xmpp_stanza_get_children(prekeys);
         prekey; prekey = xmpp_stanza_get_next(prekey))
    {
        const char *name = xmpp_stanza_get_name(prekey);
        if (weechat_strcasecmp(name, "preKeyPublic") != 0)
            continue;

        const char *pre_key_id = xmpp_stanza_get_attribute(prekey, "preKeyId");
        if (!pre_key_id)
            continue;
        pre_key_guards.emplace_back(xmpp_stanza_get_context(prekey), xmpp_stanza_get_text(prekey));
        const char *pre_key = pre_key_guards.back().ptr;
        if (!pre_key)
        {
            pre_key_guards.pop_back();
            continue;
        }

        pre_key_storage.push_back({.id = pre_key_id, .public_key = pre_key});
        pre_keys_ptrs.push_back(&pre_key_storage.back());

        weechat_string_dyn_concat(format, "\n%3$s..PK ", -1);
        weechat_string_dyn_concat(format, pre_key_id, -1);
        weechat_string_dyn_concat(format, ": ", -1);
        weechat_string_dyn_concat(format, pre_key, -1);
    }
    pre_keys_ptrs.push_back(nullptr); // null terminator for bks_store_bundle
    struct t_pre_key **pre_keys = pre_keys_ptrs.data();
    weechat_string_dyn_free(format, 1);

    struct t_pre_key signed_key = {
        .id = signed_pre_key_id,
        .public_key = signed_pre_key,
    };
    struct t_pre_key *signed_pre_keys[2] = { &signed_key, NULL };

    struct signal_protocol_address address = {
        .name = jid, .name_len = strlen(jid), .device_id = (int32_t)device_id };
    {
        uint8_t *key_buf;
        size_t key_len = base64_decode(identity_key,
                strlen(identity_key), &key_buf);
        libsignal::public_key key(key_buf, key_len, omemo->context);
        signal_protocol_identity_save_identity(omemo->store_context,
                &address, key);
    }
    bks_store_bundle(&address, pre_keys, signed_pre_keys,
        key_signature, identity_key, omemo);
}

char *omemo::decode(weechat::account *account, struct t_gui_buffer *buffer,
                    const char *jid,
                     xmpp_stanza_t *encrypted)
{
    auto omemo = &account->omemo;
    heap_buf iv_data_buf{nullptr, free};
    // key_data_buf owns the calloc'd base64-decoded key; reset to null once
    // ownership transfers to libsignal (aes_key_buf below).
    heap_buf key_data_buf{nullptr, free};
    // aes_key_buf owns the signal_buffer returned by session_cipher_decrypt_*.
    std::unique_ptr<signal_buffer, decltype(&signal_buffer_free)>
        aes_key_buf{nullptr, signal_buffer_free};
    uint8_t *key_data = NULL, *tag_data = NULL, *iv_data = NULL, *payload_data = NULL;
    size_t key_len = 0, tag_len = 0, iv_len = 0, payload_len = 0;

    xmpp_stanza_t *header = xmpp_stanza_get_child_by_name(encrypted, "header");
    if (!header)
        return NULL;
    xmpp_stanza_t *iv = xmpp_stanza_get_child_by_name(header, "iv");
    if (!iv)
        return NULL;
    xmpp_string_guard iv__text_g(xmpp_stanza_get_context(iv), xmpp_stanza_get_text(iv));
    const char *iv__text = iv__text_g.c_str();
    if (!iv__text) return NULL;
    iv_len = base64_decode(iv__text, strlen(iv__text), &iv_data);
    iv_data_buf.reset(iv_data);
    if (iv_len != AES_IV_SIZE) return NULL;

    dyn_string_ptr format_g(weechat_string_dyn_alloc(256));
    char **format = format_g.get();
    weechat_string_dyn_concat(format, "omemo msg %s:\n%s..IV: %s", -1);
    int keys_found = 0, keys_for_this_device = 0;
    bool decrypted_ok = false;
    
    for (xmpp_stanza_t *key = xmpp_stanza_get_children(header);
         key; key = xmpp_stanza_get_next(key))
    {
        const char *name = xmpp_stanza_get_name(key);
        if (weechat_strcasecmp(name, "key") != 0)
            continue;

        keys_found++;
        const char *key_prekey = xmpp_stanza_get_attribute(key, "prekey");
        const char *key_id = xmpp_stanza_get_attribute(key, "rid");
        if (!key_id)
            continue;
        if (strtol(key_id, NULL, 10) != omemo->device_id)
            continue;
        keys_for_this_device++;
        xmpp_stanza_t *key_text = xmpp_stanza_get_children(key);
        xmpp_string_guard data_g(xmpp_stanza_get_context(key), key_text ? xmpp_stanza_get_text(key_text) : nullptr);
        const char *data = data_g.c_str();
        if (!data)
            continue;
        free(key_data); key_data = NULL;
        key_len = base64_decode(data, strlen(data), &key_data);
        key_data_buf.reset(key_data);

        weechat_string_dyn_concat(format, "\n%2$s..K ", -1);
        if (key_prekey)
            weechat_string_dyn_concat(format, "*", -1);
        weechat_string_dyn_concat(format, key_id, -1);
        weechat_string_dyn_concat(format, ": ", -1);
        weechat_string_dyn_concat(format, data, -1);

        const char *source_id = xmpp_stanza_get_attribute(header, "sid");
        if (!source_id)
            continue;

        int ret;
        struct signal_protocol_address address = {
            .name = jid, .name_len = strlen(jid), .device_id = (int32_t)strtol(source_id, NULL, 10) };
        signal_message *key_message = NULL;
        struct signal_buffer *aes_key = NULL;
        aes_key_buf.reset(nullptr); // reset before each iteration
        
        if (key_prekey) {
            pre_key_signal_message *pre_key_message = NULL;
            if ((ret = pre_key_signal_message_deserialize(&pre_key_message,
                key_data, key_len, omemo->context))) {
                weechat_printf(buffer, "%somemo: failed to deserialize pre-key message from %s device %s (ret=%d)",
                               weechat_prefix("error"), jid, source_id, ret);
                return NULL;
            }
            ec_public_key *identity_key = pre_key_signal_message_get_identity_key(pre_key_message);
          //uint32_t device_id = pre_key_signal_message_get_registration_id(pre_key_message);
          //uint32_t pre_key_id = pre_key_signal_message_get_pre_key_id(pre_key_message);
          //uint32_t signed_key_id = pre_key_signal_message_get_signed_pre_key_id(pre_key_message);
          //ec_public_key *base_key = pre_key_signal_message_get_base_key(pre_key_message);
            key_message = pre_key_signal_message_get_signal_message(pre_key_message);
            struct signal_buffer *identity_buf;
            if ((ret = ec_public_key_serialize(&identity_buf, identity_key))) {
                weechat_printf(buffer, "%somemo: failed to serialize identity key from %s device %s (ret=%d)",
                               weechat_prefix("error"), jid, source_id, ret);
                SIGNAL_UNREF(pre_key_message);
                return NULL;
            }
            if ((ret = iks_save_identity(&address, signal_buffer_data(identity_buf),
                                    signal_buffer_len(identity_buf), omemo))) {
                weechat_printf(buffer, "%somemo: failed to save identity from %s device %s (ret=%d)",
                               weechat_prefix("error"), jid, source_id, ret);
                signal_buffer_free(identity_buf);
                SIGNAL_UNREF(pre_key_message);
                return NULL;
            }
            signal_buffer_free(identity_buf);

            struct session_cipher *cipher;
            if ((ret = session_cipher_create(&cipher, omemo->store_context,
                                        &address, omemo->context))) {
                weechat_printf(buffer, "%somemo: failed to create cipher for %s device %s (ret=%d)",
                               weechat_prefix("error"), jid, source_id, ret);
                SIGNAL_UNREF(pre_key_message);
                return NULL;
            }
            if ((ret = session_cipher_decrypt_pre_key_signal_message(cipher,
                                                                pre_key_message,
                                                                0, &aes_key))) {
                weechat_printf(buffer, "%somemo: pre-key decryption failed from %s device %s (ret=%d)",
                               weechat_prefix("error"), jid, source_id, ret);
                session_cipher_free(cipher);
                SIGNAL_UNREF(pre_key_message);
                return NULL;
            }
            session_cipher_free(cipher);
            SIGNAL_UNREF(pre_key_message);
            aes_key_buf.reset(aes_key);
            weechat_printf(buffer, "%somemo: new session established with %s device %s",
                           weechat_prefix("network"), jid, source_id);
        } else {
            if ((ret = signal_message_deserialize(&key_message,
                key_data, key_len, omemo->context))) {
                weechat_printf(buffer, "%somemo: failed to deserialize message from %s device %s (ret=%d)",
                               weechat_prefix("error"), jid, source_id, ret);
                return NULL;
            }
            struct session_cipher *cipher;
            if ((ret = session_cipher_create(&cipher, omemo->store_context,
                                        &address, omemo->context))) {
                weechat_printf(buffer, "%somemo: failed to create cipher for %s device %s (ret=%d)",
                               weechat_prefix("error"), jid, source_id, ret);
                SIGNAL_UNREF(key_message);
                return NULL;
            }
            if ((ret = session_cipher_decrypt_signal_message(cipher, key_message,
                                                        0, &aes_key))) {
                weechat_printf(buffer, "%somemo: decryption failed from %s device %s (ret=%d)",
                               weechat_prefix("error"), jid, source_id, ret);
                session_cipher_free(cipher);
                SIGNAL_UNREF(key_message);
                return NULL;
            }
            session_cipher_free(cipher);
            SIGNAL_UNREF(key_message);
            aes_key_buf.reset(aes_key);
        }
        decrypted_ok = true;

        if (!aes_key) return NULL;
        // Release the calloc'd buffer — aes_key_buf now owns the signal_buffer.
        // key_data/tag_data become non-owning views into aes_key's storage.
        key_data_buf.release();
        key_data = signal_buffer_data(aes_key);
        key_len = signal_buffer_len(aes_key);
        if (key_len >= AES_KEY_SIZE) {
            tag_len = key_len - AES_KEY_SIZE;
            tag_data = key_data + AES_KEY_SIZE;
            key_len = AES_KEY_SIZE;
        }
        else
        {
            return NULL;
        }

        char *aes_key64 = NULL;
        if (base64_encode(key_data, key_len, &aes_key64) && aes_key64)
        {
            weechat_string_dyn_concat(format, "\n%2$s..AES: ", -1);
            weechat_string_dyn_concat(format, aes_key64, -1);
            weechat_string_dyn_concat(format, " (", -1);
            snprintf(aes_key64, strlen(aes_key64), "%lu", key_len);
            weechat_string_dyn_concat(format, aes_key64, -1);
            weechat_string_dyn_concat(format, ")", -1);
            free(aes_key64); aes_key64 = NULL;
        }
        if (tag_len && base64_encode(tag_data, tag_len, &aes_key64) && aes_key64)
        {
            weechat_string_dyn_concat(format, "\n%2$s..TAG: ", -1);
            weechat_string_dyn_concat(format, aes_key64, -1);
            weechat_string_dyn_concat(format, " (", -1);
            snprintf(aes_key64, strlen(aes_key64), "%lu", tag_len);
            weechat_string_dyn_concat(format, aes_key64, -1);
            weechat_string_dyn_concat(format, ")", -1);
            free(aes_key64); aes_key64 = NULL;
        }
    }

    if (!decrypted_ok) {
        if (keys_for_this_device == 0) {
            weechat_printf(buffer, "%somemo: message from %s not encrypted for our device %u (%d keys total)",
                           weechat_prefix("error"), jid, omemo->device_id, keys_found);
            // Remote client has no session for us — republish our bundle and
            // devicelist so it re-establishes a session on the next message.
            std::string jid_str(account->jid());
            xmpp_stanza_t *bundle_stanza = omemo->get_bundle(account->context, jid_str.data(), NULL);
            if (bundle_stanza) {
                account->connection.send(bundle_stanza);
                xmpp_stanza_release(bundle_stanza);
            }
            xmpp_stanza_t *dl_stanza = account->get_devicelist();
            if (dl_stanza) {
                xmpp_string_guard dl_uuid_g(account->context, xmpp_uuid_gen(account->context));
                xmpp_stanza_set_id(dl_stanza, dl_uuid_g.ptr);
                account->connection.send(dl_stanza);
                xmpp_stanza_release(dl_stanza);
            }
        }
        return NULL;
    }

    // payload_data_buf must outlive the if-block so payload_data stays valid
    // through the aes_decrypt call below.
    heap_buf payload_data_buf{nullptr, free};
    xmpp_stanza_t *payload = xmpp_stanza_get_child_by_name(encrypted, "payload");
    if (payload && (payload = xmpp_stanza_get_children(payload)))
    {
        xmpp_string_guard payload_text_g(xmpp_stanza_get_context(payload), xmpp_stanza_get_text(payload));
        const char *payload_text = payload_text_g.c_str();
        if (!payload_text) {
            return NULL;
        }
        payload_len = base64_decode(payload_text, strlen(payload_text), &payload_data);
        payload_data_buf = make_heap_buf(payload_data);
        weechat_string_dyn_concat(format, "\n%2$s..PL: ", -1);
        weechat_string_dyn_concat(format, payload_text, -1);
    }
    
    if (!(payload_data && iv_data && key_data)) {
        weechat_printf(buffer, "%somemo: incomplete encrypted message from %s (missing %s%s%s)",
                       weechat_prefix("error"), jid,
                       payload_data ? "" : "payload ",
                       iv_data ? "" : "iv ",
                       key_data ? "" : "key");
        return NULL;
    }
    if (iv_len != AES_IV_SIZE || key_len != AES_KEY_SIZE) {
        weechat_printf(buffer, "%somemo: bad key/iv size from %s (iv=%zu want %d, key=%zu want %d)",
                       weechat_prefix("error"), jid, iv_len, AES_IV_SIZE, key_len, AES_KEY_SIZE);
        return NULL;
    }
    char *plaintext = NULL; size_t plaintext_len = 0;
    if (aes_decrypt(payload_data, payload_len, key_data, iv_data, tag_data, tag_len,
                    (uint8_t**)&plaintext, &plaintext_len))
    {
        plaintext[plaintext_len] = '\0';

        // After successful decryption: check if a pre-key was consumed and the
        // bundle needs re-publication.
        {
            auto txn = lmdb::txn::begin(omemo->db_env);
            lmdb::val k{"pre_key_repub_needed"}, v{};
            bool needs_repub = omemo->dbi.omemo.get(txn, k, v)
                               && v.size() == 1 && v.data()[0] == '1';
            txn.abort();

            if (needs_repub)
            {
                std::string from_str(account->jid());
                xmpp_stanza_t *bundle_stanza = omemo->get_bundle(account->context, from_str.data(), NULL);
                if (bundle_stanza)
                {
                    account->connection.send(bundle_stanza);
                    xmpp_stanza_release(bundle_stanza);
                    weechat_printf(buffer, "%somemo: re-published bundle after pre-key consumption",
                                   weechat_prefix("network"));
                }
                // Clear the flag
                auto wtxn = lmdb::txn::begin(omemo->db_env);
                omemo->dbi.omemo.del(wtxn, lmdb::val{"pre_key_repub_needed"});
                wtxn.commit();
            }
        }

        return plaintext;
    }
    weechat_printf(buffer, "%somemo: AES-GCM decryption failed for message from %s (auth tag mismatch?)",
                   weechat_prefix("error"), jid);
    return NULL;
}

xmpp_stanza_t *omemo::encode(weechat::account *account, struct t_gui_buffer *buffer,
                             const char *jid, const char *unencrypted)
{
    auto omemo = &account->omemo;
    uint8_t *key = NULL; uint8_t *iv = NULL;
    uint8_t *tag = NULL; size_t tag_len = 0;
    uint8_t *ciphertext = NULL; size_t ciphertext_len = 0;
    aes_encrypt((uint8_t*)unencrypted, strlen(unencrypted),
                &key, &iv, &tag, &tag_len,
                &ciphertext, &ciphertext_len);

    auto key_and_tag_buf = std::unique_ptr<uint8_t[], decltype(&free)>(
        static_cast<uint8_t*>(malloc(sizeof(uint8_t) * (AES_KEY_SIZE + tag_len))), free);
    if (!key_and_tag_buf) {
        free(key); free(tag); free(iv); free(ciphertext);
        return NULL;
    }
    uint8_t *key_and_tag = key_and_tag_buf.get();
    memcpy(key_and_tag, key, AES_KEY_SIZE);
    free(key);
    memcpy(key_and_tag+AES_KEY_SIZE, tag, tag_len);
    free(tag);
    char *key64 = NULL;
    base64_encode(key_and_tag, AES_KEY_SIZE+tag_len, &key64);
    std::unique_ptr<char, decltype(&free)> key64_buf(key64, free);
    char *iv64 = NULL;
    base64_encode(iv, AES_IV_SIZE, &iv64);
    free(iv);
    std::unique_ptr<char, decltype(&free)> iv64_buf(iv64, free);
    char *ciphertext64 = NULL;
    base64_encode(ciphertext, ciphertext_len, &ciphertext64);
    free(ciphertext);
    std::unique_ptr<char, decltype(&free)> ciphertext64_buf(ciphertext64, free);

    xmpp_stanza_t *encrypted = xmpp_stanza_new(account->context);
    xmpp_stanza_set_name(encrypted, "encrypted");
    xmpp_stanza_set_ns(encrypted, "eu.siacs.conversations.axolotl");
    xmpp_stanza_t *header = xmpp_stanza_new(account->context);
    xmpp_stanza_set_name(header, "header");
    char device_id_str[10+1] = {0};
    snprintf(device_id_str, 10+1, "%u", omemo->device_id);
    xmpp_stanza_set_attribute(header, "sid", device_id_str);

    int keycount = 0;
    signal_int_list *devicelist;
    const char *target = jid;
    for (int self = 0; self <= 1; self++)
    {
        if (dls_load_devicelist(&devicelist, target, omemo)) return NULL;
        for (size_t i = 0; i < signal_int_list_size(devicelist); i++)
        {
            uint32_t device_id = signal_int_list_at(devicelist, i);
            if (!device_id) continue;
            struct signal_protocol_address address = {
                .name = target, .name_len = strlen(target), .device_id = (int32_t)device_id};

            xmpp_stanza_t *header__key = xmpp_stanza_new(account->context);
            xmpp_stanza_set_name(header__key, "key");
            char device_id_str[10+1] = {0};
            snprintf(device_id_str, 10+1, "%u", device_id);
            xmpp_stanza_set_attribute(header__key, "rid", device_id_str);

            if (ss_contains_session_func(&address, omemo) <= 0)
            {
                try {
                    auto bundle = bks_load_bundle(&address, omemo);
                    if (!bundle) throw std::runtime_error(fmt::format("No bundle for {} device {}", target, device_id));

                    libsignal::session_builder builder(omemo->store_context, &address, omemo->context);
                    builder.process_pre_key_bundle(*bundle);
                    weechat_printf(buffer, "%somemo: new session established with %s device %u",
                                   weechat_prefix("network"), target, device_id);
                }
                catch (const std::exception& ex) {
                    weechat_printf(buffer, "%somemo: cannot establish session with %s device %u: %s",
                                   weechat_prefix("error"), target, device_id, ex.what());
                    xmpp_stanza_release(header__key);
                    continue;
                }
            }

            struct session_cipher *cipher;
            if (session_cipher_create(&cipher, omemo->store_context, &address, omemo->context)) continue;

            struct ciphertext_message *signal_message;
            if (session_cipher_encrypt(cipher, key_and_tag, AES_KEY_SIZE+tag_len, &signal_message)) continue;
            struct signal_buffer *record = ciphertext_message_get_serialized(signal_message);
            int prekey = ciphertext_message_get_type(signal_message) == CIPHERTEXT_PREKEY_TYPE
                ? 1 : 0;

            char *payload = NULL;
            base64_encode(signal_buffer_data(record), signal_buffer_len(record),
                    &payload);

            if (prekey)
                xmpp_stanza_set_attribute(header__key, "prekey",
                        prekey ? "true" : "false");
            stanza__set_text(account->context, header__key, with_free(payload));
            xmpp_stanza_add_child(header, header__key);
            xmpp_stanza_release(header__key);

            if (target == jid)
                keycount++;

            SIGNAL_UNREF(signal_message);
            session_cipher_free(cipher);
        }
        signal_int_list_free(devicelist);
        target = account->jid().data();
    }
    // key_and_tag_buf releases key_and_tag here automatically

    if (keycount == 0) {
        weechat_printf(NULL, "omemo: no keys for %s", jid);
        return NULL;
    }

    xmpp_stanza_t *header__iv = xmpp_stanza_new(account->context);
    xmpp_stanza_set_name(header__iv, "iv");
    stanza__set_text(account->context, header__iv, with_noop(iv64));
    xmpp_stanza_add_child(header, header__iv);
    xmpp_stanza_release(header__iv);
    xmpp_stanza_add_child(encrypted, header);
    xmpp_stanza_release(header);
    xmpp_stanza_t *encrypted__payload = xmpp_stanza_new(account->context);
    xmpp_stanza_set_name(encrypted__payload, "payload");
    stanza__set_text(account->context, encrypted__payload, with_noop(ciphertext64));
    xmpp_stanza_add_child(encrypted, encrypted__payload);
    xmpp_stanza_release(encrypted__payload);

    // iv64_buf, key64_buf, ciphertext64_buf auto-free on return
    return encrypted;
}

// ---------------------------------------------------------------------------
// Key management helpers
// ---------------------------------------------------------------------------

void omemo::show_fingerprint(struct t_gui_buffer *buffer, const char *jid)
{
    auto *self = static_cast<t_omemo*>(this);

    if (!jid)
    {
        // Show own public identity key fingerprint
        MDB_txn *txn = NULL;
        if (mdb_txn_begin(self->db_env, NULL, MDB_RDONLY, &txn)) {
            weechat_printf(buffer, "%sOMEMO: failed to open LMDB transaction",
                           weechat_prefix("error"));
            return;
        }
        MDB_val k_pub = mdb_val_str("local_public_key");
        MDB_val v_pub;
        if (mdb_get(txn, self->dbi.omemo, &k_pub, &v_pub) != 0) {
            mdb_txn_abort(txn);
            weechat_printf(buffer, "%sOMEMO: own identity key not found (not initialized?)",
                           weechat_prefix("error"));
            return;
        }
        // Format as colon-separated hex groups of 4 bytes (Conversations style)
        const uint8_t *data = static_cast<const uint8_t*>(v_pub.mv_data);
        size_t len = v_pub.mv_size;
        std::string hex;
        for (size_t i = 0; i < len; i++) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", data[i]);
            if (i > 0 && i % 4 == 0) hex += ' ';
            hex += buf;
        }
        mdb_txn_abort(txn);
        weechat_printf(buffer, "%sOMEMO own fingerprint (device %u):\n%s  %s",
                       weechat_prefix("network"), self->device_id, weechat_prefix("network"), hex.c_str());
        return;
    }

    // Show stored identity keys for a peer JID (all device IDs we have)
    // Scan for keys matching "identity_key_<jid>_*"
    MDB_txn *txn = NULL;
    if (mdb_txn_begin(self->db_env, NULL, MDB_RDONLY, &txn)) {
        weechat_printf(buffer, "%sOMEMO: failed to open LMDB transaction",
                       weechat_prefix("error"));
        return;
    }

    MDB_cursor *cursor = NULL;
    if (mdb_cursor_open(txn, self->dbi.omemo, &cursor)) {
        mdb_txn_abort(txn);
        weechat_printf(buffer, "%sOMEMO: failed to open LMDB cursor",
                       weechat_prefix("error"));
        return;
    }

    std::string prefix = fmt::format("identity_key_{}_", jid);
    MDB_val k_seek = { .mv_size = prefix.size(), .mv_data = (void*)prefix.c_str() };
    MDB_val v_iter;
    bool found_any = false;

    int rc = mdb_cursor_get(cursor, &k_seek, &v_iter, MDB_SET_RANGE);
    while (rc == 0) {
        std::string_view key(static_cast<const char*>(k_seek.mv_data), k_seek.mv_size);
        if (key.substr(0, prefix.size()) != prefix) break;

        std::string_view device_part = key.substr(prefix.size());
        const uint8_t *data = static_cast<const uint8_t*>(v_iter.mv_data);
        size_t len = v_iter.mv_size;
        std::string hex;
        for (size_t i = 0; i < len; i++) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", data[i]);
            if (i > 0 && i % 4 == 0) hex += ' ';
            hex += buf;
        }
        weechat_printf(buffer, "%sOMEMO fingerprint for %s (device %.*s):\n%s  %s",
                       weechat_prefix("network"), jid,
                       static_cast<int>(device_part.size()), device_part.data(),
                       weechat_prefix("network"), hex.c_str());
        found_any = true;
        rc = mdb_cursor_get(cursor, &k_seek, &v_iter, MDB_NEXT);
    }

    mdb_cursor_close(cursor);
    mdb_txn_abort(txn);

    if (!found_any)
        weechat_printf(buffer, "%sOMEMO: no stored identity keys for %s",
                       weechat_prefix("network"), jid);
}

void omemo::distrust_jid(struct t_gui_buffer *buffer, const char *jid)
{
    auto *self = static_cast<t_omemo*>(this);

    MDB_txn *txn = NULL;
    if (mdb_txn_begin(self->db_env, NULL, 0, &txn)) {
        weechat_printf(buffer, "%sOMEMO: failed to open LMDB transaction",
                       weechat_prefix("error"));
        return;
    }

    MDB_cursor *cursor = NULL;
    if (mdb_cursor_open(txn, self->dbi.omemo, &cursor)) {
        mdb_txn_abort(txn);
        weechat_printf(buffer, "%sOMEMO: failed to open LMDB cursor",
                       weechat_prefix("error"));
        return;
    }

    std::string prefix = fmt::format("identity_key_{}_", jid);
    MDB_val k_seek = { .mv_size = prefix.size(), .mv_data = (void*)prefix.c_str() };
    MDB_val v_iter;
    int deleted = 0;

    int rc = mdb_cursor_get(cursor, &k_seek, &v_iter, MDB_SET_RANGE);
    while (rc == 0) {
        std::string_view key(static_cast<const char*>(k_seek.mv_data), k_seek.mv_size);
        if (key.substr(0, prefix.size()) != prefix) break;

        rc = mdb_cursor_del(cursor, 0);
        if (rc != 0) {
            weechat_printf(buffer, "%sOMEMO: failed to delete key: %s",
                           weechat_prefix("error"), mdb_strerror(rc));
            break;
        }
        deleted++;
        rc = mdb_cursor_get(cursor, &k_seek, &v_iter, MDB_SET_RANGE);
    }

    mdb_cursor_close(cursor);
    if (mdb_txn_commit(txn)) {
        weechat_printf(buffer, "%sOMEMO: failed to commit LMDB transaction",
                       weechat_prefix("error"));
        return;
    }

    if (deleted > 0)
        weechat_printf(buffer, "%sOMEMO: removed %d stored identity key(s) for %s — "
                       "next contact will trigger TOFU re-verification",
                       weechat_prefix("network"), deleted, jid);
    else
        weechat_printf(buffer, "%sOMEMO: no stored identity keys found for %s",
                       weechat_prefix("network"), jid);
}

void omemo::show_devices(struct t_gui_buffer *buffer, const char *jid)
{
    auto *self = static_cast<t_omemo*>(this);

    try {
        auto txn = lmdb::txn::begin(self->db_env, nullptr, MDB_RDONLY);
        std::string k_devicelist = fmt::format("devicelist_{}", jid);
        lmdb::val k{k_devicelist}, v{};
        std::string v_devicelist;

        if (!self->dbi.omemo.get(txn, k, v) || v.size() == 0) {
            txn.abort();
            weechat_printf(buffer, "%sOMEMO: no known devices for %s",
                           weechat_prefix("network"), jid);
            return;
        }
        v_devicelist.assign(v.data(), v.size());
        txn.abort();

        auto devices = std::string_view{v_devicelist}
            | std::ranges::views::split(';')
            | std::ranges::views::transform([](auto&& r) {
                return std::string(r.begin(), r.end());
            });

        weechat_printf(buffer, "%sOMEMO devices for %s:",
                       weechat_prefix("network"), jid);
        for (auto&& dev_str : devices) {
            if (dev_str.empty()) continue;
            weechat_printf(buffer, "%s  device %s",
                           weechat_prefix("network"), dev_str.c_str());
        }
    } catch (const lmdb::error& ex) {
        weechat_printf(buffer, "%sOMEMO: LMDB error: %s",
                       weechat_prefix("error"), ex.what());
    }
}

void omemo::show_status(struct t_gui_buffer *buffer, const char *account_name,
                        const char *channel_name, int channel_omemo_enabled)
{
    auto *self = static_cast<t_omemo*>(this);

    weechat_printf(buffer, "%sOMEMO status for account %s:",
                   weechat_prefix("network"), account_name ? account_name : "?");

    // Device ID
    weechat_printf(buffer, "%s  Device ID: %u",
                   weechat_prefix("network"), self->device_id);

    // Own fingerprint
    {
        MDB_txn *txn = NULL;
        if (mdb_txn_begin(self->db_env, NULL, MDB_RDONLY, &txn) == 0) {
            MDB_val k_pub = mdb_val_str("local_public_key");
            MDB_val v_pub;
            if (mdb_get(txn, self->dbi.omemo, &k_pub, &v_pub) == 0) {
                const uint8_t *data = static_cast<const uint8_t*>(v_pub.mv_data);
                size_t len = v_pub.mv_size;
                std::string hex;
                for (size_t i = 0; i < len; i++) {
                    char buf[3];
                    snprintf(buf, sizeof(buf), "%02x", data[i]);
                    if (i > 0 && i % 4 == 0) hex += ' ';
                    hex += buf;
                }
                weechat_printf(buffer, "%s  Own fingerprint: %s",
                               weechat_prefix("network"), hex.c_str());
            } else {
                weechat_printf(buffer, "%s  Own fingerprint: (not found in LMDB)",
                               weechat_prefix("network"));
            }
            mdb_txn_abort(txn);
        } else {
            weechat_printf(buffer, "%s  Own fingerprint: (LMDB error)",
                           weechat_prefix("network"));
        }
    }

    // Pre-key count — cursor scan for "pre_key_" prefix
    {
        MDB_txn *txn = NULL;
        if (mdb_txn_begin(self->db_env, NULL, MDB_RDONLY, &txn) == 0) {
            MDB_cursor *cursor = NULL;
            int prekey_count = 0;
            if (mdb_cursor_open(txn, self->dbi.omemo, &cursor) == 0) {
                std::string_view prefix = "pre_key_";
                MDB_val k_seek = { .mv_size = prefix.size(),
                                   .mv_data = (void*)prefix.data() };
                MDB_val v_iter;
                int rc = mdb_cursor_get(cursor, &k_seek, &v_iter, MDB_SET_RANGE);
                while (rc == 0) {
                    std::string_view key(static_cast<const char*>(k_seek.mv_data),
                                        k_seek.mv_size);
                    if (key.substr(0, prefix.size()) != prefix) break;
                    // Exclude "pre_key_repub_needed" — only count numeric IDs
                    std::string_view suffix = key.substr(prefix.size());
                    if (!suffix.empty() && suffix[0] >= '0' && suffix[0] <= '9')
                        prekey_count++;
                    rc = mdb_cursor_get(cursor, &k_seek, &v_iter, MDB_NEXT);
                }
                mdb_cursor_close(cursor);
            }
            mdb_txn_abort(txn);
            weechat_printf(buffer, "%s  Pre-keys remaining: %d",
                           weechat_prefix("network"), prekey_count);
        }
    }

    // Signed pre-key current ID and age
    {
        MDB_txn *txn = NULL;
        if (mdb_txn_begin(self->db_env, NULL, MDB_RDONLY, &txn) == 0) {
            MDB_val k_id = mdb_val_str("signed_pre_key_current_id");
            MDB_val v_id;
            if (mdb_get(txn, self->dbi.omemo, &k_id, &v_id) == 0 && v_id.mv_size > 0) {
                std::string spk_id_str(static_cast<const char*>(v_id.mv_data), v_id.mv_size);
                // Strip trailing null if present
                while (!spk_id_str.empty() && spk_id_str.back() == '\0')
                    spk_id_str.pop_back();

                std::string ts_key = fmt::format("signed_pre_key_ts_{}", spk_id_str);
                MDB_val k_ts = { .mv_size = ts_key.size(), .mv_data = (void*)ts_key.c_str() };
                MDB_val v_ts;
                if (mdb_get(txn, self->dbi.omemo, &k_ts, &v_ts) == 0 && v_ts.mv_size > 0) {
                    std::string ts_str(static_cast<const char*>(v_ts.mv_data), v_ts.mv_size);
                    while (!ts_str.empty() && ts_str.back() == '\0')
                        ts_str.pop_back();
                    try {
                        long long ts = std::stoll(ts_str);
                        long long now = static_cast<long long>(std::time(nullptr));
                        long long age_days = (now - ts) / 86400;
                        weechat_printf(buffer,
                                       "%s  Signed pre-key ID: %s (age: %lld day%s)",
                                       weechat_prefix("network"),
                                       spk_id_str.c_str(), age_days,
                                       age_days == 1 ? "" : "s");
                    } catch (...) {
                        weechat_printf(buffer,
                                       "%s  Signed pre-key ID: %s (age: unknown)",
                                       weechat_prefix("network"), spk_id_str.c_str());
                    }
                } else {
                    weechat_printf(buffer, "%s  Signed pre-key ID: %s (no timestamp)",
                                   weechat_prefix("network"), spk_id_str.c_str());
                }
            } else {
                weechat_printf(buffer, "%s  Signed pre-key ID: (not found)",
                               weechat_prefix("network"));
            }
            mdb_txn_abort(txn);
        }
    }

    // Channel encryption state
    if (channel_name) {
        weechat_printf(buffer, "%s  Channel '%s' OMEMO: %s",
                       weechat_prefix("network"), channel_name,
                       channel_omemo_enabled ? "ENABLED" : "disabled");
    }
}

omemo::~omemo()
{
    // Note: Member destructors (identity_key_pair, db_env, etc.) will run automatically
    // Don't try to skip cleanup with early return - member destructors run anyway
}
