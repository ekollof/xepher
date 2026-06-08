// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "iq_caps.hh"

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <span>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <fmt/core.h>

namespace xmpp {

namespace {

struct FormData {
    std::string form_type;
    std::vector<std::pair<std::string, std::vector<std::string>>> fields;
};

[[nodiscard]] std::vector<std::string> sorted_field_values(const StanzaView field)
{
    std::vector<std::string> vals;
    for (const StanzaView vnode : field)
    {
        if (vnode.name() != "value")
            continue;
        vals.push_back(vnode.text());
    }
    std::ranges::sort(vals);
    return vals;
}

[[nodiscard]] std::optional<FormData> parse_caps_form(const StanzaView x_elem)
{
    const auto xmlns_x = x_elem.attr("xmlns");
    if (!xmlns_x || *xmlns_x != "jabber:x:data")
        return std::nullopt;

    FormData fd;
    bool has_form_type = false;
    for (const StanzaView field : x_elem)
    {
        if (field.name() != "field")
            continue;
        const auto fvar = field.attr("var");
        if (fvar && *fvar == "FORM_TYPE")
        {
            const StanzaView vnode = field.child("value");
            if (vnode.valid())
                fd.form_type = vnode.text();
            has_form_type = true;
            break;
        }
    }
    if (!has_form_type)
        return std::nullopt;

    for (const StanzaView field : x_elem)
    {
        if (field.name() != "field")
            continue;
        const auto fvar = field.attr("var");
        if (!fvar || *fvar == "FORM_TYPE")
            continue;
        fd.fields.emplace_back(std::string(*fvar), sorted_field_values(field));
    }

    std::ranges::sort(fd.fields,
                      [](const auto &a, const auto &b) { return a.first < b.first; });
    return fd;
}

[[nodiscard]] std::string base64_no_nl(std::span<const std::uint8_t> data)
{
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *mem = BIO_new(BIO_s_mem());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    b64 = BIO_push(b64, mem);
    BIO_write(b64, data.data(), static_cast<int>(data.size()));
    BIO_flush(b64);
    BUF_MEM *buf = nullptr;
    BIO_get_mem_ptr(b64, &buf);
    std::string out;
    if (buf && buf->data && buf->length > 0)
        out.assign(buf->data, static_cast<std::size_t>(buf->length));
    BIO_free_all(b64);
    return out;
}

}  // namespace

std::string build_caps_verification_string(
    const StanzaView query,
    const std::vector<std::string> &features)
{
    if (!query.valid())
        return {};

    std::string s;

    std::vector<std::string> identities;
    for (const StanzaView id_elem : query)
    {
        if (id_elem.name() != "identity")
            continue;
        identities.push_back(
            id_elem.attr_string("category") + "/" +
            id_elem.attr_string("type") + "/" +
            id_elem.attr_string("lang") + "/" +
            id_elem.attr_string("name"));
    }
    std::ranges::sort(identities);
    std::ranges::for_each(identities, [&](const std::string &ident) { s += ident + "<"; });

    std::vector<std::string> sorted_features = features;
    std::ranges::sort(sorted_features);
    std::ranges::for_each(sorted_features, [&](const std::string &feat) { s += feat + "<"; });

    std::vector<FormData> forms;
    for (const StanzaView x_elem : query)
    {
        if (x_elem.name() != "x")
            continue;
        if (auto fd = parse_caps_form(x_elem))
            forms.push_back(std::move(*fd));
    }
    std::ranges::sort(forms, [](const FormData &a, const FormData &b) {
        return a.form_type < b.form_type;
    });
    for (const auto &form : forms)
    {
        s += form.form_type + "<";
        for (const auto &[fvar, fvals] : form.fields)
        {
            s += fvar + "<";
            for (const auto &val : fvals)
                s += val + "<";
        }
    }

    return s;
}

std::string caps_sha1_base64(const std::string_view verification_string)
{
    unsigned char digest[20];
    unsigned int digest_len = sizeof(digest);
    const auto *data = reinterpret_cast<const unsigned char *>(verification_string.data());
    EVP_Digest(data, verification_string.size(), digest, &digest_len, EVP_sha1(), nullptr);
    return base64_no_nl(std::span<const std::uint8_t>(digest, digest_len));
}

bool caps_requested_node_ok(
    const std::string_view requested_node,
    const std::string_view computed_hash)
{
    if (requested_node.empty())
        return true;
    if (requested_node == "http://weechat.org")
        return true;
    if (!computed_hash.empty()
        && requested_node == fmt::format("http://weechat.org#{}", computed_hash))
        return true;
    return false;
}

}  // namespace xmpp