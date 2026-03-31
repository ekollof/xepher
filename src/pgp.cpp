// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <memory>
#include <numeric>
#include <fmt/core.h>
#include <gpgme.h>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "pgp.hh"

std::string format_key(weechat::xmpp::pgp &pgp, std::string_view keyid)
{
    gpgme_key_t key = nullptr;
    gpgme_error_t err = gpgme_get_key(pgp.gpgme, keyid.data(), &key, false);
    if (err) {
        return fmt::format("{} (none)", keyid);
    }
    std::string result(keyid);
    result += '{';
    {
        std::string keyids;
        for (auto subkey = key->subkeys; subkey; subkey = subkey->next)
        {
            if (!keyids.empty()) keyids += ", ";
            std::string keyid(subkey->keyid);
            if (keyid.length() > 8) keyid = keyid.substr(keyid.length()-8, 8);
            keyids += keyid;
        }
        result += keyids;
    }
    result += "}[";
    {
        std::string userids;
        for (auto uid = key->uids; uid; uid = uid->next)
        {
            if (!userids.empty()) userids += ", ";
            userids += fmt::format("{} ({})", uid->name, uid->email);
        }
        result += userids;
    }
    result += ']';
    return result;
}

#define PGP_MESSAGE_HEADER "-----BEGIN PGP MESSAGE-----\r\n"
#define PGP_MESSAGE_FOOTER "\r\n-----END PGP MESSAGE-----"
#define PGP_SIGNATURE_HEADER "-----BEGIN PGP SIGNATURE-----\r\n"
#define PGP_SIGNATURE_FOOTER "\r\n-----END PGP SIGNATURE-----"

static constexpr std::string_view kPgpMessageHeader   { PGP_MESSAGE_HEADER };
static constexpr std::string_view kPgpMessageFooter   { PGP_MESSAGE_FOOTER };
static constexpr std::string_view kPgpSignatureHeader { PGP_SIGNATURE_HEADER };
static constexpr std::string_view kPgpSignatureFooter { PGP_SIGNATURE_FOOTER };

const char *weechat::xmpp::PGP_ADVICE = "[PGP encrypted message (XEP-0027)]";

weechat::xmpp::pgp::pgp()
{
    gpgme_error_t err;
  //gpgme_data_t keydata;

    gpgme_check_version(NULL);

    err = gpgme_new(&this->gpgme);
    if (err) {
        weechat_printf(nullptr, "gpg (error): %s - %s",
                gpgme_strsource(err), gpgme_strerror(err));
        throw nullptr;
    }
    gpgme_set_armor(this->gpgme, true);

  //err = gpgme_data_new_from_file(&keydata, pub, true);
  //if (err) {
  //    return;
  //}

  //err = gpgme_op_import(this->gpgme, keydata);
  //if (err) {
  //    return;
  //}

  //gpgme_import_result_t impRes = gpgme_op_import_result(this->gpgme);
  //weechat_printf(nullptr, "(gpg) imported %d keys", impRes->imported);

  //err = gpgme_data_new_from_file(&keydata, sec, true);
  //if (err) {
  //    return;
  //}

  //err = gpgme_op_import(this->gpgme, keydata);
  //if (err) {
  //    return;
  //}

  //impRes = gpgme_op_import_result(this->gpgme);
  //weechat_printf(nullptr, "(gpg) imported %d secret keys", impRes->imported);
}

weechat::xmpp::pgp::~pgp()
{
    // Safety check: if plugin is destroyed, skip cleanup
    if (!weechat::plugin::instance || !weechat::plugin::instance->ptr())
        return;
        
    gpgme_release(this->gpgme);
}

std::optional<std::string> weechat::xmpp::pgp::encrypt(struct t_gui_buffer *buffer, std::string_view source, std::vector<std::string>&& targets, std::string_view message)
{
    struct data_guard {
        gpgme_data_t h = nullptr;
        ~data_guard() { if (h) gpgme_data_release(h); }
    } in_g, out_g;

    std::string encrypted;
    gpgme_key_t keys[3] = {nullptr, nullptr, nullptr};
    gpgme_error_t err;

    err = gpgme_data_new_from_mem(&in_g.h, message.data(), message.size(), false);
    if (err) goto encrypt_finish;

    err = gpgme_data_new(&out_g.h);
    if (err) goto encrypt_finish;

    for (const std::string& target : targets)
    {
        err = gpgme_get_key(this->gpgme, target.data(), &keys[0], false);
        if (err) goto encrypt_finish;
    }
    err = gpgme_get_key(this->gpgme, source.data(), &keys[1], false);
    if (err) goto encrypt_finish;

    err = gpgme_op_encrypt(this->gpgme, keys, GPGME_ENCRYPT_ALWAYS_TRUST, in_g.h, out_g.h);
    if (err) goto encrypt_finish;

    if (gpgme_encrypt_result_t enc_result = gpgme_op_encrypt_result(this->gpgme);
            enc_result->invalid_recipients)
        goto encrypt_finish;

    {
        gpgme_data_seek(out_g.h, 0, SEEK_SET);
        char data[512 + 1];
        int ret;
        while ((ret = gpgme_data_read(out_g.h, data, 512)) > 0)
            encrypted += std::string_view(data, ret);
    }

encrypt_finish:
    if (err) {
        weechat_printf(buffer, "[PGP]\t%s - %s",
                gpgme_strsource(err), gpgme_strerror(err));
        return std::nullopt;
    }
    if (encrypted.size() <= kPgpMessageHeader.size() + kPgpMessageFooter.size())
        return std::nullopt;
    return std::string(encrypted.data() + kPgpMessageHeader.size(),
                       encrypted.size() - kPgpMessageHeader.size() - kPgpMessageFooter.size());
}

