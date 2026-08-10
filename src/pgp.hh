// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace weechat::xmpp
{
    extern const char *PGP_ADVICE;

    class pgp
    {
    public:
        struct gpgme_context *gpgme;
        const char *keyid;

    public:
        pgp();

        ~pgp();

        std::optional<std::string> decrypt(struct t_gui_buffer *buffer, std::string_view ciphertext);

        std::optional<std::string> encrypt(struct t_gui_buffer *buffer, std::string_view source, std::vector<std::string>&& target, std::string_view message);

        // XEP-0027: detached signature over presence <status/> character data
        // (may be empty). Returns signer fingerprint when GPGME reports one.
        std::optional<std::string> verify(struct t_gui_buffer *buffer,
                                          std::string_view signed_text,
                                          std::string_view signature);

        std::optional<std::string> sign(struct t_gui_buffer *buffer, std::string_view source, std::string_view message);
    };
}
