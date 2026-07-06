// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <optional>
#include <string>

#include "connection.hh"
#include "connection/strophe_stream_features.hh"

namespace {

constexpr std::size_t k_stream_features_sniff_max = 65536;

weechat::connection *g_stream_features_sniff_target = nullptr;
int (*g_stream_features_orig_read)(libstrophe_layout::conn_interface *, void *, size_t) =
    nullptr;
std::string g_stream_features_recv_buf;
std::optional<bool> g_stream_features_sniffed_csi;

int stream_features_sniff_read(libstrophe_layout::conn_interface *intf, void *buf, size_t len)
{
    if (!g_stream_features_orig_read)
        return -1;

    const int nbytes = g_stream_features_orig_read(intf, buf, len);
    if (nbytes <= 0 || !g_stream_features_sniff_target)
        return nbytes;

    if (g_stream_features_recv_buf.size() < k_stream_features_sniff_max)
    {
        const auto append_len = std::min<std::size_t>(
            static_cast<std::size_t>(nbytes),
            k_stream_features_sniff_max - g_stream_features_recv_buf.size());
        g_stream_features_recv_buf.append(static_cast<const char *>(buf), append_len);
        if (stream_features_xml_advertises_csi(g_stream_features_recv_buf))
            g_stream_features_sniffed_csi = true;
    }

    return nbytes;
}

}  // namespace

void stream_features_sniff_install(weechat::connection &connection) noexcept
{
    stream_features_sniff_restore(connection);

    g_stream_features_sniff_target = &connection;
    g_stream_features_recv_buf.clear();
    g_stream_features_sniffed_csi = std::nullopt;

    auto &layout = *reinterpret_cast<libstrophe_layout::conn_through_sm_state *>(
        static_cast<xmpp_conn_t *>(connection));
    g_stream_features_orig_read = layout.intf.read;
    layout.intf.read = stream_features_sniff_read;
}

void stream_features_sniff_restore(weechat::connection &connection) noexcept
{
    if (!g_stream_features_orig_read)
        return;

    auto &layout = *reinterpret_cast<libstrophe_layout::conn_through_sm_state *>(
        static_cast<xmpp_conn_t *>(connection));
    if (layout.intf.read == stream_features_sniff_read)
        layout.intf.read = g_stream_features_orig_read;

    g_stream_features_orig_read = nullptr;
    g_stream_features_sniff_target = nullptr;
    g_stream_features_recv_buf.clear();
}

[[nodiscard]] std::optional<bool>
stream_features_sniff_finish(weechat::connection &connection) noexcept
{
    const auto sniffed = g_stream_features_sniffed_csi;
    stream_features_sniff_restore(connection);
    return sniffed;
}