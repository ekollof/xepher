// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <optional>
#include <string>
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

        std::optional<std::string> decrypt(struct t_gui_buffer *buffer, const char *ciphertext);

        std::optional<std::string> encrypt(struct t_gui_buffer *buffer, const char *source, std::vector<std::string>&& target, const char *message);

        std::optional<std::string> verify(struct t_gui_buffer *buffer, const char *certificate);

        std::optional<std::string> sign(struct t_gui_buffer *buffer, const char *source, const char *message);
    };
}