//"hQIMAzlgcSFDGLKEAQ//cGG3DFughC5xBF7xeXz1RdayOfhBAPfoZIq62MVuSnfS\nMfig65Zxz1LtAnnFq90TZY7hiHPBtVlYqg47AbSoYweMdpXsKgbUrd3NNf6k2nsZ\nUkChCtyGuHi8pTzclfle7gT0nNXJ1WcLCZ4ORZCrg3D5A+YTO9tdmE8GQsTT6TdV\nbbxF5yR4JF5SzFhuFL3ZoXPXrWylcwKXarYfoOTa6M2vSsCwApVIXQgJ/FI46sLT\nb0B/EVCjFvcvjkNr7+K7mQtth+x0a0pC4BtEhRvnIRAe/sdGp8NY+DP76clx4U+k\nIDG4H92F632pR6eEIoZttnBoaj0O4sTVAJCao5AoecR4w2FDqBWWtIyQp5vbo17/\nMtzungkk5vQP6Jhu36wa+JKpbHoxomVpHPZfAtIoyaY6pzQ0bUomIlSVpbZDvF68\nZKTlFd89Pm5x0JO5gsVYvf+N9Ed33d34n/0CFz5K5Tgu4Bk0v4LWEy3wtNsuQB4p\nkBSZJk7I2BakcRwP0zwld6rRHFIX1pb7zqThBPZGB9RkWPltiktUTibOII12tWhi\nksFpQJ8l1A8h9vM5kUXIeD6H2yP0CBUEIZF3Sf+jiSRZ/1/n3KoUrKEzkf/y4xgv\n1LA4pMjNLEr6J2fqGyYRFv4Bxv3PIvF17V5CwOtguxGRJHJXdIzm1BSHSqXxHezS\nYAFXMUb9fw3QX7Ed23KiyZjzd/LRsQBqMs9RsYyZB2PqF9x84lQYYbE8lErrryvK\nUEtmJKPw3Hvb7kgGox5vl5+KCg9q64EU9TgQpufYNShKtDz7Fsvc+ncgZoshDUeo\npw==\n=euIB"
std::optional<std::string> weechat::xmpp::pgp::decrypt(struct t_gui_buffer *buffer, std::string_view ciphertext)
{
    struct data_guard {
        gpgme_data_t h = nullptr;
        ~data_guard() { if (h) gpgme_data_release(h); }
    } in_g, out_g;

    std::string decrypted;
    std::string keyids;
    gpgme_error_t err;

    std::string buf = fmt::format(PGP_MESSAGE_HEADER "{}" PGP_MESSAGE_FOOTER, ciphertext);
    err = gpgme_data_new_from_mem(&in_g.h, buf.data(), buf.size(), false);
    if (err) goto decrypt_finish;

    err = gpgme_data_new(&out_g.h);
    if (err) goto decrypt_finish;

    err = gpgme_op_decrypt(this->gpgme, in_g.h, out_g.h);
    if (gpgme_decrypt_result_t dec_result = gpgme_op_decrypt_result(this->gpgme); dec_result)
    {
        for (auto recip = dec_result->recipients; recip; recip = recip->next)
        {
            if (!keyids.empty()) keyids += ", ";
            keyids += format_key(*this, recip->keyid);
        }
        if (dec_result->unsupported_algorithm)
            goto decrypt_finish;
    }
    if (err) goto decrypt_finish;

    {
        gpgme_data_seek(out_g.h, 0, SEEK_SET);
        char data[512 + 1];
        int ret;
        while ((ret = gpgme_data_read(out_g.h, data, 512)) > 0)
            decrypted += std::string_view(data, ret);
    }

decrypt_finish:
    if (err) {
        weechat_printf(buffer, "[PGP]\t%s - %s (%s)",
                gpgme_strsource(err), gpgme_strerror(err), keyids.data());
        return std::nullopt;
    }
    return decrypted;
}

