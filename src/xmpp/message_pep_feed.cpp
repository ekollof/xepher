// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "message_pep_feed.hh"

#include <algorithm>
#include <fmt/core.h>
#include <fmt/ranges.h>
#include <ranges>
#include <weechat/weechat-plugin.h>

#include "account.hh"
#include "channel.hh"
#include "plugin.hh"
#include "xmpp/iq_pubsub_feed.hh"

namespace xmpp {

namespace {

struct FeedColors {
    const char *pfx;
    const char *bold;
    const char *rst;
    const char *dim;
    const char *grn;
};

[[nodiscard]] FeedColors feed_colors()
{
    return {
        .pfx = weechat_prefix("join"),
        .bold = weechat_color("bold"),
        .rst = weechat_color("reset"),
        .dim = weechat_color("darkgray"),
        .grn = weechat_color("green"),
    };
}

void print_feed_header_line(struct t_gui_buffer *buffer,
                            const FeedColors &c,
                            std::string_view alias_pfx,
                            std::string_view title,
                            std::string_view author,
                            std::string_view pubdate)
{
    const char *ag = alias_pfx.empty() ? "" : c.grn;
    const char *ar = alias_pfx.empty() ? "" : c.rst;

    if (!title.empty())
    {
        if (!author.empty() && !pubdate.empty())
        {
            weechat_printf_date_tags(buffer, 0, "xmpp_feed,notify_message",
                                     "%s%s%s%s %s%s%s  [%s%s%s] — %s",
                                     c.pfx, ag, alias_pfx.data(), ar,
                                     c.bold, title.data(), c.rst,
                                     c.dim, author.data(), c.rst,
                                     pubdate.data());
        }
        else if (!author.empty())
        {
            weechat_printf_date_tags(buffer, 0, "xmpp_feed,notify_message",
                                     "%s%s%s%s %s%s%s  [%s%s%s]",
                                     c.pfx, ag, alias_pfx.data(), ar,
                                     c.bold, title.data(), c.rst,
                                     c.dim, author.data(), c.rst);
        }
        else if (!pubdate.empty())
        {
            weechat_printf_date_tags(buffer, 0, "xmpp_feed,notify_message",
                                     "%s%s%s%s %s%s%s — %s",
                                     c.pfx, ag, alias_pfx.data(), ar,
                                     c.bold, title.data(), c.rst,
                                     pubdate.data());
        }
        else
        {
            weechat_printf_date_tags(buffer, 0, "xmpp_feed,notify_message",
                                     "%s%s%s%s %s%s%s",
                                     c.pfx, ag, alias_pfx.data(), ar,
                                     c.bold, title.data(), c.rst);
        }
        return;
    }

    if (!author.empty() && !pubdate.empty())
    {
        weechat_printf_date_tags(buffer, 0, "xmpp_feed,notify_message",
                                 "%s%s%s%s  [%s%s%s] — %s",
                                 c.pfx, ag, alias_pfx.data(), ar,
                                 c.dim, author.data(), c.rst,
                                 pubdate.data());
    }
    else if (!author.empty())
    {
        weechat_printf_date_tags(buffer, 0, "xmpp_feed,notify_message",
                                 "%s%s%s%s  [%s%s%s]",
                                 c.pfx, ag, alias_pfx.data(), ar,
                                 c.dim, author.data(), c.rst);
    }
    else if (!pubdate.empty())
    {
        weechat_printf_date_tags(buffer, 0, "xmpp_feed,notify_message",
                                 "%s%s%s%s  — %s",
                                 c.pfx, ag, alias_pfx.data(), ar,
                                 pubdate.data());
    }
}

void print_feed_body_lines(struct t_gui_buffer *buffer, const FeedColors &c, std::string_view body)
{
    if (body.empty())
        return;

    weechat_printf_date_tags(buffer, 0, "xmpp_feed,notify_none",
                             "  %s\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500%s",
                             c.dim, c.rst);
    for (auto line : body | std::views::split('\n'))
    {
        const std::string_view chunk(line.begin(), line.end());
        weechat_printf_date_tags(buffer, 0, "xmpp_feed,notify_none",
                                 "  %.*s", static_cast<int>(chunk.size()), chunk.data());
    }
    weechat_printf_date_tags(buffer, 0, "xmpp_feed,notify_none", "");
}

[[nodiscard]] std::string format_attachment_size(std::uint64_t size)
{
    if (size >= 1024 * 1024)
        return fmt::format("{:.1f} MB", size / (1024.0 * 1024.0));
    if (size >= 1024)
        return fmt::format("{:.1f} KB", size / 1024.0);
    return fmt::format("{} B", size);
}

}  // namespace

std::string feed_alias_prefix(int item_alias)
{
    if (item_alias > 0)
        return fmt::format("#{}", item_alias);
    return {};
}

std::string feed_node_display_label(std::string_view service_jid, std::string_view node)
{
    if (is_microblog_comments_node(node))
        return "comments";

    if (node == "urn:xmpp:microblog:0")
    {
        std::string_view local = service_jid;
        if (const auto at = service_jid.find('@'); at != std::string_view::npos)
            local = service_jid.substr(0, at);
        return std::string(local);
    }

    if (!node.contains(':'))
        return std::string(node);

    const auto colon = node.rfind(':');
    return std::string(node.substr(colon + 1));
}

std::string feed_parent_display_label(std::string_view parent_service,
                                      std::string_view parent_node)
{
    if (parent_node == "urn:xmpp:microblog:0")
    {
        std::string_view local = parent_service;
        if (const auto at = parent_service.find('@'); at != std::string_view::npos)
            local = parent_service.substr(0, at);
        return fmt::format("{} (blog)", local);
    }

    if (!parent_node.contains(':'))
        return std::string(parent_node);

    std::string_view local = parent_service;
    if (const auto at = parent_service.find('@'); at != std::string_view::npos)
        local = parent_service.substr(0, at);
    const auto colon = parent_node.rfind(':');
    return fmt::format("{} ({})", local, parent_node.substr(colon + 1));
}

std::string feed_buffer_short_name(std::string_view feed_key)
{
    const auto slash = feed_key.find('/');
    const std::string_view service =
        (slash != std::string_view::npos) ? feed_key.substr(0, slash) : feed_key;
    const std::string_view node =
        (slash != std::string_view::npos) ? feed_key.substr(slash + 1) : feed_key;

    if (is_microblog_comments_node(node))
    {
        std::string_view local = service;
        if (const auto at = service.find('@'); at != std::string_view::npos)
            local = service.substr(0, at);
        return fmt::format("={} (comments)", local);
    }

    if (node == "urn:xmpp:microblog:0")
    {
        std::string_view local = service;
        if (const auto at = service.find('@'); at != std::string_view::npos)
            local = service.substr(0, at);
        return fmt::format("={} (blog)", local);
    }

    if (!node.contains(':'))
        return fmt::format("={}", node);

    std::string_view local = service;
    if (const auto at = service.find('@'); at != std::string_view::npos)
        local = service.substr(0, at);
    return fmt::format("={} ({})", local, feed_node_display_label(service, node));
}

std::string feed_comments_buffer_short_name(std::string_view parent_label, int item_alias)
{
    if (parent_label.empty())
        return item_alias > 0 ? fmt::format("=(#{} comments)", item_alias) : "=comments";

    if (item_alias > 0)
        return fmt::format("={} (#{} comments)", parent_label, item_alias);

    return fmt::format("={} (comments)", parent_label);
}

std::string feed_item_xmpp_link(
    std::string_view feed_service,
    std::string_view node,
    std::string_view item_id)
{
    if (item_id.empty())
        return {};
    return fmt::format("xmpp:{}?;node={};item={}", feed_service, node, item_id);
}

std::string feed_reply_label(
    std::string_view reply_to,
    const std::function<int(std::string_view)> &alias_lookup)
{
    if (reply_to.empty())
        return {};

    if (const auto item_eq = reply_to.rfind("item="); item_eq != std::string_view::npos)
    {
        const std::string_view reply_uuid = reply_to.substr(item_eq + 5);
        if (const int ralias = alias_lookup(reply_uuid); ralias > 0)
            return fmt::format("#{}", ralias);
        return std::string(reply_uuid);
    }
    return std::string(reply_to);
}

void render_atom_entry_to_feed(
    weechat::channel &feed_ch,
    weechat::account &account,
    std::string_view feed_key,
    std::string_view feed_service,
    std::string_view node,
    std::string_view item_id,
    int item_alias,
    const atom_entry &ae)
{
    if (!feed_ch.buffer || ae.empty())
        return;

    const FeedColors c = feed_colors();
    const std::string alias_pfx = feed_alias_prefix(item_alias);

    std::string link = ae.link;
    if (link.empty() && !item_id.empty())
        link = feed_item_xmpp_link(feed_service, node, item_id);

    print_feed_header_line(feed_ch.buffer, c, alias_pfx,
                           ae.title, ae.author, ae.pubdate);

    if (!ae.reply_to.empty())
    {
        const std::string reply_label = feed_reply_label(
            ae.reply_to,
            [&](std::string_view uuid) {
                return account.feed_alias_lookup(feed_key, uuid);
            });
        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                 "  %sIn reply to:%s %s", c.dim, c.rst, reply_label.c_str());
    }

