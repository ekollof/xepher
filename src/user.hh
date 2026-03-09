// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <optional>
#include <vector>

namespace weechat
{
    class account;
    class channel;

    class user
    {
    private:
        struct profile
        {
            std::string avatar_hash;
            std::vector<uint8_t> avatar_data;  // Cached avatar image data
            std::string avatar_rendered;        // Cached Unicode rendering
            std::optional<std::string> status_text;
            std::optional<std::string> status;
            std::optional<std::string> idle;
            std::string display_name;
            std::optional<std::string> email;
            std::optional<std::string> role;
            std::optional<std::string> affiliation;
            std::optional<std::string> pgp_id;
            int omemo = 0;
            // XEP-0054 vcard-temp fields
            std::optional<std::string> fn;          // Full name
            std::optional<std::string> nickname;    // Nickname
            std::optional<std::string> url;         // URL / homepage
            std::optional<std::string> description; // Free-text description / bio
            std::optional<std::string> org;         // Organisation name
            std::optional<std::string> title;       // Job title
            std::optional<std::string> tel;         // Phone number
            std::optional<std::string> bday;        // Birthday (ISO 8601)
            std::optional<std::string> note;        // Note
            std::optional<std::string> jabberid;    // JID declared in vCard
            bool vcard_fetched = false;             // true once a vcard response has been processed
        };

    private:
        std::string name;

        bool updated = false;

    public:
        std::string id;
        bool is_away = false;
        struct profile profile;

    public:
        user(weechat::account *account, weechat::channel *channel, const char *id, const char *display_name);

        static std::string get_colour(const char *name);
        static std::string get_colour_for_nicklist(const char *name);
        std::string get_colour();
        std::string get_colour_for_nicklist();
        static std::string as_prefix_raw(const char *name);
        static std::string as_prefix(const char *name);
        std::string as_prefix_raw();
        std::string as_prefix();

        static std::string as_prefix_raw(weechat::account *account, const char *id) {
            auto found = search(account, id);
            return found ? found->as_prefix_raw() : "";
        }
        static std::string as_prefix(weechat::account *account, const char *id) {
            auto found = search(account, id);
            return found ? found->as_prefix() : "";
        }

        static weechat::user *bot_search(weechat::account *account, const char *pgp_id);
        static weechat::user *search(weechat::account *account, const char *id);

        void nicklist_add(weechat::account *account, weechat::channel *channel);
        void nicklist_remove(weechat::account *account, weechat::channel *channel);
    };
}