std::optional<std::string> weechat::xmpp::pgp::verify(struct t_gui_buffer *buffer, std::string_view certificate)
{
    struct data_guard {
        gpgme_data_t h = nullptr;
        ~data_guard() { if (h) gpgme_data_release(h); }
    } in_g, out_g;

    std::optional<std::string> result;
    gpgme_verify_result_t vrf_result = nullptr;
    gpgme_error_t err;

    std::string buf = fmt::format(PGP_SIGNATURE_HEADER "{}" PGP_SIGNATURE_FOOTER, certificate);
    err = gpgme_data_new_from_mem(&in_g.h, buf.data(), buf.size(), false);
    if (err) goto verify_finish;

    err = gpgme_data_new(&out_g.h);
    if (err) goto verify_finish;

    err = gpgme_op_verify(this->gpgme, in_g.h, out_g.h, nullptr);
    if (err) goto verify_finish;

    if (vrf_result = gpgme_op_verify_result(this->gpgme);
            !(vrf_result->signatures->summary & GPGME_SIGSUM_VALID))
    {
      //goto verify_finish;
    }

    result = std::string(vrf_result->signatures->fpr);

    {
        gpgme_key_t key = nullptr;
        err = gpgme_get_key(this->gpgme, result->c_str(), &key, false);
        if (err) {
            const char *keyids[2] = { result->c_str(), nullptr };
            // Attempt to fetch the key from a keyserver; if it fails (e.g.
            // GPG_ERR_NO_DATA from Dirmngr), clear the error silently —
            // a keyserver miss is not user-actionable.
            gpgme_error_t fetch_err = gpgme_op_receive_keys(this->gpgme, keyids);
            if (fetch_err)
                err = GPG_ERR_NO_ERROR;
        } else {
            gpgme_key_release(key);
        }
    }

verify_finish:
    if (err) {
        weechat_printf(buffer, "[PGP]\t%s - %s",
                gpgme_strsource(err), gpgme_strerror(err));
        return std::nullopt;
    }
    return result;
}

std::optional<std::string> weechat::xmpp::pgp::sign(struct t_gui_buffer *buffer, std::string_view source, std::string_view message)
{
    struct data_guard {
        gpgme_data_t h = nullptr;
        ~data_guard() { if (h) gpgme_data_release(h); }
    } in_g, out_g;
    struct key_guard {
        gpgme_key_t k = nullptr;
        ~key_guard() { if (k) gpgme_key_release(k); }
    } key_g;

    std::string signature;
    gpgme_error_t err;

    err = gpgme_data_new_from_mem(&in_g.h, message.data(), message.size(), false);
    if (err) goto sign_finish;

    err = gpgme_data_new(&out_g.h);
    if (err) goto sign_finish;

    {
        gpgme_keylist_mode_t kmode = gpgme_get_keylist_mode(this->gpgme);
        kmode |= GPGME_KEYLIST_MODE_LOCATE;
        kmode |= GPGME_KEYLIST_MODE_SIGS;
        err = gpgme_set_keylist_mode(this->gpgme, kmode);
    }
    if (err) goto sign_finish;

    err = gpgme_get_key(this->gpgme, source.data(), &key_g.k, false);
    if (err) {
        weechat_printf(nullptr, "(gpg) get key fail for %s", source.data());
        goto sign_finish;
    }
    err = gpgme_signers_add(this->gpgme, key_g.k);
    if (err) {
        weechat_printf(nullptr, "(gpg) add key fail for %s", source.data());
        goto sign_finish;
    }

    err = gpgme_op_sign(this->gpgme, in_g.h, out_g.h, GPGME_SIG_MODE_DETACH);
    if (err) {
        weechat_printf(nullptr, "(gpg) sign fail for %s", source.data());
        goto sign_finish;
    }
    if (gpgme_sign_result_t sgn_result = gpgme_op_sign_result(this->gpgme);
            !sgn_result->signatures)
        weechat_printf(nullptr, "(gpg) signature fail for %s", source);

    {
        gpgme_data_seek(out_g.h, 0, SEEK_SET);
        char data[512 + 1];
        int ret;
        while ((ret = gpgme_data_read(out_g.h, data, 512)) > 0)
            signature += std::string_view(data, ret);
    }

sign_finish:
    if (err) {
        weechat_printf(buffer, "[PGP]\t%s - %s",
                gpgme_strsource(err), gpgme_strerror(err));
        return std::nullopt;
    }
    if (signature.size() <= kPgpSignatureHeader.size() + kPgpSignatureFooter.size())
        return std::nullopt;
    return std::string(signature.data() + kPgpSignatureHeader.size(),
                       signature.size() - kPgpSignatureHeader.size() - kPgpSignatureFooter.size());
}
