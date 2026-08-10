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
#include <stdexcept>
#include <span>
#include <vector>
#include <fmt/core.h>
#include <fmt/ranges.h>
#include <gpgme.h>
#include <weechat/weechat-plugin.h>

#include "plugin.hh"
#include "pgp.hh"
#include "weechat/ui_port.hh"

namespace {

void pgp_print(struct t_gui_buffer *buffer, std::string_view msg)
{
    weechat::UiPort::for_buffer(buffer)->printf(msg);
}

}  // namespace

std::string format_key(weechat::xmpp::pgp &pgp, std::string_view keyid)
{
    gpgme_key_t key = nullptr;
    // gpgme_get_key requires a null-terminated C string.
    const std::string keyid_nt(keyid);
    gpgme_error_t err = gpgme_get_key(pgp.gpgme, keyid_nt.c_str(), &key, false);
    if (err || !key) {
        return fmt::format("{} (none)", keyid);
    }
    std::vector<std::string> subkey_ids;
    for (auto subkey = key->subkeys; subkey; subkey = subkey->next)
    {
        std::string sid(subkey->keyid);
        if (sid.size() > 8)
            sid = sid.substr(sid.size() - 8);
        subkey_ids.push_back(std::move(sid));
    }
    std::vector<std::string> userids;
    for (auto uid = key->uids; uid; uid = uid->next)
        userids.push_back(fmt::format("{} ({})", uid->name, uid->email));

    return fmt::format("{}{{{}}}[{}]",
                       keyid,
                       fmt::join(subkey_ids, ", "),
                       fmt::join(userids, ", "));
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

    gpgme_check_version(nullptr);

    err = gpgme_new(&this->gpgme);
    if (err) {
        pgp_print(nullptr, fmt::format("gpg (error): {} - {}",
                gpgme_strsource(err), gpgme_strerror(err)));
        throw std::runtime_error("gpgme_new failed");
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

    std::span<const char> message_span = message;
    err = gpgme_data_new_from_mem(&in_g.h, message_span.data(), message_span.size(), false);
    if (err) goto encrypt_finish;

    err = gpgme_data_new(&out_g.h);
    if (err) goto encrypt_finish;

    for (const std::string& target : targets)
    {
        err = gpgme_get_key(this->gpgme, target.c_str(), &keys[0], false);
        if (err) goto encrypt_finish;
    }
    {
        const std::string source_nt(source);
        err = gpgme_get_key(this->gpgme, source_nt.c_str(), &keys[1], false);
        if (err) goto encrypt_finish;
    }

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
        pgp_print(buffer, fmt::format("[PGP]\t{} - {}",
                gpgme_strsource(err), gpgme_strerror(err)));
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
    std::span<const char> buf_span = buf;
    err = gpgme_data_new_from_mem(&in_g.h, buf_span.data(), buf_span.size(), false);
    if (err) goto decrypt_finish;

    err = gpgme_data_new(&out_g.h);
    if (err) goto decrypt_finish;

    err = gpgme_op_decrypt(this->gpgme, in_g.h, out_g.h);
    if (gpgme_decrypt_result_t dec_result = gpgme_op_decrypt_result(this->gpgme); dec_result)
    {
        std::vector<std::string> recip_keys;
        for (auto recip = dec_result->recipients; recip; recip = recip->next)
            recip_keys.push_back(format_key(*this, recip->keyid));
        keyids = fmt::format("{}", fmt::join(recip_keys, ", "));
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
        pgp_print(buffer, fmt::format("[PGP]\t{} - {} ({})",
                gpgme_strsource(err), gpgme_strerror(err), keyids));
        return std::nullopt;
    }
    return decrypted;
}

std::optional<std::string> weechat::xmpp::pgp::verify(struct t_gui_buffer *buffer,
                                                      std::string_view signed_text,
                                                      std::string_view signature)
{
    // XEP-0027 presence signatures are detached OpenPGP signatures of the
    // <status/> character data (possibly empty), not clearsigned messages.
    // gpgme_op_verify(sig, nullptr, plain) is clearsign/inline semantics and
    // often leaves signatures==nullptr — the old code then crashed on
    // vrf_result->signatures->fpr. Never call gpgme_op_receive_keys here:
    // keyserver I/O from the presence handler hangs/crashes WeeChat.
    struct data_guard {
        gpgme_data_t h = nullptr;
        ~data_guard() { if (h) gpgme_data_release(h); }
    } sig_g, text_g;

    if (signature.empty())
        return std::nullopt;

    std::string armored =
        fmt::format(PGP_SIGNATURE_HEADER "{}" PGP_SIGNATURE_FOOTER, signature);

    // copy=1: buffers must outlive only until op_verify returns; string_view
    // data need not be null-terminated.
    gpgme_error_t err =
        gpgme_data_new_from_mem(&sig_g.h, armored.data(), armored.size(), 1);
    if (err)
        goto verify_finish;

    {
        const char *text_ptr = signed_text.empty() ? "" : signed_text.data();
        err = gpgme_data_new_from_mem(&text_g.h, text_ptr, signed_text.size(), 1);
        if (err)
            goto verify_finish;
    }

    // Detached: signature blob + original signed text; no plaintext output.
    err = gpgme_op_verify(this->gpgme, sig_g.h, text_g.h, nullptr);
    if (err)
        goto verify_finish;

    {
        gpgme_verify_result_t vrf = gpgme_op_verify_result(this->gpgme);
        if (!vrf || !vrf->signatures)
            return std::nullopt;

        gpgme_signature_t sig = vrf->signatures;
        // fpr is often null when the public key is missing; never construct
        // std::string from a null const char*.
        if (!sig->fpr || !sig->fpr[0])
            return std::nullopt;

        return std::string(sig->fpr);
    }

verify_finish:
    if (err)
    {
        pgp_print(buffer, fmt::format("[PGP]\t{} - {}",
                                      gpgme_strsource(err),
                                      gpgme_strerror(err)));
    }
    return std::nullopt;
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

    std::span<const char> message_span = message;
    err = gpgme_data_new_from_mem(&in_g.h, message_span.data(), message_span.size(), false);
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

    {
        const std::string source_nt(source);
        err = gpgme_get_key(this->gpgme, source_nt.c_str(), &key_g.k, false);
    }
    if (err) {
        pgp_print(nullptr, fmt::format("(gpg) get key fail for {}", source));
        goto sign_finish;
    }
    err = gpgme_signers_add(this->gpgme, key_g.k);
    if (err) {
        pgp_print(nullptr, fmt::format("(gpg) add key fail for {}", source));
        goto sign_finish;
    }

    err = gpgme_op_sign(this->gpgme, in_g.h, out_g.h, GPGME_SIG_MODE_DETACH);
    if (err) {
        pgp_print(nullptr, fmt::format("(gpg) sign fail for {}", source));
        goto sign_finish;
    }
    if (gpgme_sign_result_t sgn_result = gpgme_op_sign_result(this->gpgme);
            !sgn_result->signatures)
        pgp_print(nullptr, fmt::format("(gpg) signature fail for {}", source));

    {
        gpgme_data_seek(out_g.h, 0, SEEK_SET);
        char data[512 + 1];
        int ret;
        while ((ret = gpgme_data_read(out_g.h, data, 512)) > 0)
            signature += std::string_view(data, ret);
    }

sign_finish:
    if (err) {
        pgp_print(buffer, fmt::format("[PGP]\t{} - {}",
                gpgme_strsource(err), gpgme_strerror(err)));
        return std::nullopt;
    }
    if (signature.size() <= kPgpSignatureHeader.size() + kPgpSignatureFooter.size())
        return std::nullopt;
    return std::string(signature.data() + kPgpSignatureHeader.size(),
                       signature.size() - kPgpSignatureHeader.size() - kPgpSignatureFooter.size());
}