    if (!ae.via_link.empty())
    {
        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                 "  %sRepeated from:%s %s", c.dim, c.rst, ae.via_link.c_str());
    }

    if (!link.empty())
        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none", "  %s", link.c_str());

    if (!ae.replies_link.empty())
    {
        const std::string &comments_ref = alias_pfx.empty() ? std::string(item_id) : alias_pfx;
        if (!comments_ref.empty())
        {
            if (ae.comments_count >= 0)
            {
                weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                         "  %sComments (%d):%s /feed comments %s",
                                         c.dim, ae.comments_count, c.rst, comments_ref.c_str());
            }
            else
            {
                weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                         "  %sComments:%s /feed comments %s",
                                         c.dim, c.rst, comments_ref.c_str());
            }
        }
    }

    if (!ae.categories.empty())
    {
        const std::string tags = fmt::format("{}", fmt::join(ae.categories, ", "));
        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                 "  %sTags:%s %s", c.dim, c.rst, tags.c_str());
    }

    std::ranges::for_each(ae.enclosures, [&](const std::string &enclosure) {
        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                 "  %sAttachment:%s %s", c.dim, c.rst, enclosure.c_str());
    });

    for (const auto &att : ae.attachments)
    {
        const bool is_image = !att.media_type.empty() && att.media_type.starts_with("image/");
        const bool is_video = !att.media_type.empty() && att.media_type.starts_with("video/");
        const std::string kind_str = (att.disposition == "attachment") ? "File"
            : is_image ? "Image"
            : is_video ? "Video"
            : "Media";

        std::string meta;
        if (!att.media_type.empty())
            meta += att.media_type;
        if (att.size > 0)
        {
            const std::string size_str = format_attachment_size(att.size);
            if (!meta.empty())
                meta += ", ";
            meta += size_str;
        }

        const std::string meta_suffix = meta.empty() ? "" : fmt::format(" ({})", meta);
        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                 "  %s[%s: %s%s] %s%s",
                                 c.dim, kind_str.c_str(), att.filename.c_str(),
                                 meta_suffix.c_str(), att.url.c_str(), c.rst);
    }

    if (!ae.geoloc.empty())
    {
        weechat_printf_date_tags(feed_ch.buffer, 0, "xmpp_feed,notify_none",
                                 "  %sLocation:%s %s", c.dim, c.rst, ae.geoloc.c_str());
    }

    print_feed_body_lines(feed_ch.buffer, c, ae.body());
}

}  // namespace xmpp