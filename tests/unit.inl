// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// Unit tests for pure and near-pure plugin functions.
//
// Calls the real symbols from xmpp.cov.so (coverage-instrumented plugin)
// so that gcovr reports non-zero coverage for the exercised source files.
//
// Functions under test:
//   • char_cmp, unescape, parse_uint32, parse_int64,
//     format_local_timestamp, format_utc_timestamp  → src/util.cpp
//   • message__htmldecode                  → src/message.cpp
//   • get_name, get_attribute, get_text    → src/xmpp/node.cpp
//   • jid::jid, jid::is_bare              → src/xmpp/node.cpp
//   • weechat::consistent_color            → src/color.cpp
//   • weechat::angle_to_weechat_color      → src/color.cpp

#include <doctest/doctest.h>

// ── plugin headers (real symbols declared here) ───────────────────────────────
#include "util.hh"
#include "message.hh"
#include "color.hh"
#include "strophe.hh"
#include "xmpp/node.hh"
#include "xmpp/stanza_view.hh"
#include "xmpp/atom.hh"
#include "xmpp/xhtml.hh"
#include "xmpp/iq_handlers.hh"
#include "xmpp/iq_error.hh"
#include "xmpp/iq_ping.hh"
#include "xmpp/iq_pubsub_feed.hh"
#include "xmpp/iq_omemo_pubsub.hh"
#include "xmpp/iq_upload.hh"
#include "xmpp/iq_mam.hh"
#include "xmpp/iq_disco.hh"
#include "xmpp/iq_caps.hh"
#include "xmpp/iq_vcard.hh"
#include "xmpp/iq_bookmarks.hh"
#include "weechat/render_event.hh"
#include "xmpp/chat_state.hh"
#include "xmpp/message_forward.hh"
#include "xmpp/message_body.hh"
#include "xmpp/message_media.hh"
#include "xmpp/message_sticker_emoji.hh"
#include "xmpp/message_bob.hh"
#include "xmpp/message_omemo.hh"
#include "xmpp/message_invite.hh"
#include "xmpp/message_ephemeral.hh"
#include "xmpp/message_spoiler.hh"
#include "xmpp/message_fallback.hh"
#include "xmpp/message_line_tag.hh"
#include "xmpp/message_correct.hh"
#include "xmpp/message_retract.hh"
#include "xmpp/message_reactions.hh"
#include "xmpp/message_reply.hh"
#include "xmpp/message_pep.hh"
#include "xmpp/message_pep_feed.hh"
#include "xmpp/message_ack.hh"
#include "weechat/line_store.hh"
#include "weechat/runtime_port.hh"
#include "weechat_stub.hh"

// ── stdlib ────────────────────────────────────────────────────────────────────
#include <cstring>
#include <ctime>
#include <string>
#include <fmt/chrono.h>

// ── libstrophe ────────────────────────────────────────────────────────────────
#include <strophe.h>

// ─────────────────────────────────────────────────────────────────────────────
// Shared libstrophe fixture
// ─────────────────────────────────────────────────────────────────────────────
namespace {

struct unit_strophe_env {
    xmpp_ctx_t *ctx{nullptr};
    unit_strophe_env()
    {
        xmpp_initialize();
        ctx = xmpp_ctx_new(nullptr, nullptr);
    }
    ~unit_strophe_env()
    {
        if (ctx)
            xmpp_ctx_free(ctx);
        xmpp_shutdown();
    }
};

}  // namespace

// =============================================================================
// TEST CASES – Group A: pure stdlib / C++23 (src/util.cpp, src/message.cpp)
// =============================================================================

TEST_CASE("char_cmp semantics")
{
    // Returns non-zero when the two pointed-to chars are equal
    char a = 'x', b = 'x', c = 'y';
    CHECK(char_cmp(&a, &b) != 0);  // equal → truthy
    CHECK(char_cmp(&a, &c) == 0);  // unequal → falsy
    // Note: deliberately opposite of strcmp/memcmp convention
}

TEST_CASE("unescape: numeric HTML entity decoding")
{
    SUBCASE("no entities returns input unchanged")
    {
        const std::string s = "hello world";
        CHECK(unescape(s) == s);
    }

    SUBCASE("single entity in the middle")
    {
        // &#65; is 'A'
        CHECK(unescape("&#65;") == "A");
    }

    SUBCASE("entity at start")
    {
        // &#72; = 'H', &#101; = 'e'
        CHECK(unescape("&#72;ello") == "Hello");
    }

    SUBCASE("multiple entities")
    {
        // &#65;&#66;&#67; = "ABC"
        CHECK(unescape("&#65;&#66;&#67;") == "ABC");
    }

    SUBCASE("entity at end")
    {
        // "X&#33;" → "X!"  (33 = '!')
        CHECK(unescape("X&#33;") == "X!");
    }

    SUBCASE("mixed text and entities")
    {
        // "a&#43;b" → "a+b"  (43 = '+')
        CHECK(unescape("a&#43;b") == "a+b");
    }

    SUBCASE("non-entity ampersand left unchanged")
    {
        const std::string s = "AT&T";
        CHECK(unescape(s) == s);
    }

    SUBCASE("empty string returns empty")
    {
        CHECK(unescape("") == "");
    }
}

TEST_CASE("parse_uint32")
{
    SUBCASE("valid decimal")
    {
        CHECK(parse_uint32("42") == std::expected<std::uint32_t, std::string>(42));
    }

    SUBCASE("empty input is invalid")
    {
        CHECK_FALSE(parse_uint32(""));
    }

    SUBCASE("trailing junk is invalid")
    {
        CHECK_FALSE(parse_uint32("42x"));
    }

    SUBCASE("negative is invalid")
    {
        CHECK_FALSE(parse_uint32("-1"));
    }
}

TEST_CASE("parse_sm_location")
{
    SUBCASE("hostname with port")
    {
        const auto ep = parse_sm_location("xmpp.example.com:5222");
        REQUIRE(ep);
        CHECK(ep->host == "xmpp.example.com");
        CHECK(ep->port == 5222);
    }

    SUBCASE("bracketed IPv6 with port")
    {
        const auto ep = parse_sm_location("[2001:41D0:1:A49b::1]:9222");
        REQUIRE(ep);
        CHECK(ep->host == "2001:41D0:1:A49b::1");
        CHECK(ep->port == 9222);
    }

    SUBCASE("empty is invalid")
    {
        CHECK_FALSE(parse_sm_location(""));
    }

    SUBCASE("missing port is invalid")
    {
        CHECK_FALSE(parse_sm_location("xmpp.example.com"));
    }
}

TEST_CASE("parse_int64")
{
    SUBCASE("valid positive")
    {
        CHECK(parse_int64("1704067200") == std::expected<std::int64_t, std::string>(1704067200));
    }

    SUBCASE("valid negative")
    {
        CHECK(parse_int64("-5") == std::expected<std::int64_t, std::string>(-5));
    }

    SUBCASE("empty input is invalid")
    {
        CHECK_FALSE(parse_int64(""));
    }

    SUBCASE("non-numeric is invalid")
    {
        CHECK_FALSE(parse_int64("abc"));
    }
}

// doctest line-number collision guard (omemo_xep.inl)
TEST_CASE("format_utc_timestamp")
{
    CHECK(format_utc_timestamp(0) == "1970-01-01T00:00:00Z");
    CHECK(format_utc_timestamp(946684800) == "2000-01-01T00:00:00Z");
}

TEST_CASE("format_local_timestamp")
{
    const std::time_t t = 946684800;
    std::tm lt {};
    REQUIRE(localtime_r(&t, &lt));
    const std::string expected = fmt::format("{:%Y-%m-%d %H:%M}", lt);
    CHECK(format_local_timestamp(t) == expected);
}

TEST_CASE("stanza_element_text reads element body")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *el = xmpp_stanza_new_from_string(
        env.ctx, "<value xmlns='jabber:x:data'>hello world</value>");
    REQUIRE(el != nullptr);
    CHECK(stanza_element_text(el) == "hello world");
    CHECK(stanza_element_text(nullptr).empty());
    xmpp_stanza_release(el);
}

TEST_CASE("StanzaView is_text detects text nodes")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(
        env.ctx,
        "<body xmlns='http://www.w3.org/1999/xhtml'>"
        "<p>hello</p></body>");
    REQUIRE(root != nullptr);

    const xmpp::StanzaView body(root);
    CHECK_FALSE(body.is_text());

    bool saw_text = false;
    for (const xmpp::StanzaView child : body.child("p"))
        if (child.is_text())
            saw_text = true;
    CHECK(saw_text);

    xmpp_stanza_release(root);
}

TEST_CASE("html_strip_to_plain strips tags and decodes entities")
{
    CHECK(html_strip_to_plain("<p>Hello &amp; world</p>") == "Hello & world");
    CHECK(html_strip_to_plain("") == "");
}

TEST_CASE("parse_atom_entry reads Atom entry fields")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *entry = xmpp_stanza_new_from_string(
        env.ctx,
        "<entry xmlns='http://www.w3.org/2005/Atom'>"
        "<title>Post title</title>"
        "<content type='html'>&lt;p&gt;Body text&lt;/p&gt;</content>"
        "<author><name>Alice</name><uri>xmpp:alice@example.org</uri></author>"
        "<id>tag:example,2024:1</id>"
        "<link rel='alternate' href='https://example.org/post/1'/>"
        "</entry>");
    REQUIRE(entry != nullptr);

    const atom_entry ae = parse_atom_entry(xmpp::StanzaView(entry));
    CHECK(ae.title == "Post title");
    CHECK(ae.content == "Body text");
    CHECK(ae.content_type == "html");
    CHECK(ae.author == "Alice");
    CHECK(ae.author_uri == "xmpp:alice@example.org");
    CHECK(ae.item_id == "tag:example,2024:1");
    CHECK(ae.link == "https://example.org/post/1");

    xmpp_stanza_release(entry);
}

TEST_CASE("xhtml_to_weechat accepts StanzaView input")
{
    CHECK(xhtml_to_weechat(xmpp::StanzaView(nullptr)).empty());

    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *body = xmpp_stanza_new_from_string(
        env.ctx,
        "<body xmlns='http://www.w3.org/1999/xhtml'/>");
    REQUIRE(body != nullptr);
    CHECK(xhtml_to_weechat(xmpp::StanzaView(body)).empty());
    xmpp_stanza_release(body);
}

TEST_CASE("apply_xep394_markup accepts StanzaView input")
{
    CHECK(apply_xep394_markup(xmpp::StanzaView(nullptr), "Hello world").empty());

    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *msg = xmpp_stanza_new_from_string(
        env.ctx,
        "<message xmlns='jabber:client'><body>Hello world</body></message>");
    REQUIRE(msg != nullptr);
    CHECK(apply_xep394_markup(xmpp::StanzaView(msg), "Hello world").empty());
    xmpp_stanza_release(msg);
}

TEST_CASE("StanzaView child_iterator terminates on last child")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *msg = xmpp_stanza_new_from_string(env.ctx,
        "<message xmlns='jabber:client' type='chat'>"
        "<body>a</body>"
        "<reference xmlns='urn:xmpp:reference:0' type='data'/>"
        "</message>");
    REQUIRE(msg != nullptr);

    int count = 0;
    for ([[maybe_unused]] xmpp::StanzaView child : xmpp::StanzaView(msg))
        ++count;
    CHECK(count == 2);

    xmpp_stanza_release(msg);
}

TEST_CASE("StanzaView reads inbound attributes and children")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(
        env.ctx,
        "<iq type='get' id='q1' from='bob@example.org'><query xmlns='jabber:iq:version'/></iq>");
    REQUIRE(root != nullptr);

    const xmpp::StanzaView view(root);
    CHECK(view.name() == "iq");
    REQUIRE(view.type().has_value());
    CHECK(*view.type() == "get");
    REQUIRE(view.id().has_value());
    CHECK(*view.id() == "q1");
    REQUIRE(view.from().has_value());
    CHECK(*view.from() == "bob@example.org");

    const xmpp::StanzaView query = view.child("query", "jabber:iq:version");
    CHECK(query.valid());
    CHECK(query.name() == "query");

    xmpp_stanza_release(root);
}

TEST_CASE("message__htmldecode")
{
    auto decode = [](const std::string& src) -> std::string {
        std::string dest(src.size() + 1, '\0');
        message__htmldecode(dest.data(), src.c_str(), src.size() + 1);
        dest.resize(std::strlen(dest.c_str()));
        return dest;
    };

    SUBCASE("plain text passes through")
    {
        CHECK(decode("hello") == "hello");
    }

    SUBCASE("&gt; decoded to >")
    {
        CHECK(decode("a&gt;b") == "a>b");
    }

    SUBCASE("&lt; decoded to <")
    {
        CHECK(decode("a&lt;b") == "a<b");
    }

    SUBCASE("&amp; decoded to &")
    {
        CHECK(decode("a&amp;b") == "a&b");
    }

    SUBCASE("multiple entities in sequence")
    {
        CHECK(decode("&lt;tag&gt;") == "<tag>");
    }

    SUBCASE("entities adjacent to plain text")
    {
        CHECK(decode("x&amp;y&gt;z") == "x&y>z");
    }

    SUBCASE("unknown entity left as-is")
    {
        // &foo; is not decoded – chars are copied verbatim
        const std::string src = "&foo;";
        const std::string got = decode(src);
        CHECK(got.find('&') != std::string::npos);
    }
}

// =============================================================================
// TEST CASES – Group B: libstrophe stanza accessors and JID parser
// =============================================================================

TEST_CASE("strophe stanza accessors")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    static constexpr const char *xml =
        "<message from='alice@example.org/phone'"
        " to='bob@example.org'"
        " type='chat'"
        " id='msg1'>"
        "<body>Hello</body>"
        "</message>";

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml);
    REQUIRE(root != nullptr);

    SUBCASE("get_name returns element name")
    {
        CHECK(get_name(root) == "message");
    }

    SUBCASE("get_attribute present attribute")
    {
        auto from = get_attribute(root, "from");
        REQUIRE(from.has_value());
        CHECK(*from == "alice@example.org/phone");
    }

    SUBCASE("get_attribute missing attribute returns nullopt")
    {
        auto absent = get_attribute(root, "nonexistent");
        CHECK_FALSE(absent.has_value());
    }

    SUBCASE("get_name on child body element")
    {
        xmpp_stanza_t *body = xmpp_stanza_get_child_by_name(root, "body");
        REQUIRE(body != nullptr);
        CHECK(get_name(body) == "body");
    }

    SUBCASE("get_text on child body returns text content")
    {
        xmpp_stanza_t *body = xmpp_stanza_get_child_by_name(root, "body");
        REQUIRE(body != nullptr);
        // Body text is in a text child
        xmpp_stanza_t *text_node = xmpp_stanza_get_children(body);
        if (text_node && xmpp_stanza_is_text(text_node))
            CHECK(get_text(text_node) == "Hello");
    }

    xmpp_stanza_release(root);
}

TEST_CASE("JID parser")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    SUBCASE("full JID: local@domain/resource")
    {
        jid j(env.ctx, "alice@example.org/phone");
        CHECK(j.full     == "alice@example.org/phone");
        CHECK(j.bare     == "alice@example.org");
        CHECK(j.local    == "alice");
        CHECK(j.domain   == "example.org");
        CHECK(j.resource == "phone");
        CHECK(j.is_bare() == false);
    }

    SUBCASE("bare JID: local@domain")
    {
        jid j(env.ctx, "alice@example.org");
        CHECK(j.full     == "alice@example.org");
        CHECK(j.bare     == "alice@example.org");
        CHECK(j.local    == "alice");
        CHECK(j.domain   == "example.org");
        CHECK(j.resource.empty());
        CHECK(j.is_bare() == true);
    }

    SUBCASE("domain-only JID (server or component)")
    {
        jid j(env.ctx, "conference.example.org");
        CHECK(j.full   == "conference.example.org");
        CHECK(j.bare   == "conference.example.org");
        CHECK(j.domain == "conference.example.org");
        CHECK(j.local.empty());
        CHECK(j.resource.empty());
        CHECK(j.is_bare() == true);
    }

    SUBCASE("resource may be empty string (trailing slash)")
    {
        jid j(env.ctx, "alice@example.org/");
        CHECK(j.bare   == "alice@example.org");
        CHECK(j.local  == "alice");
        CHECK(j.domain == "example.org");
        CHECK(j.resource.empty());
    }

    SUBCASE("resource with spaces is captured")
    {
        jid j(env.ctx, "alice@example.org/res with spaces");
        CHECK(j.resource == "res with spaces");
    }

    SUBCASE("MUC nick with apostrophe is captured")
    {
        jid j(env.ctx, "gajim@conference.gajim.org/don't mention me");
        CHECK(j.bare == "gajim@conference.gajim.org");
        CHECK(j.resource == "don't mention me");
        CHECK(j.is_bare() == false);
    }
}

TEST_CASE("presence show/status extraction")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    // Helper: return first child with given name's text via real plugin symbols
    auto first_child_text = [&](xmpp_stanza_t *parent, const char *child_name)
        -> std::optional<std::string>
    {
        xmpp_stanza_t *ch = xmpp_stanza_get_child_by_name(parent, child_name);
        if (!ch)
            return std::nullopt;
        xmpp_stanza_t *txt = xmpp_stanza_get_children(ch);
        if (txt && xmpp_stanza_is_text(txt))
            return get_text(txt);
        return std::nullopt;
    };

    SUBCASE("presence with show and status")
    {
        static constexpr const char *xml =
            "<presence from='alice@example.org/phone'>"
            "<show>away</show>"
            "<status>Out for lunch</status>"
            "</presence>";

        xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml);
        REQUIRE(root != nullptr);

        auto show   = first_child_text(root, "show");
        auto status = first_child_text(root, "status");

        REQUIRE(show.has_value());
        CHECK(*show == "away");

        REQUIRE(status.has_value());
        CHECK(*status == "Out for lunch");

        xmpp_stanza_release(root);
    }

    SUBCASE("presence without show or status returns nullopt")
    {
        static constexpr const char *xml =
            "<presence from='alice@example.org/phone'/>";

        xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml);
        REQUIRE(root != nullptr);

        CHECK_FALSE(first_child_text(root, "show").has_value());
        CHECK_FALSE(first_child_text(root, "status").has_value());

        xmpp_stanza_release(root);
    }
}

// =============================================================================
// TEST CASES – Group B2: MAM IQ stanza builder (src/xmpp/xep-0313.inl)
// =============================================================================

// Helper: serialise a built stanza to XML string and release the raw pointer.
static std::string stanza_to_xml(xmpp_ctx_t *ctx, xmpp_stanza_t *raw)
{
    char *buf = nullptr;
    size_t len = 0;
    xmpp_stanza_to_text(raw, &buf, &len);
    std::string result(buf ? buf : "");
    if (buf) xmpp_free(ctx, buf);
    return result;
}

TEST_CASE("handle_version_iq builds XEP-0092 result")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *req = xmpp_stanza_new_from_string(
        env.ctx, "<iq type='get' id='v1' from='bob@example.org'/>");
    REQUIRE(req != nullptr);

    test_weechat::StubRuntimePort runtime("4.9.0-test");
    auto reply = xmpp::handle_version_iq(
        xmpp::StanzaView(req), runtime, "alice@example.org");
    auto built = reply.build(env.ctx);
    const std::string xml = stanza_to_xml(env.ctx, built.get());

    CHECK(xml.find("type=\"result\"") != std::string::npos);
    CHECK(xml.find("to=\"bob@example.org\"") != std::string::npos);
    CHECK(xml.find("from=\"alice@example.org\"") != std::string::npos);
    CHECK(xml.find("4.9.0-test") != std::string::npos);
    CHECK(xml.find("weechat") != std::string::npos);

    xmpp_stanza_release(req);
}

TEST_CASE("handle_time_iq builds XEP-0202 result")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *req = xmpp_stanza_new_from_string(
        env.ctx, "<iq type='get' id='t1' from='bob@example.org'/>");
    REQUIRE(req != nullptr);

    constexpr std::time_t now = 946684800;
    auto reply = xmpp::handle_time_iq(
        xmpp::StanzaView(req), "alice@example.org", now);
    auto built = reply.build(env.ctx);
    const std::string xml = stanza_to_xml(env.ctx, built.get());

    CHECK(xml.find("type=\"result\"") != std::string::npos);
    CHECK(xml.find("2000-01-01T00:00:00Z") != std::string::npos);

    xmpp_stanza_release(req);
}

TEST_CASE("handle_ping_iq and ping helpers")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *ping_get = xmpp_stanza_new_from_string(env.ctx,
        "<iq xmlns='jabber:client' type='get' id='p1' from='bob@example.org' to='alice@example.org'>"
        "<ping xmlns='urn:xmpp:ping'/>"
        "</iq>");
    REQUIRE(ping_get != nullptr);
    CHECK(xmpp::is_ping_get_iq(xmpp::StanzaView(ping_get)));

    auto reply = xmpp::handle_ping_iq(xmpp::StanzaView(ping_get), "alice@example.org");
    auto built = reply.build(env.ctx);
    const std::string xml = stanza_to_xml(env.ctx, built.get());
    CHECK(xml.find("type=\"result\"") != std::string::npos);
    CHECK(xml.find("to=\"bob@example.org\"") != std::string::npos);
    CHECK(xml.find("from=\"alice@example.org\"") != std::string::npos);

    CHECK(xmpp::compute_ping_rtt_ms(100, 101) == 1000);

    const auto muc_from = xmpp::parse_muc_ping_from("room@conf.example/nick");
    REQUIRE(muc_from.has_value());
    CHECK(muc_from->room_jid == "room@conf.example");
    CHECK(muc_from->resource == "nick");

    CHECK(xmpp::is_muc_self_ping(
        *muc_from,
        "nick",
        [](std::string_view room) { return room == "room@conf.example"; }));
    CHECK_FALSE(xmpp::is_muc_self_ping(
        *muc_from,
        "other",
        [](std::string_view) { return true; }));

    xmpp_stanza_t *err = xmpp_stanza_new_from_string(env.ctx,
        "<error type='cancel' xmlns='jabber:client'>"
        "<service-unavailable xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
        "</error>");
    REQUIRE(err != nullptr);
    CHECK(xmpp::classify_muc_self_ping_error(xmpp::StanzaView(err))
          == xmpp::MucSelfPingErrorOutcome::still_joined);

    xmpp_stanza_t *err_text = xmpp_stanza_new_from_string(env.ctx,
        "<error type='auth' xmlns='jabber:client'>"
        "<text xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'>denied</text>"
        "<not-allowed xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
        "</error>");
    REQUIRE(err_text != nullptr);
    CHECK(xmpp::iq_error_text(xmpp::StanzaView(err_text)) == "denied");

    xmpp_stanza_release(ping_get);
    xmpp_stanza_release(err);
    xmpp_stanza_release(err_text);
}

TEST_CASE("MAM PM IQ stanza builder: structure and attributes")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    // Reproduce what fetch_mam() does for a PM channel
    const std::string mam_id    = "test-uuid-1234";
    const std::string partner   = "bob@example.org";
    const std::string own_bare  = "alice@example.org";

    stanza::xep0313::x_filter xf;
    xf.with(partner);

    stanza::xep0313::query q;
    q.queryid(mam_id).filter(xf);

    stanza::iq iq_s;
    iq_s.id(mam_id).type("set").to(own_bare);
    iq_s.xep0313().query(q);

    auto built = iq_s.build(env.ctx);
    REQUIRE(built != nullptr);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    // Top-level IQ attributes
    CHECK(xml.find("id=\"test-uuid-1234\"")  != std::string::npos);
    CHECK(xml.find("type=\"set\"")            != std::string::npos);
    CHECK(xml.find("to=\"alice@example.org\"") != std::string::npos);

    // <query> element with MAM namespace and queryid
    CHECK(xml.find("xmlns=\"urn:xmpp:mam:2\"") != std::string::npos);
    CHECK(xml.find("queryid=\"test-uuid-1234\"") != std::string::npos);

    // jabber:x:data form for the filter
    CHECK(xml.find("xmlns=\"jabber:x:data\"") != std::string::npos);
    CHECK(xml.find("type=\"submit\"")          != std::string::npos);

    // FORM_TYPE hidden field
    CHECK(xml.find("urn:xmpp:mam:2") != std::string::npos);

    // <with> field for the partner JID
    CHECK(xml.find("bob@example.org") != std::string::npos);
}

TEST_CASE("MAM PM IQ stanza: <with> field appears inside <query>")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    stanza::xep0313::x_filter xf;
    xf.with("carol@example.org");

    stanza::xep0313::query q;
    q.queryid("qid-abc").filter(xf);

    stanza::iq iq_s;
    iq_s.id("qid-abc").type("set");
    iq_s.xep0313().query(q);

    auto built = iq_s.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    // Parse with libstrophe to check structural nesting
    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(root != nullptr);

    xmpp_stanza_t *query_el = xmpp_stanza_get_child_by_name_and_ns(
        root, "query", "urn:xmpp:mam:2");
    CHECK(query_el != nullptr);

    if (query_el)
    {
        xmpp_stanza_t *x_el = xmpp_stanza_get_child_by_name_and_ns(
            query_el, "x", "jabber:x:data");
        CHECK(x_el != nullptr);

        bool found_with_field = false;
        if (x_el)
        {
            for (xmpp_stanza_t *field = xmpp_stanza_get_children(x_el);
                 field; field = xmpp_stanza_get_next(field))
            {
                const char *var = xmpp_stanza_get_attribute(field, "var");
                if (var && std::string_view(var) == "with")
                {
                    xmpp_stanza_t *val = xmpp_stanza_get_child_by_name(field, "value");
                    if (val)
                    {
                        const std::string val_text = stanza_element_text(val);
                        CHECK(!val_text.empty());
                        CHECK(val_text == "carol@example.org");
                    }
                    found_with_field = true;
                    break;
                }
            }
        }
        CHECK(found_with_field);
    }

    xmpp_stanza_release(root);
}

TEST_CASE("MAM RSM pagination IQ: <after> element present in <set>")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    // Reproduce the pagination branch in fetch_mam() (after != nullptr)
    stanza::xep0313::x_filter xf;
    xf.with("dave@example.org");

    stanza::xep0313::query q;
    q.queryid("page2-id").filter(xf);

    stanza::xep0059::set rsm;
    rsm.after("last-seen-uid-xyz");
    q.rsm(rsm);

    stanza::iq iq_s;
    iq_s.id("page2-id").type("set");
    iq_s.xep0313().query(q);

    auto built = iq_s.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(root != nullptr);

    xmpp_stanza_t *query_el = xmpp_stanza_get_child_by_name_and_ns(
        root, "query", "urn:xmpp:mam:2");
    REQUIRE(query_el != nullptr);

    xmpp_stanza_t *set_el = xmpp_stanza_get_child_by_name_and_ns(
        query_el, "set", "http://jabber.org/protocol/rsm");
    CHECK(set_el != nullptr);

    if (set_el)
    {
        xmpp_stanza_t *after_el = xmpp_stanza_get_child_by_name(set_el, "after");
        CHECK(after_el != nullptr);
        if (after_el)
        {
            const std::string after_text = stanza_element_text(after_el);
            CHECK(!after_text.empty());
            CHECK(after_text == "last-seen-uid-xyz");
        }
    }

    xmpp_stanza_release(root);
}

// =============================================================================
// TEST CASES – Group C: consistent_color / angle_to_weechat_color (src/color.cpp)
// =============================================================================

TEST_CASE("angle_to_weechat_color output range")
{
    SUBCASE("result is always in [16, 231]")
    {
        for (int i = 0; i <= 360; i += 15)
        {
            int color = std::stoi(weechat::angle_to_weechat_color(static_cast<double>(i)));
            CHECK(color >= 16);
            CHECK(color <= 231);
        }
    }

    SUBCASE("normalises negative angle to same result as positive equivalent")
    {
        CHECK(weechat::angle_to_weechat_color(-90.0) == weechat::angle_to_weechat_color(270.0));
    }

    SUBCASE("normalises angle >= 360 to same result as modulo equivalent")
    {
        CHECK(weechat::angle_to_weechat_color(360.0) == weechat::angle_to_weechat_color(0.0));
        CHECK(weechat::angle_to_weechat_color(720.0) == weechat::angle_to_weechat_color(0.0));
        CHECK(weechat::angle_to_weechat_color(400.0) == weechat::angle_to_weechat_color(40.0));
    }

    SUBCASE("0 degrees maps to a valid color")
    {
        int c = std::stoi(weechat::angle_to_weechat_color(0.0));
        CHECK(c >= 16);
        CHECK(c <= 231);
    }
}

// doctest line-number collision guard (omemo_mechanics.inl)
TEST_CASE("consistent_color")
{
    SUBCASE("empty string returns empty")
    {
        CHECK(weechat::consistent_color("") == "");
    }

    SUBCASE("non-empty string returns a color in [16, 231]")
    {
        int c = std::stoi(weechat::consistent_color("alice@example.org"));
        CHECK(c >= 16);
        CHECK(c <= 231);
    }

    SUBCASE("case-insensitive: same color for different cases")
    {
        CHECK(weechat::consistent_color("Alice") == weechat::consistent_color("alice"));
        CHECK(weechat::consistent_color("ALICE") == weechat::consistent_color("alice"));
        CHECK(weechat::consistent_color("AlIcE@Example.ORG")
              == weechat::consistent_color("alice@example.org"));
    }

    SUBCASE("different strings produce potentially different colors (determinism)")
    {
        CHECK(weechat::consistent_color("bob@example.org")
              == weechat::consistent_color("bob@example.org"));
    }

    SUBCASE("different JIDs do not necessarily map to the same color")
    {
        CHECK(weechat::consistent_color("alice@example.org")
              != weechat::consistent_color("zzzz@totally-different-domain.net"));
    }
}

// =============================================================================
// TEST CASES – Group D: Message envelope builders (XEP-0184, 0333, 0359, 0085, 0428, 0066, 0382, 0249)
// =============================================================================

// Helper: find first child with given name+ns, return raw pointer (or nullptr)
static xmpp_stanza_t *find_child_ns(xmpp_stanza_t *parent, const char *name, const char *ns)
{
    return xmpp_stanza_get_child_by_name_and_ns(parent, name, ns);
}

// Helper: get attribute as optional string
static std::optional<std::string> attr_opt(xmpp_stanza_t *el, const char *name)
{
    const char *v = xmpp_stanza_get_attribute(el, name);
    if (!v) return std::nullopt;
    return std::string(v);
}

// Helper: get text content of first text child
static std::optional<std::string> text_opt(xmpp_stanza_t *el)
{
    const std::string t = stanza_element_text(el);
    if (t.empty())
        return std::nullopt;
    return t;
}

TEST_CASE("collect_sims_shares parses XEP-0385 reference")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *msg = xmpp_stanza_new_from_string(env.ctx,
        "<message xmlns='jabber:client' type='chat'>"
        "<body>photo</body>"
        "<reference xmlns='urn:xmpp:reference:0' type='data'>"
        "<media-sharing xmlns='urn:xmpp:sims:1'>"
        "<file xmlns='urn:xmpp:jingle:apps:file-transfer:5'>"
        "<name>pic.png</name><media-type>image/png</media-type><size>1234</size>"
        "</file>"
        "<sources>"
        "<reference xmlns='urn:xmpp:reference:0' type='data' uri='https://ex/img.png'/>"
        "</sources>"
        "</media-sharing>"
        "</reference>"
        "</message>");
    REQUIRE(msg != nullptr);

    auto shares = xmpp::collect_sims_shares(xmpp::StanzaView(msg));
    REQUIRE(shares.size() == 1);
    CHECK(shares[0].meta.name == "pic.png");
    CHECK(shares[0].meta.mime == "image/png");
    CHECK(shares[0].url == "https://ex/img.png");

    xmpp_stanza_release(msg);
}

TEST_CASE("stanza_has_sticker detects XEP-0449 sticker element")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *msg = xmpp_stanza_new_from_string(env.ctx,
        "<message xmlns='jabber:client' type='chat'>"
        "<sticker xmlns='urn:xmpp:stickers:0'/>"
        "<file-sharing xmlns='urn:xmpp:sfs:0'>"
        "<file xmlns='urn:xmpp:file:metadata:0'>"
        "<media-type>image/png</media-type>"
        "</file>"
        "<sources><url-data target='https://ex/sticker.png'/></sources>"
        "</file-sharing>"
        "</message>");
    REQUIRE(msg != nullptr);
    CHECK(xmpp::stanza_has_sticker(xmpp::StanzaView(msg)));
    xmpp_stanza_release(msg);
}

TEST_CASE("collect_custom_emoji_previews resolves XEP-0514 emoji markup")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *msg = xmpp_stanza_new_from_string(env.ctx,
        "<message xmlns='jabber:client' type='chat'>"
        "<body>To be, or not to be 🤔</body>"
        "<markup xmlns='urn:xmpp:markup:0'>"
        "<span start='42' end='43'>"
        "<emoji xmlns='urn:xmpp:markup:emoji:0' name='pondering'>"
        "<hash xmlns='urn:xmpp:hashes:2' algo='sha3-256'>"
        "ENeyvkxcfv8dmL4HBrF3JU1OX1BfpNV3YbhlEb20ReU="
        "</hash>"
        "</emoji>"
        "</span>"
        "</markup>"
        "<file-sharing xmlns='urn:xmpp:sfs:0'>"
        "<file xmlns='urn:xmpp:file:metadata:0'>"
        "<media-type>image/png</media-type>"
        "<name>pondering</name>"
        "<width>64</width><height>64</height>"
        "<hash xmlns='urn:xmpp:hashes:2' algo='sha3-256'>"
        "ENeyvkxcfv8dmL4HBrF3JU1OX1BfpNV3YbhlEb20ReU="
        "</hash>"
        "</file>"
        "<sources>"
        "<url-data target='https://download.example/pondering.png'/>"
        "</sources>"
        "</file-sharing>"
        "</message>");
    REQUIRE(msg != nullptr);

    const auto previews = xmpp::collect_custom_emoji_previews(xmpp::StanzaView(msg));
    REQUIRE(previews.size() == 1);
    CHECK(previews[0].url == "https://download.example/pondering.png");
    CHECK(previews[0].mime == "image/png");
    CHECK(previews[0].width == 64);
    CHECK(previews[0].height == 64);
    CHECK(previews[0].name == "pondering");

    const auto keys = xmpp::collect_emoji_markup_hash_keys(xmpp::StanzaView(msg));
    CHECK(keys.contains("sha3-256:ENeyvkxcfv8dmL4HBrF3JU1OX1BfpNV3YbhlEb20ReU="));

    xmpp_stanza_release(msg);
}

TEST_CASE("collect_sfs_shares parses plain and encrypted sources")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *plain = xmpp_stanza_new_from_string(env.ctx,
        "<message xmlns='jabber:client' type='chat'>"
        "<file-sharing xmlns='urn:xmpp:sfs:0'>"
        "<file xmlns='urn:xmpp:file:metadata:0'><name>a.txt</name></file>"
        "<sources><url-data target='https://ex/a.txt'/></sources>"
        "</file-sharing>"
        "</message>");
    REQUIRE(plain != nullptr);

    auto plain_shares = xmpp::collect_sfs_shares(xmpp::StanzaView(plain));
    REQUIRE(plain_shares.size() == 1);
    REQUIRE(plain_shares[0].plain_url.has_value());
    CHECK(*plain_shares[0].plain_url == "https://ex/a.txt");

    xmpp_stanza_t *enc = xmpp_stanza_new_from_string(env.ctx,
        "<message xmlns='jabber:client' type='chat'>"
        "<file-sharing xmlns='urn:xmpp:sfs:0'>"
        "<file xmlns='urn:xmpp:file:metadata:0'><name>secret.bin</name></file>"
        "<sources>"
        "<encrypted xmlns='urn:xmpp:esfs:0'"
        " cipher='urn:xmpp:ciphers:aes-256-gcm-nopadding:0'>"
        "<key>a2V5</key><iv>aXY=</iv>"
        "<sources><url-data target='https://ex/ct.bin'/></sources>"
        "</encrypted>"
        "</sources>"
        "</file-sharing>"
        "</message>");
    REQUIRE(enc != nullptr);

    auto enc_shares = xmpp::collect_sfs_shares(xmpp::StanzaView(enc));
    REQUIRE(enc_shares.size() == 1);
    REQUIRE(enc_shares[0].encrypted.has_value());
    CHECK(enc_shares[0].encrypted->ciphertext_url == "https://ex/ct.bin");
    CHECK(enc_shares[0].encrypted->key_b64 == "a2V5");

    xmpp_stanza_release(plain);
    xmpp_stanza_release(enc);
}

TEST_CASE("message_omemo stable id and self-copy policy")
{
    CHECK(xmpp::omemo_stable_id({
              "origin-1", "stanza-1", "msg-1"}) == "origin-1");
    CHECK(xmpp::omemo_stable_id({
              "", "stanza-1", "msg-1"}) == "stanza-1");
    CHECK(xmpp::omemo_stable_id({
              "", "", "msg-1"}) == "msg-1");

    const auto live_own_copy = xmpp::evaluate_omemo_self_copy_advice(
        true, true, false, true);
    CHECK(live_own_copy.apply_advice);
    CHECK_FALSE(live_own_copy.clear_encrypted_on_mam);

    const auto live_carbon_other_device = xmpp::evaluate_omemo_self_copy_advice(
        true, true, false, false, true);
    CHECK_FALSE(live_carbon_other_device.apply_advice);

    const auto mam_other_device = xmpp::evaluate_omemo_self_copy_advice(
        true, true, true, false);
    CHECK_FALSE(mam_other_device.apply_advice);

    const auto mam_own_device = xmpp::evaluate_omemo_self_copy_advice(
        true, true, true, true);
    CHECK(mam_own_device.apply_advice);
    CHECK(mam_own_device.clear_encrypted_on_mam);
}

TEST_CASE("message_omemo parses axolotl header and decode jid")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *msg = xmpp_stanza_new_from_string(env.ctx,
        "<message xmlns='jabber:client' type='chat'>"
        "<encrypted xmlns='eu.siacs.conversations.axolotl'>"
        "<header sid='424242'><keys/></header>"
        "<payload>UEFZ</payload>"
        "</encrypted>"
        "</message>");
    REQUIRE(msg != nullptr);

    const auto view = xmpp::StanzaView(msg);
    const auto enc = xmpp::stanza_axolotl_encrypted(view);
    REQUIRE(enc.valid());
    REQUIRE(xmpp::axolotl_header_sender_id(enc) == 424242u);
    CHECK(xmpp::is_own_device_omemo_self_copy(enc, 424242u));
    CHECK_FALSE(xmpp::is_own_device_omemo_self_copy(enc, 1u));
    CHECK_FALSE(xmpp::axolotl_payload_is_empty(enc));

    CHECK(xmpp::resolve_omemo_decode_jid("room@conf.example", std::nullopt)
          == "room@conf.example");
    CHECK(xmpp::resolve_omemo_decode_jid(
              "room@conf.example", std::string_view("alice@example.org"))
          == "alice@example.org");

    xmpp_stanza_release(msg);
}

TEST_CASE("message_omemo decrypt failure disposition")
{
    using xmpp::OmemoDecryptFailureDisposition;
    using xmpp::OmemoDecryptFailureInput;

    CHECK(xmpp::disposition_for_omemo_decrypt_failure(
              OmemoDecryptFailureInput{.is_self_outbound_copy = true})
          == OmemoDecryptFailureDisposition::ContinueAfterOmemo);
    CHECK(xmpp::disposition_for_omemo_decrypt_failure(
              OmemoDecryptFailureInput{
                  .is_self_outbound_copy = true, .is_carbon_copy = true})
          == OmemoDecryptFailureDisposition::ShowUndecryptablePlaceholder);
    CHECK(xmpp::disposition_for_omemo_decrypt_failure(
              OmemoDecryptFailureInput{.is_mam_replay = true})
          == OmemoDecryptFailureDisposition::ShowUndecryptablePlaceholder);
    CHECK(xmpp::disposition_for_omemo_decrypt_failure(
              OmemoDecryptFailureInput{.payload_missing_or_empty = true})
          == OmemoDecryptFailureDisposition::AbortSilent);
    CHECK(xmpp::disposition_for_omemo_decrypt_failure({})
          == OmemoDecryptFailureDisposition::ShowDecryptionError);

    CHECK(xmpp::should_note_omemo_peer_traffic(true, false, true, false));
    CHECK_FALSE(xmpp::should_note_omemo_peer_traffic(true, true, true, false));
    CHECK_FALSE(xmpp::should_note_omemo_peer_traffic(true, false, true, true));
    CHECK(xmpp::should_auto_enable_channel_omemo(true, false, false, false));
    CHECK_FALSE(xmpp::should_auto_enable_channel_omemo(true, false, true, false));
    CHECK_FALSE(xmpp::should_auto_enable_channel_omemo(true, false, false, true));

    const auto cache_ids = xmpp::omemo_plaintext_cache_ids({
        "sid", "origin", "sid"});
    REQUIRE(cache_ids.size() == 2);
    CHECK(cache_ids[0] == "sid");
    CHECK(cache_ids[1] == "origin");

    CHECK(xmpp::should_skip_display_after_omemo(true, false, true, false));
    CHECK_FALSE(xmpp::should_skip_display_after_omemo(true, false, true, false, true));
    CHECK_FALSE(xmpp::should_skip_display_after_omemo(false, false, true, false));
}

TEST_CASE("parse_direct_muc_invite reads XEP-0249 attributes")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *msg = xmpp_stanza_new_from_string(env.ctx,
        "<message xmlns='jabber:client' from='alice@example.org' to='bob@example.org'>"
        "<x xmlns='jabber:x:conference' jid='room@conf.example' password='secret'"
        " reason='join us'/>"
        "</message>");
    REQUIRE(msg != nullptr);

    auto invite = xmpp::parse_direct_muc_invite(xmpp::StanzaView(msg));
    REQUIRE(invite.has_value());
    CHECK(invite->inviter_bare == "alice@example.org");
    CHECK(invite->room_jid == "room@conf.example");
    REQUIRE(invite->password.has_value());
    CHECK(*invite->password == "secret");
    REQUIRE(invite->reason.has_value());
    CHECK(*invite->reason == "join us");

    xmpp_stanza_release(msg);
}

TEST_CASE("parse_mediated_muc_invite reads XEP-0045 muc#user invite")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *msg = xmpp_stanza_new_from_string(env.ctx,
        "<message xmlns='jabber:client' from='room@conf.example' to='bob@example.org'>"
        "<x xmlns='http://jabber.org/protocol/muc#user'>"
        "<invite to='bob@example.org' from='alice@example.org'>"
        "<reason>join us</reason></invite>"
        "<password>secret</password>"
        "</x></message>");
    REQUIRE(msg != nullptr);

    auto invite = xmpp::parse_mediated_muc_invite(xmpp::StanzaView(msg));
    REQUIRE(invite.has_value());
    CHECK(invite->room_jid == "room@conf.example");
    REQUIRE(invite->inviter_bare.has_value());
    CHECK(*invite->inviter_bare == "alice@example.org");
    REQUIRE(invite->reason.has_value());
    CHECK(*invite->reason == "join us");
    REQUIRE(invite->password.has_value());
    CHECK(*invite->password == "secret");

    xmpp_stanza_release(msg);
}

TEST_CASE("render_mediated_muc_invite_notification includes decline hint")
{
    xmpp::MediatedMucInvite invite;
    invite.room_jid = "room@conf.example";
    invite.inviter_bare = "alice@example.org";
    invite.reason = "join us";

    const auto lines = xmpp::render_mediated_muc_invite_notification(invite).network_lines;
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].contains("alice@example.org"));
    CHECK(lines[0].contains("room@conf.example"));
    CHECK(lines[1].contains("/join room@conf.example"));
    CHECK(lines[2].contains("/decline"));
}

TEST_CASE("parse_muc_admin_list_items reads muc#admin affiliation list")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *query = xmpp_stanza_new_from_string(env.ctx,
        "<query xmlns='http://jabber.org/protocol/muc#admin'>"
        "<item affiliation='member' jid='bob@example.org' nick='bob'/>"
        "</query>");
    REQUIRE(query != nullptr);

    const auto items = xmpp::parse_muc_admin_list_items(xmpp::StanzaView{query});
    REQUIRE(items.size() == 1);
    CHECK(items[0].jid == "bob@example.org");
    CHECK(items[0].nick == "bob");
    CHECK(items[0].affiliation == "member");

    xmpp_stanza_release(query);
}

TEST_CASE("parse_mediated_muc_decline reads XEP-0045 muc#user decline")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *msg = xmpp_stanza_new_from_string(env.ctx,
        "<message xmlns='jabber:client' from='room@conf.example' to='alice@example.org/desktop'>"
        "<x xmlns='http://jabber.org/protocol/muc#user'>"
        "<decline from='bob@example.org'>"
        "<reason>busy</reason></decline>"
        "</x></message>");
    REQUIRE(msg != nullptr);

    auto decline = xmpp::parse_mediated_muc_decline(xmpp::StanzaView(msg));
    REQUIRE(decline.has_value());
    CHECK(decline->room_jid == "room@conf.example");
    REQUIRE(decline->decliner_bare.has_value());
    CHECK(*decline->decliner_bare == "bob@example.org");
    REQUIRE(decline->reason.has_value());
    CHECK(*decline->reason == "busy");

    xmpp_stanza_release(msg);
}

TEST_CASE("message_ephemeral spoiler and fallback helpers")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *eph = xmpp_stanza_new_from_string(env.ctx,
        "<message xmlns='jabber:client' type='chat'>"
        "<ephemeral xmlns='urn:xmpp:ephemeral:0' timer='30'/>"
        "<body>hi</body>"
        "</message>");
    REQUIRE(eph != nullptr);
    CHECK(xmpp::parse_ephemeral_timer(xmpp::StanzaView(eph)) == 30);
    CHECK(xmpp::should_schedule_ephemeral_tombstone(30, "msg-1"));
    CHECK_FALSE(xmpp::should_schedule_ephemeral_tombstone(30, ""));

    xmpp_stanza_t *spoiler = xmpp_stanza_new_from_string(env.ctx,
        "<message xmlns='jabber:client' type='chat'>"
        "<spoiler xmlns='urn:xmpp:spoiler:0'>ending</spoiler>"
        "<body>text</body>"
        "</message>");
    REQUIRE(spoiler != nullptr);
    auto hint = xmpp::parse_spoiler_hint(xmpp::StanzaView(spoiler));
    REQUIRE(hint.has_value());
    CHECK(*hint == "ending");

    xmpp_stanza_t *reply = xmpp_stanza_new_from_string(env.ctx,
        "<message xmlns='jabber:client' type='chat'>"
        "<reply xmlns='urn:xmpp:reply:0' to='orig'/>"
        "<fallback xmlns='urn:xmpp:fallback:0'>"
        "<body start='0' end='7'/>"
        "</fallback>"
        "<body>&gt; quote\n\nanswer</body>"
        "</message>");
    REQUIRE(reply != nullptr);
    const auto fb = xmpp::apply_fallback_body_trim(
        xmpp::StanzaView(reply), "> quote\n\nanswer", false);
    CHECK(fb.disposition == xmpp::FallbackBodyDisposition::Trimmed);
    CHECK(fb.trimmed == "answer");

    xmpp_stanza_t *reactions = xmpp_stanza_new_from_string(env.ctx,
        "<message xmlns='jabber:client' type='chat'>"
        "<fallback xmlns='urn:xmpp:fallback:0'/>"
        "<reactions xmlns='urn:xmpp:reactions:0'><reaction>👍</reaction></reactions>"
        "<body>ignored</body>"
        "</message>");
    REQUIRE(reactions != nullptr);
    CHECK(xmpp::apply_fallback_body_trim(
              xmpp::StanzaView(reactions), "ignored", false).disposition
          == xmpp::FallbackBodyDisposition::Cleared);

    xmpp_stanza_t *err = xmpp_stanza_new_from_string(env.ctx,
        "<message xmlns='jabber:client' type='error'/>");
    REQUIRE(err != nullptr);
    CHECK(xmpp::stanza_is_error_message(xmpp::StanzaView(err)));

    xmpp_stanza_release(eph);
    xmpp_stanza_release(spoiler);
    xmpp_stanza_release(reply);
    xmpp_stanza_release(reactions);
    xmpp_stanza_release(err);
}

TEST_CASE("message_line_tag and correction/retraction parsing")
{
    CHECK(xmpp::line_tag_matches_message_id("id_abc", "abc"));
    CHECK(xmpp::line_tag_matches_message_id("stanza_id_abc", "abc"));
    CHECK(xmpp::line_tag_matches_message_id("origin_id_abc", "abc"));
    CHECK_FALSE(xmpp::line_tag_matches_message_id("nick_alice", "abc"));
    CHECK(xmpp::line_tag_matches_nick_sender("nick_alice", "alice"));
    CHECK(xmpp::line_tag_matches_occupant_sender("occupant_id_o1", "o1"));

    xmpp::LineSenderVerify nick_verify{ .sender_key = "alice" };
    const std::string_view nick_tags[] = { "id_m1", "nick_alice" };
    CHECK(xmpp::line_tags_verify_sender(nick_tags, nick_verify));

    xmpp::LineSenderVerify occ_verify{
        .sender_key = "alice",
        .occupant_id = std::string("occ-9"),
        .prefer_occupant_id = true,
    };
    const std::string_view occ_tags[] = { "occupant_id_occ-9" };
    CHECK(xmpp::line_tags_verify_sender(occ_tags, occ_verify));
    const std::string_view alice_only[] = { "nick_alice" };
    CHECK_FALSE(xmpp::line_tags_verify_sender(alice_only, occ_verify));

    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *corr = xmpp_stanza_new_from_string(env.ctx,
        "<message xmlns='jabber:client' type='groupchat' from='room@conf/alice'>"
        "<replace xmlns='urn:xmpp:message-correct:0' id='m1'/>"
        "<body>fixed</body>"
        "</message>");
    REQUIRE(corr != nullptr);
    CHECK(xmpp::stanza_has_message_correction(xmpp::StanzaView(corr)));
    auto parsed_corr = xmpp::parse_message_correction(xmpp::StanzaView(corr));
    REQUIRE(parsed_corr.has_value());
    CHECK(parsed_corr->target_id == "m1");
    CHECK(xmpp::message_correction_sender_key(
              "room@conf/alice", "room@conf", true) == "alice");
    CHECK(xmpp::format_message_correction_text("hi") == "📝 hi");

    xmpp_stanza_t *retract = xmpp_stanza_new_from_string(env.ctx,
        "<message xmlns='jabber:client' type='chat' from='alice@example.org'>"
        "<retract xmlns='urn:xmpp:message-retract:1' id='m2'/>"
        "</message>");
    REQUIRE(retract != nullptr);
    auto parsed_retract = xmpp::parse_message_retraction(xmpp::StanzaView(retract));
    REQUIRE(parsed_retract.has_value());
    CHECK(parsed_retract->target_id == "m2");

    xmpp_stanza_t *moderated = xmpp_stanza_new_from_string(env.ctx,
        "<message xmlns='jabber:client' type='groupchat' from='room@conf'>"
        "<retract xmlns='urn:xmpp:message-retract:1' id='m3'>"
        "<moderated xmlns='urn:xmpp:message-moderate:1'>"
        "<reason>spam</reason>"
        "</moderated>"
        "</retract>"
        "</message>");
    REQUIRE(moderated != nullptr);
    CHECK_FALSE(xmpp::parse_message_retraction(xmpp::StanzaView(moderated)).has_value());
    auto parsed_mod = xmpp::parse_moderated_retraction(xmpp::StanzaView(moderated));
    REQUIRE(parsed_mod.has_value());
    CHECK(parsed_mod->target_id == "m3");
    REQUIRE(parsed_mod->reason.has_value());
    CHECK(*parsed_mod->reason == "spam");
    CHECK(xmpp::should_accept_moderation_from_sender("room@conf", "room@conf"));
    CHECK_FALSE(xmpp::should_accept_moderation_from_sender("alice@example.org", "room@conf"));

    xmpp_stanza_release(corr);
    xmpp_stanza_release(retract);
    xmpp_stanza_release(moderated);
}

TEST_CASE("message_reactions and reply helpers")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *rxn = xmpp_stanza_new_from_string(env.ctx,
        "<message xmlns='jabber:client' type='chat' from='alice@example.org'>"
        "<reactions xmlns='urn:xmpp:reactions:0' id='m9'>"
        "<reaction>👍</reaction><reaction>❤️</reaction>"
        "</reactions>"
        "</message>");
    REQUIRE(rxn != nullptr);
    auto parsed_rxn = xmpp::parse_message_reactions(xmpp::StanzaView(rxn));
    REQUIRE(parsed_rxn.has_value());
    CHECK(parsed_rxn->target_id == "m9");
    CHECK(parsed_rxn->emojis == "👍 ❤️");

    xmpp_stanza_t *rxn_clear = xmpp_stanza_new_from_string(env.ctx,
        "<message xmlns='jabber:client' type='chat'>"
        "<reactions xmlns='urn:xmpp:reactions:0' id='m9'/>"
        "</message>");
    REQUIRE(rxn_clear != nullptr);
    auto parsed_clear = xmpp::parse_message_reactions(xmpp::StanzaView(rxn_clear));
    REQUIRE(parsed_clear.has_value());
    CHECK(parsed_clear->emojis.empty());

    xmpp_stanza_t *reply = xmpp_stanza_new_from_string(env.ctx,
        "<message xmlns='jabber:client' type='groupchat' from='room@conf/bob'>"
        "<reply xmlns='urn:xmpp:reply:0' id='orig-1'/>"
        "<body>answer</body>"
        "</message>");
    REQUIRE(reply != nullptr);
    auto parsed_reply = xmpp::parse_message_reply(xmpp::StanzaView(reply));
    REQUIRE(parsed_reply.has_value());
    CHECK(parsed_reply->target_id == "orig-1");

    CHECK_FALSE(xmpp::is_og_preview_continuation_line("hello"));
    CHECK(xmpp::is_og_preview_continuation_line("\xE2\x94\x8C title"));

    CHECK(xmpp::strip_leading_reply_chain(
              "\xE2\x86\xAA alice: hi there") == "alice: hi there");
    CHECK(xmpp::build_reply_excerpt("short") == "short");

    const std::string long_line(201, 'x');
    CHECK(xmpp::should_truncate_reply_excerpt(long_line));
    CHECK(xmpp::build_reply_excerpt(long_line) == std::string(40, 'x') + "...");

    const std::string_view tag_list[] = { "id_m1", "nick_carol" };
    auto nick = xmpp::nick_from_line_tags(tag_list);
    REQUIRE(nick.has_value());
    CHECK(*nick == "carol");

    xmpp_stanza_release(rxn);
    xmpp_stanza_release(rxn_clear);
    xmpp_stanza_release(reply);
}

TEST_CASE("render_event ack builders")
{
    const auto receipt = weechat::build_incoming_receipt_render_event("msg-1", false);
    REQUIRE(receipt.size() == 1);
    const auto *glyph = std::get_if<weechat::UpdateLineGlyphByTagAction>(&receipt.front());
    REQUIRE(glyph != nullptr);
    CHECK(glyph->acked_id == "msg-1");
    CHECK(glyph->glyph == weechat::k_glyph_delivered);

    const auto displayed = weechat::build_incoming_displayed_render_event("msg-2", false);
    REQUIRE(displayed.size() == 1);
    const auto *seen = std::get_if<weechat::UpdateLineGlyphByTagAction>(&displayed.front());
    REQUIRE(seen != nullptr);
    CHECK(seen->glyph == weechat::k_glyph_seen);

    CHECK(weechat::build_incoming_receipt_render_event("msg-1", true).empty());
    CHECK(weechat::build_incoming_displayed_render_event("", false).empty());
}

TEST_CASE("render_event line_store builders")
{
    const auto reactions = weechat::build_reactions_render_event("m1", "👍");
    REQUIRE(reactions.size() == 1);
    const auto *rx = std::get_if<weechat::ApplyReactionsByIdAction>(&reactions.front());
    REQUIRE(rx != nullptr);
    CHECK(rx->target_id == "m1");
    CHECK(rx->emojis == "👍");

    const auto correction = weechat::build_correction_render_event("m2", "fixed text");
    REQUIRE(correction.size() == 1);
    const auto *corr = std::get_if<weechat::UpdateMessageByIdAction>(&correction.front());
    REQUIRE(corr != nullptr);
    CHECK(corr->target_id == "m2");
    CHECK(corr->new_message == "fixed text");

    const auto moderation = weechat::build_moderation_tombstone_render_event(
        "m3", "moderated", "xmpp_moderated,notify_none");
    REQUIRE(moderation.size() == 1);
    const auto *mod = std::get_if<weechat::TombstoneMessageByIdAction>(&moderation.front());
    REQUIRE(mod != nullptr);
    CHECK(mod->target_id == "m3");
    CHECK(mod->tombstone_message == "moderated");

    const auto retraction = weechat::build_retraction_tombstone_render_event(
        "m4", "retracted", "xmpp_retracted,notify_none", "alice", "occ-1", true);
    REQUIRE(retraction.size() == 1);
    const auto *ret = std::get_if<weechat::TombstoneRetractionByIdAction>(&retraction.front());
    REQUIRE(ret != nullptr);
    CHECK(ret->target_id == "m4");
    CHECK(ret->sender_key == "alice");
    CHECK(ret->occupant_id == "occ-1");
    CHECK(ret->prefer_occupant_id);

    CHECK(weechat::build_reactions_render_event("", "👍").empty());
    CHECK(weechat::build_correction_render_event("", "x").empty());
}

TEST_CASE("handler harness applies line_store render events")
{
    test_weechat::HandlerTestHarness harness;

    harness.apply(weechat::build_reactions_render_event("m1", "❤️"));
    REQUIRE(harness.line_store.reactions.size() == 1);
    CHECK(harness.line_store.reactions.front().target_id == "m1");
    CHECK(harness.line_store.reactions.front().emojis == "❤️");

    harness.clear();

    harness.apply(weechat::build_correction_render_event("m2", "updated"));
    REQUIRE(harness.line_store.message_updates.size() == 1);
    CHECK(harness.line_store.message_updates.front().new_message == "updated");

    harness.clear();

    harness.apply(weechat::build_moderation_tombstone_render_event(
        "m3", "gone", "xmpp_moderated,notify_none"));
    REQUIRE(harness.line_store.tombstones.size() == 1);
    CHECK(harness.line_store.tombstones.front().target_id == "m3");

    harness.clear();

    harness.apply(weechat::build_retraction_tombstone_render_event(
        "m4", "gone", "xmpp_retracted,notify_none", "bob", "", false));
    REQUIRE(harness.line_store.retractions.size() == 1);
    CHECK(harness.line_store.retractions.front().sender_key == "bob");
}

TEST_CASE("NullUiPort discards all output")
{
    weechat::NullUiPort ui;
    ui.printf("plain");
    ui.printf_error("err");
    ui.printf_info("info");
    ui.printf_network("net");
    ui.printf_date_tags(0, "tag", "dated");
    CHECK(true);
}

TEST_CASE("handler test harness applies RenderEvent to capturing ports")
{
    test_weechat::HandlerTestHarness harness;

    const auto receipt = weechat::build_incoming_receipt_render_event("msg-1", false);
    harness.apply(receipt);
    REQUIRE(harness.line_store.glyph_updates.size() == 1);
    CHECK(harness.line_store.glyph_updates.front().acked_id == "msg-1");
    CHECK(harness.line_store.glyph_updates.front().glyph == weechat::k_glyph_delivered);
    CHECK(harness.ui.lines.empty());

    harness.clear();

    weechat::RenderEvent mixed;
    mixed.push_back(weechat::PrintAction{weechat::PrintStyle::Error, "upload failed"});
    mixed.push_back(weechat::NicklistRemoveNickAction{"bob"});
    harness.apply(mixed);

    REQUIRE(harness.ui.errors.size() == 1);
    CHECK(harness.ui.errors.front() == "upload failed");
    REQUIRE(harness.buffer.nicklist_removed.size() == 1);
    CHECK(harness.buffer.nicklist_removed.front() == "bob");
}

TEST_CASE("NullUiPort via harness for protocol-only render events")
{
    test_weechat::HandlerTestHarness harness;
    weechat::RenderEvent event;
    event.push_back(weechat::PrintAction{weechat::PrintStyle::Network, "connected"});

    test_weechat::apply_render_event_to(
        harness.null_ui, harness.buffer, harness.line_store, event);

    CHECK(harness.ui.lines.empty());
    CHECK(harness.ui.network.empty());
}

TEST_CASE("iq_vcard and iq_bookmarks helpers")
{
    CHECK(xmpp::is_vcard4_pubsub_node("urn:xmpp:vcard4"));
    CHECK_FALSE(xmpp::is_vcard4_pubsub_node("other"));

    CHECK(xmpp::is_bookmark_autojoin_true("true"));
    CHECK(xmpp::is_bookmark_autojoin_true("1"));
    CHECK_FALSE(xmpp::is_bookmark_autojoin_true("false"));

    CHECK(xmpp::is_biboumi_gateway_room("room%irc.server@biboumi"));
    CHECK_FALSE(xmpp::is_biboumi_gateway_room("room@conference.example"));

    CHECK(xmpp::bookmark_enter_command("room@conf", "nick")
        == "/enter room@conf/nick --no-switch");
    CHECK(xmpp::bookmark_enter_command("room@conf", "")
        == "/enter room@conf --no-switch");

    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *vcard = xmpp_stanza_new_from_string(env.ctx,
        "<vCard xmlns='vcard-temp'>"
        "<FN>Alice</FN><NICKNAME>ali</NICKNAME>"
        "<EMAIL><USERID>alice@example</USERID></EMAIL>"
        "<ADR><STREET>1 Main</STREET><CTRY>SE</CTRY></ADR>"
        "</vCard>");
    REQUIRE(vcard != nullptr);

    auto fields = xmpp::vcard_fields_from_stanza(xmpp::StanzaView(vcard));
    REQUIRE(fields.fn.has_value());
    CHECK(*fields.fn == "Alice");
    REQUIRE(fields.nickname.has_value());
    CHECK(*fields.nickname == "ali");
    REQUIRE(fields.email.has_value());
    CHECK(*fields.email == "alice@example");

    CHECK(xmpp::apply_vcard_set_field_override(fields, "nickname", "alice2"));
    REQUIRE(fields.nickname.has_value());
    CHECK(*fields.nickname == "alice2");

    const xmpp::StanzaView adr = xmpp::StanzaView(vcard).child("ADR");
    CHECK(xmpp::format_vcard_temp_adr(adr) == "1 Main, SE");

    xmpp_stanza_release(vcard);
}

TEST_CASE("iq_disco and iq_caps helpers")
{
    CHECK(xmpp::is_adhoc_commands_disco_node("http://jabber.org/protocol/commands"));
    CHECK_FALSE(xmpp::is_adhoc_commands_disco_node("other"));

    CHECK(xmpp::is_channel_search_item_open("true"));
    CHECK(xmpp::is_channel_search_item_open(""));
    CHECK_FALSE(xmpp::is_channel_search_item_open("false"));

    CHECK(xmpp::normalize_channel_search_service_type("xep-0045") == "muc");
    CHECK(xmpp::normalize_channel_search_service_type("xep-0369") == "mix");
    CHECK(xmpp::normalize_channel_search_service_type("custom") == "custom");

    CHECK(xmpp::join_bracketed_meta({"3 users", "open"}) == "[3 users, open]");
    CHECK(xmpp::join_bracketed_meta({}).empty());

    CHECK(xmpp::caps_requested_node_ok("", "hash"));
    CHECK(xmpp::caps_requested_node_ok("http://weechat.org", "hash"));
    CHECK(xmpp::caps_requested_node_ok("http://weechat.org#abc", "abc"));
    CHECK_FALSE(xmpp::caps_requested_node_ok("http://weechat.org#abc", "def"));

    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *query = xmpp_stanza_new_from_string(env.ctx,
        "<query xmlns='http://jabber.org/protocol/disco#info'>"
        "<identity category='client' type='pc'/>"
        "<feature var='feature1'/>"
        "<feature var='feature2'/>"
        "</query>");
    REQUIRE(query != nullptr);

    const auto features = xmpp::disco_feature_vars(xmpp::StanzaView(query));
    REQUIRE(features.size() == 2);
    CHECK(features[0] == "feature1");
    CHECK(features[1] == "feature2");

    const std::string s = xmpp::build_caps_verification_string(
        xmpp::StanzaView(query), features);
    CHECK(s == "client/pc//<feature1<feature2<");
    CHECK_FALSE(xmpp::caps_sha1_base64(s).empty());

    xmpp_stanza_release(query);
}

TEST_CASE("iq_mam helpers")
{
    CHECK(xmpp::is_mam_fin_bool_attr_true("true"));
    CHECK(xmpp::is_mam_fin_bool_attr_true("1"));
    CHECK(xmpp::is_mam_fin_bool_attr_true("TRUE"));
    CHECK_FALSE(xmpp::is_mam_fin_bool_attr_true("false"));
    CHECK_FALSE(xmpp::is_mam_fin_bool_attr_true(""));

    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *fin = xmpp_stanza_new_from_string(env.ctx,
        "<fin xmlns='urn:xmpp:mam:2' complete='true'>"
        "<set xmlns='http://jabber.org/protocol/rsm'>"
        "<last>cursor-abc</last>"
        "</set>"
        "</fin>");
    REQUIRE(fin != nullptr);
    CHECK(xmpp::mam_fin_rsm_last(xmpp::StanzaView(fin)) == "cursor-abc");
    xmpp_stanza_release(fin);

    xmpp_stanza_t *err_iq = xmpp_stanza_new_from_string(env.ctx,
        "<iq type='error' xmlns='jabber:client'>"
        "<error type='cancel'>"
        "<item-not-found xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
        "</error>"
        "</iq>");
    REQUIRE(err_iq != nullptr);
    CHECK(xmpp::iq_has_item_not_found_error(xmpp::StanzaView(err_iq)));
    xmpp_stanza_release(err_iq);
}

TEST_CASE("iq_upload helpers")
{
    CHECK(xmpp::is_allowed_http_upload_put_header("Authorization"));
    CHECK(xmpp::is_allowed_http_upload_put_header("cookie"));
    CHECK_FALSE(xmpp::is_allowed_http_upload_put_header("X-Custom"));

    CHECK(xmpp::sanitize_http_header_value("token\r\ninjected") == "tokeninjected");

    CHECK(xmpp::content_type_from_upload_filename("photo.JPG") == "image/jpeg");
    CHECK(xmpp::content_type_from_upload_filename("data.bin") == "application/octet-stream");

    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *err = xmpp_stanza_new_from_string(env.ctx,
        "<error type='cancel' xmlns='jabber:client'>"
        "<not-allowed xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
        "</error>");
    REQUIRE(err != nullptr);
    CHECK(xmpp::format_upload_slot_error_message(xmpp::StanzaView(err))
          == "Upload slot request failed: not-allowed");
    xmpp_stanza_release(err);
}

TEST_CASE("iq_pubsub and omemo pubsub helpers")
{
    CHECK(xmpp::is_skipped_non_atom_feed_item_id("urn:xmpp:avatar:metadata"));
    CHECK_FALSE(xmpp::is_skipped_non_atom_feed_item_id("post-uuid"));

    CHECK(xmpp::is_microblog_comments_node("urn:xmpp:microblog:0:comments/abc"));
    CHECK_FALSE(xmpp::is_microblog_comments_node("urn:xmpp:microblog:0"));

    CHECK(xmpp::is_pubsub_component_jid("news.movim.eu"));
    CHECK(xmpp::is_pubsub_component_jid("feed@ussr.win"));
    CHECK(xmpp::is_pubsub_component_jid("pubsub.example.org"));
    CHECK_FALSE(xmpp::is_pubsub_component_jid("alice@example.org"));

    CHECK(xmpp::should_default_pep_microblog_node("alice@example.org"));
    CHECK_FALSE(xmpp::should_default_pep_microblog_node("feed@ussr.win"));
    CHECK_FALSE(xmpp::should_default_pep_microblog_node("news.movim.eu"));

    CHECK(xmpp::is_legacy_devicelist_pubsub_node(
              "eu.siacs.conversations.axolotl.devicelist"));
    CHECK(xmpp::is_legacy_bundle_pubsub_node(
              "eu.siacs.conversations.axolotl.bundles:42"));

    CHECK(xmpp::omemo_precondition_retry_node_from_publish_id("announce-legacy1", 7)
          == "eu.siacs.conversations.axolotl.devicelist");
    CHECK(xmpp::omemo_precondition_retry_node_from_publish_id("omemo-legacy-bundle", 9)
          == "eu.siacs.conversations.axolotl.bundles:9");

    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *dl_err = xmpp_stanza_new_from_string(env.ctx,
        "<iq xmlns='jabber:client' type='error' id='dl1'>"
        "<error type='cancel'>"
        "<item-not-found xmlns='urn:ietf:params:xml:ns:xmpp-stanzas'/>"
        "</error>"
        "<pubsub xmlns='http://jabber.org/protocol/pubsub'>"
        "<items node='eu.siacs.conversations.axolotl.devicelist'/>"
        "</pubsub>"
        "</iq>");
    REQUIRE(dl_err != nullptr);
    CHECK(xmpp::iq_error_has_item_not_found(xmpp::StanzaView(dl_err)));
    CHECK(xmpp::iq_has_legacy_devicelist_pubsub_error(xmpp::StanzaView(dl_err)));
    xmpp_stanza_release(dl_err);
}

TEST_CASE("message_pep and feed helpers")
{
    CHECK(xmpp::pep_node_is_microblog("urn:xmpp:microblog:0"));
    CHECK(xmpp::pep_node_is_microblog("urn:xmpp:microblog:0:comments/abc"));
    CHECK_FALSE(xmpp::pep_node_is_microblog("news.example/feed"));

    CHECK(xmpp::pep_node_is_protocol_uri("urn:xmpp:avatar:metadata"));
    CHECK(xmpp::pep_node_is_protocol_uri("eu.siacs.conversations.axolotl.devicelist"));
    CHECK_FALSE(xmpp::pep_node_is_protocol_uri("Phoronix"));

    CHECK(xmpp::pep_from_is_self("Alice@Example.org/resource", "alice@example.org"));
    CHECK_FALSE(xmpp::pep_from_is_self("bob@example.org", "alice@example.org"));

    CHECK(xmpp::pep_node_is_legacy_omemo("eu.siacs.conversations.axolotl.bundles:1"));
    CHECK(xmpp::pep_node_is_known_protocol_node("urn:xmpp:bookmarks:1"));

    const auto generic = xmpp::classify_generic_pubsub_feed(
        "Phoronix", "news.movim.eu", "alice@example.org");
    CHECK(generic.is_generic_feed);
    CHECK_FALSE(generic.drop_legacy_omemo);

    const auto self_pep = xmpp::classify_generic_pubsub_feed(
        "mood", "alice@example.org", "alice@example.org");
    CHECK_FALSE(self_pep.is_generic_feed);

    const auto legacy = xmpp::classify_generic_pubsub_feed(
        "eu.siacs.conversations.axolotl.devicelist",
        "alice@example.org",
        "alice@example.org");
    CHECK(legacy.drop_legacy_omemo);
    CHECK_FALSE(legacy.is_generic_feed);

    CHECK(xmpp::feed_alias_prefix(3) == "#3");
    CHECK(xmpp::feed_alias_prefix(0).empty());

    CHECK(xmpp::feed_node_display_label("pubsub.hackerheaven.org", "Phoronix") == "Phoronix");
    CHECK(xmpp::feed_node_display_label("alice@example.org", "urn:xmpp:microblog:0") == "alice");
    CHECK(xmpp::feed_node_display_label(
              "pubsub.hackerheaven.org",
              "urn:xmpp:microblog:0:comments/58118fb5-5321-4eb5-9194-d5ea36c3488c")
          == "comments");

    CHECK(xmpp::feed_parent_display_label("pubsub.hackerheaven.org", "Phoronix") == "Phoronix");
    CHECK(xmpp::feed_parent_display_label("alice@example.org", "urn:xmpp:microblog:0")
          == "alice (blog)");

    CHECK(xmpp::feed_buffer_short_name("pubsub.hackerheaven.org/Phoronix") == "=Phoronix");
    CHECK(xmpp::feed_buffer_short_name("alice@example.org/urn:xmpp:microblog:0")
          == "=alice (blog)");
    CHECK(xmpp::feed_buffer_short_name(
              "pubsub.hackerheaven.org/urn:xmpp:microblog:0:comments/abc-uuid")
          == "=pubsub.hackerheaven.org (comments)");

    CHECK(xmpp::feed_comments_buffer_short_name("Phoronix", 1) == "=Phoronix (#1 comments)");
    CHECK(xmpp::feed_comments_buffer_short_name("Phoronix", -1) == "=Phoronix (comments)");
    CHECK(xmpp::feed_comments_buffer_short_name("alice (blog)", 3) == "=alice (blog) (#3 comments)");

    CHECK(xmpp::feed_item_xmpp_link("news.movim.eu", "Phoronix", "item-1")
          == "xmpp:news.movim.eu?;node=Phoronix;item=item-1");

    const auto alias_lookup = [](std::string_view uuid) -> int {
        return uuid == "uuid-42" ? 7 : -1;
    };
    CHECK(xmpp::feed_reply_label(
              "xmpp:news.movim.eu?;node=Phoronix;item=uuid-42",
              alias_lookup)
          == "#7");
    CHECK(xmpp::feed_reply_label(
              "xmpp:news.movim.eu?;node=Phoronix;item=unknown",
              alias_lookup)
          == "unknown");
    CHECK(xmpp::feed_reply_label("plain-ref", alias_lookup) == "plain-ref");
}

TEST_CASE("format_inbound_message_body respects unstyled hint")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *msg = xmpp_stanza_new_from_string(env.ctx,
        "<message xmlns='jabber:client' type='chat'>"
        "<unstyled xmlns='urn:xmpp:styling:0'/>"
        "<body>*bold*</body>"
        "</message>");
    REQUIRE(msg != nullptr);

    CHECK(xmpp::stanza_has_unstyled_hint(xmpp::StanzaView(msg)));
    CHECK(xmpp::format_inbound_message_body(msg, "*bold*") == "*bold*");

    xmpp_stanza_release(msg);
}

TEST_CASE("parse_carbon_inner_message unwraps forwarded stanza")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *msg = xmpp_stanza_new_from_string(env.ctx,
        "<message from='alice@example.org' to='alice@example.org/phone'>"
        "<received xmlns='urn:xmpp:carbons:2'>"
        "<forwarded xmlns='urn:xmpp:forward:0'>"
        "<message xmlns='jabber:client' from='bob@example.org' to='alice@example.org' type='chat'>"
        "<body>hi</body>"
        "</message>"
        "</forwarded>"
        "</received>"
        "</message>");
    REQUIRE(msg != nullptr);

    const auto view = xmpp::StanzaView(msg);
    CHECK(xmpp::stanza_is_carbon(view));

    auto inner = xmpp::parse_carbon_inner_message(view, "alice@example.org");
    REQUIRE(inner.has_value());
    CHECK(xmpp::stanza_has_user_message_payload(xmpp::StanzaView(*inner)));

    CHECK_FALSE(xmpp::parse_carbon_inner_message(view, "eve@example.org").has_value());

    xmpp_stanza_release(msg);
}

TEST_CASE("bare_jid_iequals and conversation_channel_jid are case-insensitive")
{
    CHECK(xmpp::bare_jid_iequals("Alice@Example.org", "alice@example.org"));
    CHECK_FALSE(xmpp::bare_jid_iequals("bob@example.org", "alice@example.org"));

    const auto sent_carbon_channel = xmpp::conversation_channel_jid(
        "Alice@Example.org", "bob@example.org", "alice@example.org");
    REQUIRE(sent_carbon_channel.has_value());
    CHECK(*sent_carbon_channel == "bob@example.org");

    const auto inbound_channel = xmpp::conversation_channel_jid(
        "Bob@Example.org", "alice@example.org", "alice@example.org");
    REQUIRE(inbound_channel.has_value());
    CHECK(*inbound_channel == "Bob@Example.org");
}

TEST_CASE("conversation_channel_jid_from_message routes sent carbon payloads")
{
    const auto sent_receipt = xmpp::conversation_channel_jid_from_message(
        "alice@example.org/phone", "bob@example.org", "alice@example.org");
    REQUIRE(sent_receipt.has_value());
    CHECK(*sent_receipt == "bob@example.org");

    const auto sent_without_from = xmpp::conversation_channel_jid_from_message(
        "", "bob@example.org", "alice@example.org");
    REQUIRE(sent_without_from.has_value());
    CHECK(*sent_without_from == "bob@example.org");

    const auto inbound = xmpp::conversation_channel_jid_from_message(
        "bob@example.org/mobile", "alice@example.org/desktop", "alice@example.org");
    REQUIRE(inbound.has_value());
    CHECK(*inbound == "bob@example.org");
}

TEST_CASE("parse_carbon_inner_message accepts mixed-case envelope from")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *msg = xmpp_stanza_new_from_string(env.ctx,
        "<message from='Alice@Example.org' to='alice@example.org/phone'>"
        "<received xmlns='urn:xmpp:carbons:2'>"
        "<forwarded xmlns='urn:xmpp:forward:0'>"
        "<message xmlns='jabber:client' from='bob@example.org' to='alice@example.org' type='chat'>"
        "<body>hi</body>"
        "</message>"
        "</forwarded>"
        "</received>"
        "</message>");
    REQUIRE(msg != nullptr);

    const auto view = xmpp::StanzaView(msg);
    auto inner = xmpp::parse_carbon_inner_message(view, "alice@example.org");
    REQUIRE(inner.has_value());
    CHECK(xmpp::stanza_has_user_message_payload(xmpp::StanzaView(*inner)));

    xmpp_stanza_release(msg);
}

TEST_CASE("parse_carbon_inner_message unwraps sent carbon")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *msg = xmpp_stanza_new_from_string(env.ctx,
        "<message from='alice@example.org' to='alice@example.org/desktop'>"
        "<sent xmlns='urn:xmpp:carbons:2'>"
        "<forwarded xmlns='urn:xmpp:forward:0'>"
        "<message xmlns='jabber:client' from='alice@example.org/phone' "
        "to='bob@example.org' type='chat'>"
        "<body>hello from phone</body>"
        "</message>"
        "</forwarded>"
        "</sent>"
        "</message>");
    REQUIRE(msg != nullptr);

    const auto view = xmpp::StanzaView(msg);
    CHECK(xmpp::stanza_is_carbon(view));

    auto inner = xmpp::parse_carbon_inner_message(view, "alice@example.org");
    REQUIRE(inner.has_value());
    CHECK(xmpp::stanza_has_user_message_payload(xmpp::StanzaView(*inner)));

    xmpp_stanza_release(msg);
}

TEST_CASE("parse_carbon_inner_message unwraps sent carbon without inner from")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *msg = xmpp_stanza_new_from_string(env.ctx,
        "<message from='alice@example.org' to='alice@example.org/desktop'>"
        "<sent xmlns='urn:xmpp:carbons:2'>"
        "<forwarded xmlns='urn:xmpp:forward:0'>"
        "<message xmlns='jabber:client' to='bob@example.org' type='chat'>"
        "<body>hello from phone</body>"
        "</message>"
        "</forwarded>"
        "</sent>"
        "</message>");
    REQUIRE(msg != nullptr);

    const auto view = xmpp::StanzaView(msg);
    auto inner = xmpp::parse_carbon_inner_message(view, "alice@example.org");
    REQUIRE(inner.has_value());
    CHECK(xmpp::stanza_has_user_message_payload(xmpp::StanzaView(*inner)));

    xmpp_stanza_release(msg);
}

TEST_CASE("parse_mam_forwarded_dispatch extracts archive metadata")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *msg = xmpp_stanza_new_from_string(env.ctx,
        "<message from='alice@example.org' to='alice@example.org/phone'>"
        "<result xmlns='urn:xmpp:mam:2' id='arch-1' queryid='q1'>"
        "<forwarded xmlns='urn:xmpp:forward:0'>"
        "<delay xmlns='urn:xmpp:delay' stamp='2020-01-01T12:00:00Z'/>"
        "<message xmlns='jabber:client' from='bob@example.org' to='alice@example.org'"
        " type='chat' id='m1'><body>hello</body></message>"
        "</forwarded>"
        "</result>"
        "</message>");
    REQUIRE(msg != nullptr);

    const auto view = xmpp::StanzaView(msg);
    CHECK(xmpp::stanza_is_mam_result(view));
    REQUIRE(xmpp::mam_pubsub_query_id(view) == "q1");

    auto dispatch = xmpp::parse_mam_forwarded_dispatch(view);
    REQUIRE(dispatch.has_value());
    CHECK(dispatch->archive_id == "arch-1");
    CHECK(dispatch->delay_stamp == "2020-01-01T12:00:00Z");
    CHECK(xmpp::parse_forward_delay_stamp(dispatch->delay_stamp) > 0);

    const auto partner = xmpp::mam_conversation_partner_jid(
        "bob@example.org", "alice@example.org", "alice@example.org");
    REQUIRE(partner.has_value());
    CHECK(*partner == "bob@example.org");

    const auto needles = xmpp::mam_dedup_needles("arch-1", "m1");
    CHECK(needles.stanza_id_needle == "stanza_id_arch-1");
    CHECK(needles.message_id_needle == "id_m1");

    xmpp::MamPmDiscoveryPolicy policy;
    policy.has_partner_jid = true;
    policy.has_user_payload = true;
    CHECK(xmpp::should_discover_pm_channel_from_mam(policy));

    xmpp_stanza_release(msg);
}

TEST_CASE("parse_incoming_chat_state from StanzaView")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *msg = xmpp_stanza_new_from_string(env.ctx,
        "<message from='bob@example.org/phone' type='chat'>"
        "<composing xmlns='http://jabber.org/protocol/chatstates'/>"
        "</message>");
    REQUIRE(msg != nullptr);

    const auto view = xmpp::StanzaView(msg);
    CHECK(xmpp::stanza_has_chat_state(view));

    auto state = xmpp::parse_incoming_chat_state(view);
    REQUIRE(state.has_value());
    CHECK(state->from == "bob@example.org/phone");
    CHECK(state->state == xmpp::ChatStateKind::composing);
    CHECK(xmpp::typing_action_for_state(state->state) == xmpp::TypingAction::show);

    xmpp_stanza_release(msg);
}

TEST_CASE("chat state priority prefers composing over paused")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *msg = xmpp_stanza_new_from_string(env.ctx,
        "<message from='bob@example.org/phone' type='chat'>"
        "<composing xmlns='http://jabber.org/protocol/chatstates'/>"
        "<paused xmlns='http://jabber.org/protocol/chatstates'/>"
        "</message>");
    REQUIRE(msg != nullptr);

    auto state = xmpp::parse_incoming_chat_state(xmpp::StanzaView(msg));
    REQUIRE(state.has_value());
    CHECK(state->state == xmpp::ChatStateKind::composing);

    xmpp_stanza_release(msg);
}

TEST_CASE("typing_action_for_state maps paused and gone to clear")
{
    CHECK(xmpp::typing_action_for_state(xmpp::ChatStateKind::paused) == xmpp::TypingAction::clear);
    CHECK(xmpp::typing_action_for_state(xmpp::ChatStateKind::gone) == xmpp::TypingAction::clear);
    CHECK(xmpp::should_clear_typing_on_message(false, false));
    CHECK_FALSE(xmpp::should_clear_typing_on_message(true, false));
    CHECK_FALSE(xmpp::should_clear_typing_on_message(false, true));
}

TEST_CASE("parse_incoming_receipt from StanzaView")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *msg = xmpp_stanza_new_from_string(env.ctx,
        "<message from='bob@example.org/phone' type='chat'>"
        "<received xmlns='urn:xmpp:receipts' id='msg-42'/>"
        "</message>");
    REQUIRE(msg != nullptr);

    const auto view = xmpp::StanzaView(msg);
    CHECK(xmpp::stanza_is_receipt_ack(view));
    CHECK_FALSE(xmpp::stanza_is_displayed_ack(view));

    auto ack = xmpp::parse_incoming_receipt(view);
    REQUIRE(ack.has_value());
    CHECK(ack->from == "bob@example.org/phone");
    CHECK(ack->acked_id == "msg-42");

    xmpp_stanza_release(msg);
}

TEST_CASE("parse_incoming_displayed from StanzaView")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *msg = xmpp_stanza_new_from_string(env.ctx,
        "<message from='bob@example.org/phone' type='chat'>"
        "<displayed xmlns='urn:xmpp:chat-markers:0' id='orig-7'/>"
        "</message>");
    REQUIRE(msg != nullptr);

    const auto view = xmpp::StanzaView(msg);
    CHECK(xmpp::stanza_is_displayed_ack(view));
    CHECK_FALSE(xmpp::stanza_is_receipt_ack(view));

    auto ack = xmpp::parse_incoming_displayed(view);
    REQUIRE(ack.has_value());
    CHECK(ack->from == "bob@example.org/phone");
    CHECK(ack->acked_id == "orig-7");

    xmpp_stanza_release(msg);
}

TEST_CASE("build_ack_reply respects suppress policy")
{
    xmpp::AckReplyInput input;
    input.message_id = "m1";
    input.reply_to = "bob@example.org/phone";
    input.message_type = "chat";
    input.receipt_requested = true;
    input.marker_markable = true;

    xmpp::AckReplySuppress suppress;
    CHECK(xmpp::build_ack_reply(input, suppress).has_value());

    suppress.muc_channel = true;
    CHECK_FALSE(xmpp::build_ack_reply(input, suppress).has_value());

    suppress.muc_channel = false;
    suppress.mam_replay = true;
    CHECK_FALSE(xmpp::build_ack_reply(input, suppress).has_value());

    suppress.mam_replay = false;
    suppress.delayed_delivery = true;
    CHECK_FALSE(xmpp::build_ack_reply(input, suppress).has_value());
}

TEST_CASE("build_ack_reply builds receipt and displayed children")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp::AckReplyInput input;
    input.message_id = "m1";
    input.reply_to = "bob@example.org/phone";
    input.message_type = "chat";
    input.receipt_requested = true;
    input.marker_markable = true;
    input.thread = "thr-1";

    auto built = xmpp::build_ack_reply(input, {});
    REQUIRE(built.has_value());

    auto stanza_sp = built->reply.build(env.ctx);
    const std::string xml = stanza_to_xml(env.ctx, stanza_sp.get());

    CHECK(xml.find("to=\"bob@example.org/phone\"") != std::string::npos);
    CHECK(xml.find("type=\"chat\"") != std::string::npos);
    CHECK(xml.find("urn:xmpp:receipts") != std::string::npos);
    CHECK(xml.find("urn:xmpp:chat-markers:0") != std::string::npos);
    CHECK(xml.find("no-store") != std::string::npos);
    CHECK(built->unread.id == "m1");
    CHECK(built->unread.thread == "thr-1");
}

TEST_CASE("strip_status_glyph_suffix removes trailing delivery glyphs")
{
    CHECK(weechat::strip_status_glyph_suffix("hello") == "hello");
    CHECK(weechat::strip_status_glyph_suffix("hello ✓") == "hello");
    CHECK(weechat::strip_status_glyph_suffix("hello ✓✓") == "hello");
    CHECK(weechat::strip_status_glyph_suffix("hello ⌛") == "hello");
    CHECK(weechat::strip_status_glyph_suffix("hello ✓") + " ✓✓" == "hello ✓✓");
}

TEST_CASE("XEP-0184 receipt request builder")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    stanza::message msg;
    msg.id("msg-1").to("bob@example.org").type("chat").receipt_request();

    auto built = msg.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(root != nullptr);

    xmpp_stanza_t *req = find_child_ns(root, "request", "urn:xmpp:receipts");
    CHECK(req != nullptr);

    xmpp_stanza_release(root);
}

TEST_CASE("XEP-0184 receipt received builder")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    stanza::message msg;
    msg.id("ack-1").to("alice@example.org").type("chat").receipt_received("msg-1");

    auto built = msg.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(root != nullptr);

    xmpp_stanza_t *rcvd = find_child_ns(root, "received", "urn:xmpp:receipts");
    REQUIRE(rcvd != nullptr);
    auto id_attr = attr_opt(rcvd, "id");
    REQUIRE(id_attr.has_value());
    CHECK(*id_attr == "msg-1");

    xmpp_stanza_release(root);
}

TEST_CASE("XEP-0333 store hint builder")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    stanza::message msg;
    msg.id("s1").to("bob@example.org").store();

    auto built = msg.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(root != nullptr);
    CHECK(find_child_ns(root, "store", "urn:xmpp:hints") != nullptr);
    xmpp_stanza_release(root);
}

TEST_CASE("XEP-0333 no-store and no-copy hints builder")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    stanza::message msg;
    msg.id("ns1").to("bob@example.org").no_store().no_copy();

    auto built = msg.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(root != nullptr);
    CHECK(find_child_ns(root, "no-store", "urn:xmpp:hints") != nullptr);
    CHECK(find_child_ns(root, "no-copy", "urn:xmpp:hints") != nullptr);
    xmpp_stanza_release(root);
}

TEST_CASE("XEP-0333 chat marker markable builder")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    stanza::message msg;
    msg.id("m1").to("bob@example.org").chat_marker_markable();

    auto built = msg.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(root != nullptr);
    CHECK(find_child_ns(root, "markable", "urn:xmpp:chat-markers:0") != nullptr);
    xmpp_stanza_release(root);
}

TEST_CASE("XEP-0333 chat marker displayed builder")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    stanza::message msg;
    msg.id("d1").to("bob@example.org").chat_marker_displayed("orig-id-42");

    auto built = msg.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(root != nullptr);

    xmpp_stanza_t *disp = find_child_ns(root, "displayed", "urn:xmpp:chat-markers:0");
    REQUIRE(disp != nullptr);
    auto id_attr = attr_opt(disp, "id");
    REQUIRE(id_attr.has_value());
    CHECK(*id_attr == "orig-id-42");

    xmpp_stanza_release(root);
}

TEST_CASE("XEP-0359 origin-id builder")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    stanza::message msg;
    msg.id("o1").to("bob@example.org").origin_id("uuid-1234");

    auto built = msg.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(root != nullptr);

    xmpp_stanza_t *oid = find_child_ns(root, "origin-id", "urn:xmpp:sid:0");
    REQUIRE(oid != nullptr);
    auto id_attr = attr_opt(oid, "id");
    REQUIRE(id_attr.has_value());
    CHECK(*id_attr == "uuid-1234");

    xmpp_stanza_release(root);
}

TEST_CASE("XEP-0277 Atom entry builder")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    stanza::xep0277::entry entry;
    entry.title_text("Hello world")
        .atom_id("tag:example.org,2026-06-08;posts/abc")
        .published("2026-06-08T12:00:00Z")
        .updated("2026-06-08T12:00:00Z")
        .author("alice@example.org", "xmpp:alice@example.org")
        .content_text("Body text")
        .link("replies", "xmpp:alice@example.org?;node=urn:xmpp:microblog:0:comments/abc",
              "comments")
        .generator("Xepher", "https://github.com/ekollof/xepher", "1.0.0");

    auto built = entry.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    CHECK(xml.find("xmlns=\"http://www.w3.org/2005/Atom\"") != std::string::npos);
    CHECK(xml.find("<title type=\"text\">Hello world</title>") != std::string::npos);
    CHECK(xml.find("<content type=\"text\">Body text</content>") != std::string::npos);
    CHECK(xml.find("rel=\"replies\"") != std::string::npos);
    CHECK(xml.find("<generator uri=\"https://github.com/ekollof/xepher\"") != std::string::npos);
}

TEST_CASE("XEP-0511 rdf:Description link metadata builder")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    stanza::xep0511::rdf_description rdf("https://example.org/page");
    rdf.og("og:title", "Example Page")
       .og("og:url", "https://example.org/page")
       .og("og:description", "");

    auto built = rdf.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    CHECK(xml.find("rdf:Description") != std::string::npos);
    CHECK(xml.find("xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"") != std::string::npos);
    CHECK(xml.find("xmlns:og=\"https://ogp.me/ns#\"") != std::string::npos);
    CHECK(xml.find("rdf:about=\"https://example.org/page\"") != std::string::npos);
    CHECK(xml.find("og:title") != std::string::npos);
    CHECK(xml.find("Example Page") != std::string::npos);
    CHECK(xml.find("og:description") == std::string::npos);
}

TEST_CASE("XEP-0085 chatstate active builder")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    stanza::message msg;
    msg.id("c1").to("bob@example.org").chatstate("active");

    auto built = msg.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(root != nullptr);

    xmpp_stanza_t *cs = find_child_ns(root, "active", "http://jabber.org/protocol/chatstates");
    CHECK(cs != nullptr);

    xmpp_stanza_release(root);
}

TEST_CASE("XEP-0085 chatstate composing builder")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    stanza::message msg;
    msg.id("c2").to("bob@example.org").chatstate("composing");

    auto built = msg.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(root != nullptr);
    CHECK(find_child_ns(root, "composing", "http://jabber.org/protocol/chatstates") != nullptr);
    xmpp_stanza_release(root);
}

TEST_CASE("XEP-0428 fallback builder")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    stanza::message msg;
    msg.id("f1").to("bob@example.org").fallback(stanza::xep0428::fallback("urn:xmpp:sfs:0", 12));

    auto built = msg.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(root != nullptr);

    xmpp_stanza_t *fb = find_child_ns(root, "fallback", "urn:xmpp:fallback:0");
    REQUIRE(fb != nullptr);
    auto for_attr = attr_opt(fb, "for");
    REQUIRE(for_attr.has_value());
    CHECK(*for_attr == "urn:xmpp:sfs:0");

    xmpp_stanza_t *body_range = xmpp_stanza_get_child_by_name(fb, "body");
    REQUIRE(body_range != nullptr);
    auto start_attr = attr_opt(body_range, "start");
    auto end_attr = attr_opt(body_range, "end");
    REQUIRE(start_attr.has_value());
    REQUIRE(end_attr.has_value());
    CHECK(*start_attr == "0");
    CHECK(*end_attr == "12");

    xmpp_stanza_release(root);
}

TEST_CASE("XEP-0428 fallback builder omits body range when end is 0")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    stanza::message msg;
    msg.id("f2").to("bob@example.org").fallback(stanza::xep0428::fallback("urn:xmpp:sfs:0", 0));

    auto built = msg.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(root != nullptr);

    xmpp_stanza_t *fb = find_child_ns(root, "fallback", "urn:xmpp:fallback:0");
    REQUIRE(fb != nullptr);
    CHECK(xmpp_stanza_get_child_by_name(fb, "body") == nullptr);

    xmpp_stanza_release(root);
}

TEST_CASE("XEP-0066 OOB builder")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    stanza::message msg;
    msg.id("oob1").to("bob@example.org").oob(stanza::xep0066::oob("https://example.org/file.png"));

    auto built = msg.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(root != nullptr);

    xmpp_stanza_t *x_el = find_child_ns(root, "x", "jabber:x:oob");
    REQUIRE(x_el != nullptr);

    xmpp_stanza_t *url_el = xmpp_stanza_get_child_by_name(x_el, "url");
    REQUIRE(url_el != nullptr);
    auto url_text = text_opt(url_el);
    REQUIRE(url_text.has_value());
    CHECK(*url_text == "https://example.org/file.png");

    xmpp_stanza_release(root);
}

TEST_CASE("XEP-0382 spoiler builder without hint")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    stanza::message msg;
    msg.id("sp1").to("bob@example.org").spoiler();

    auto built = msg.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(root != nullptr);

    xmpp_stanza_t *sp = find_child_ns(root, "spoiler", "urn:xmpp:spoiler:0");
    REQUIRE(sp != nullptr);
    CHECK(text_opt(sp) == std::nullopt);

    xmpp_stanza_release(root);
}

TEST_CASE("XEP-0382 spoiler builder with hint")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    stanza::message msg;
    msg.id("sp2").to("bob@example.org").spoiler("Movie plot");

    auto built = msg.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(root != nullptr);

    xmpp_stanza_t *sp = find_child_ns(root, "spoiler", "urn:xmpp:spoiler:0");
    REQUIRE(sp != nullptr);
    auto hint = text_opt(sp);
    REQUIRE(hint.has_value());
    CHECK(*hint == "Movie plot");

    xmpp_stanza_release(root);
}

TEST_CASE("XEP-0249 direct MUC invitation builder")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    stanza::message msg;
    msg.id("inv1").to("bob@example.org").invite("room@conference.example.org", "secret123", "Join us!");

    auto built = msg.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(root != nullptr);

    xmpp_stanza_t *x_el = find_child_ns(root, "x", "jabber:x:conference");
    REQUIRE(x_el != nullptr);
    auto jid_attr = attr_opt(x_el, "jid");
    auto pw_attr = attr_opt(x_el, "password");
    auto reason_attr = attr_opt(x_el, "reason");
    REQUIRE(jid_attr.has_value());
    CHECK(*jid_attr == "room@conference.example.org");
    REQUIRE(pw_attr.has_value());
    CHECK(*pw_attr == "secret123");
    REQUIRE(reason_attr.has_value());
    CHECK(*reason_attr == "Join us!");

    xmpp_stanza_release(root);
}

// =============================================================================
// TEST CASES – Group E: File-sharing builders (XEP-0447 SFS, XEP-0385 SIMS)
// =============================================================================

TEST_CASE("XEP-0447 plain file-sharing builder")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    stanza::xep0447::file f;
    f.media_type("image/png").name("cat.png").size(12345).hash_sha256("dGVzdA==");

    stanza::xep0447::sources srcs;
    stanza::xep0447::url_data ud("https://example.org/cat.png");
    srcs.add(ud);

    stanza::xep0447::file_sharing fs;
    fs.file(f).sources(srcs);

    stanza::message msg;
    msg.id("fs1").to("bob@example.org").file_sharing(fs);

    auto built = msg.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(root != nullptr);

    xmpp_stanza_t *fs_el = find_child_ns(root, "file-sharing", "urn:xmpp:sfs:0");
    REQUIRE(fs_el != nullptr);

    xmpp_stanza_t *file_el = find_child_ns(fs_el, "file", "urn:xmpp:file:metadata:0");
    REQUIRE(file_el != nullptr);

    xmpp_stanza_t *hash_el = find_child_ns(file_el, "hash", "urn:xmpp:hashes:2");
    REQUIRE(hash_el != nullptr);
    auto algo = attr_opt(hash_el, "algo");
    REQUIRE(algo.has_value());
    CHECK(*algo == "sha-256");

    xmpp_stanza_t *sources_el = xmpp_stanza_get_child_by_name(fs_el, "sources");
    REQUIRE(sources_el != nullptr);

    xmpp_stanza_release(root);
}

TEST_CASE("XEP-0447 encrypted file-sharing (ESFS) builder")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    stanza::xep0447::file f;
    f.media_type("application/octet-stream").name("secret.zip").size(999);

    stanza::xep0447::url_data cipher_ud("https://example.org/secret.zip.bin");
    stanza::xep0447::sources inner_srcs;
    inner_srcs.add(cipher_ud);

    stanza::xep0447::encrypted enc;
    enc.key("QUJDRA==").iv("UVdFUlM=").sources(inner_srcs).cipher_hash_sha256("aGFzaA==");

    stanza::xep0447::sources outer_srcs;
    outer_srcs.add(enc);

    stanza::xep0447::file_sharing fs;
    fs.file(f).sources(outer_srcs);

    stanza::message msg;
    msg.id("fs2").to("bob@example.org").file_sharing(fs);

    auto built = msg.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(root != nullptr);

    xmpp_stanza_t *fs_el = find_child_ns(root, "file-sharing", "urn:xmpp:sfs:0");
    REQUIRE(fs_el != nullptr);

    xmpp_stanza_t *sources_el = xmpp_stanza_get_child_by_name(fs_el, "sources");
    REQUIRE(sources_el != nullptr);

    xmpp_stanza_t *enc_el = find_child_ns(sources_el, "encrypted", "urn:xmpp:esfs:0");
    REQUIRE(enc_el != nullptr);
    auto cipher_attr = attr_opt(enc_el, "cipher");
    REQUIRE(cipher_attr.has_value());
    CHECK(cipher_attr->find("aes-256-gcm") != std::string::npos);

    xmpp_stanza_release(root);
}

TEST_CASE("XEP-0385 SIMS reference builder")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    stanza::xep0385::file f;
    f.media_type("image/jpeg").name("sunset.jpg").size(45678).width(1920).height(1080);

    stanza::xep0385::sources srcs;
    srcs.add_source("https://example.org/sunset.jpg");

    stanza::xep0385::media_sharing ms;
    ms.file(f).sources(srcs);

    stanza::xep0385::reference ref("0", "4");
    ref.uri("https://example.org/sunset.jpg").media_sharing(ms);

    stanza::message msg;
    msg.id("sims1").to("bob@example.org").sims_reference(ref);

    auto built = msg.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(root != nullptr);

    xmpp_stanza_t *ref_el = find_child_ns(root, "reference", "urn:xmpp:reference:0");
    REQUIRE(ref_el != nullptr);
    auto type_attr = attr_opt(ref_el, "type");
    auto begin_attr = attr_opt(ref_el, "begin");
    auto end_attr = attr_opt(ref_el, "end");
    REQUIRE(type_attr.has_value());
    CHECK(*type_attr == "data");
    CHECK(*begin_attr == "0");
    CHECK(*end_attr == "4");

    xmpp_stanza_t *ms_el = find_child_ns(ref_el, "media-sharing", "urn:xmpp:sims:1");
    REQUIRE(ms_el != nullptr);

    xmpp_stanza_release(root);
}

// =============================================================================
// TEST CASES – Group F: IQ builders (XEP-0191 Blocking Command)
// =============================================================================

TEST_CASE("XEP-0191 block IQ builder")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    stanza::iq iq_s;
    iq_s.id("b1").type("set").to("example.org");
    stanza::xep0191::block b;
    b.item("spam@example.org");
    iq_s.block(b);

    auto built = iq_s.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(root != nullptr);

    xmpp_stanza_t *block_el = find_child_ns(root, "block", "urn:xmpp:blocking");
    REQUIRE(block_el != nullptr);

    xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(block_el, "item");
    REQUIRE(item != nullptr);
    auto jid_attr = attr_opt(item, "jid");
    REQUIRE(jid_attr.has_value());
    CHECK(*jid_attr == "spam@example.org");

    xmpp_stanza_release(root);
}

TEST_CASE("XEP-0191 unblock IQ builder")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    stanza::iq iq_s;
    iq_s.id("u1").type("set").to("example.org");
    stanza::xep0191::unblock u;
    u.item("spam@example.org");
    iq_s.unblock(u);

    auto built = iq_s.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(root != nullptr);

    xmpp_stanza_t *unblock_el = find_child_ns(root, "unblock", "urn:xmpp:blocking");
    REQUIRE(unblock_el != nullptr);

    xmpp_stanza_t *item = xmpp_stanza_get_child_by_name(unblock_el, "item");
    REQUIRE(item != nullptr);
    CHECK(attr_opt(item, "jid").value_or("") == "spam@example.org");

    xmpp_stanza_release(root);
}

TEST_CASE("XEP-0191 blocklist IQ builder")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    stanza::iq iq_s;
    iq_s.id("bl1").type("get").to("example.org").blocklist();

    auto built = iq_s.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(root != nullptr);

    xmpp_stanza_t *bl_el = find_child_ns(root, "blocklist", "urn:xmpp:blocking");
    REQUIRE(bl_el != nullptr);

    xmpp_stanza_release(root);
}

// =============================================================================
// TEST CASES – Group G: OMEMO axolotl builders (XEP-0384 legacy)
// =============================================================================

TEST_CASE("XEP-0384 axolotl encrypted envelope builder")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    stanza::xep0384::axolotl_keys keys("alice@example.org");
    keys.add_key(stanza::xep0384::axolotl_key("1111111", "QUJDRA==", true));

    stanza::xep0384::axolotl_header hdr("2222222");
    hdr.add_keys(keys).add_iv(stanza::xep0384::axolotl_iv("UVdFUlM="));

    stanza::xep0384::axolotl_encrypted enc;
    enc.add_header(hdr).add_payload(stanza::xep0384::axolotl_payload("RkFLRV9QTEFJTlRFWFQ="));

    stanza::message msg;
    msg.id("o1").to("alice@example.org").type("chat").omemo_axolotl_encrypted(enc);

    auto built = msg.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(root != nullptr);

    xmpp_stanza_t *enc_el = find_child_ns(root, "encrypted", "eu.siacs.conversations.axolotl");
    REQUIRE(enc_el != nullptr);

    xmpp_stanza_t *header_el = xmpp_stanza_get_child_by_name(enc_el, "header");
    REQUIRE(header_el != nullptr);
    auto sid = attr_opt(header_el, "sid");
    REQUIRE(sid.has_value());
    CHECK(*sid == "2222222");

    xmpp_stanza_t *keys_el = xmpp_stanza_get_child_by_name(header_el, "keys");
    REQUIRE(keys_el != nullptr);
    auto keys_jid = attr_opt(keys_el, "jid");
    REQUIRE(keys_jid.has_value());
    CHECK(*keys_jid == "alice@example.org");

    xmpp_stanza_t *payload_el = xmpp_stanza_get_child_by_name(enc_el, "payload");
    REQUIRE(payload_el != nullptr);
    auto ptext = text_opt(payload_el);
    REQUIRE(ptext.has_value());
    CHECK(*ptext == "RkFLRV9QTEFJTlRFWFQ=");

    xmpp_stanza_release(root);
}

TEST_CASE("XEP-0384 axolotl bundle builder")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    stanza::xep0384::axolotl_bundle bundle;
    bundle.add_spk(stanza::xep0384::axolotl_spk("1", "QUJDRA=="));
    bundle.add_spks(stanza::xep0384::axolotl_spks("Rk9PQkFS"));
    bundle.add_ik(stanza::xep0384::axolotl_ik("SUtLRVk="));

    stanza::xep0384::axolotl_prekeys pks;
    pks.add_pk(stanza::xep0384::axolotl_pk("1001", "UEsx"));
    bundle.add_prekeys(pks);

    auto built = bundle.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(root != nullptr);
    CHECK(std::string(xmpp_stanza_get_name(root)) == "bundle");

    xmpp_stanza_t *spk = xmpp_stanza_get_child_by_name(root, "signedPreKeyPublic");
    REQUIRE(spk != nullptr);
    CHECK(attr_opt(spk, "signedPreKeyId").value_or("") == "1");

    xmpp_stanza_t *spks = xmpp_stanza_get_child_by_name(root, "signedPreKeySignature");
    REQUIRE(spks != nullptr);

    xmpp_stanza_t *ik = xmpp_stanza_get_child_by_name(root, "identityKey");
    REQUIRE(ik != nullptr);

    xmpp_stanza_t *prekeys = xmpp_stanza_get_child_by_name(root, "prekeys");
    REQUIRE(prekeys != nullptr);

    bool found_pk = false;
    for (xmpp_stanza_t *pk = xmpp_stanza_get_children(prekeys); pk; pk = xmpp_stanza_get_next(pk))
    {
        if (std::strcmp(xmpp_stanza_get_name(pk), "preKeyPublic") == 0)
        {
            found_pk = true;
            break;
        }
    }
    CHECK(found_pk);

    xmpp_stanza_release(root);
}

// =============================================================================
// TEST CASES – Group H: Presence builders (XEP-0437 RAI)
// =============================================================================

TEST_CASE("XEP-0437 RAI presence builder")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    stanza::presence pres;
    pres.to("alice@example.org").rai_indicator();

    auto built = pres.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(root != nullptr);

    xmpp_stanza_t *rai_el = find_child_ns(root, "rai", "urn:xmpp:rai:0");
    REQUIRE(rai_el != nullptr);

    xmpp_stanza_release(root);
}

TEST_CASE("XEP-0437 RAI message builder")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    stanza::xep0437::rai r;
    r.add_activity("room1@conference.example.org");
    r.add_activity("room2@conference.example.org");

    stanza::message msg;
    msg.id("rai1").to("alice@example.org").child(r);

    auto built = msg.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(root != nullptr);

    xmpp_stanza_t *rai_el = find_child_ns(root, "rai", "urn:xmpp:rai:0");
    REQUIRE(rai_el != nullptr);

    int activity_count = 0;
    for (xmpp_stanza_t *ch = xmpp_stanza_get_children(rai_el); ch; ch = xmpp_stanza_get_next(ch))
    {
        if (std::strcmp(xmpp_stanza_get_name(ch), "activity") == 0)
            ++activity_count;
    }
    CHECK(activity_count == 2);

    xmpp_stanza_release(root);
}

// =============================================================================
// TEST CASES – Group I: Full message with multiple mixins (integration-style)
// =============================================================================

TEST_CASE("Full message with body + origin-id + receipt-request + chatstate + store")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    stanza::message msg;
    msg.id("full1")
       .to("bob@example.org")
       .type("chat")
       .body("Hello world")
       .origin_id("uuid-full-1")
       .receipt_request()
       .chatstate("active")
       .store();

    auto built = msg.build(env.ctx);
    std::string xml = stanza_to_xml(env.ctx, built.get());

    xmpp_stanza_t *root = xmpp_stanza_new_from_string(env.ctx, xml.c_str());
    REQUIRE(root != nullptr);

    CHECK(attr_opt(root, "id").value_or("") == "full1");
    CHECK(attr_opt(root, "to").value_or("") == "bob@example.org");
    CHECK(attr_opt(root, "type").value_or("") == "chat");

    xmpp_stanza_t *body_el = xmpp_stanza_get_child_by_name(root, "body");
    REQUIRE(body_el != nullptr);
    CHECK(text_opt(body_el).value_or("") == "Hello world");

    CHECK(find_child_ns(root, "origin-id", "urn:xmpp:sid:0") != nullptr);
    CHECK(find_child_ns(root, "request", "urn:xmpp:receipts") != nullptr);
    CHECK(find_child_ns(root, "active", "http://jabber.org/protocol/chatstates") != nullptr);
    CHECK(find_child_ns(root, "store", "urn:xmpp:hints") != nullptr);

    xmpp_stanza_release(root);
}

// =============================================================================
// TEST CASES – Group J: Exported .cpp pure functions (coverage-improving)
// =============================================================================

TEST_CASE("stanza::uuid generates non-empty string")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    std::string u = stanza::uuid(env.ctx);
    CHECK(!u.empty());
    // UUIDs from libstrophe are typically 36 chars with hyphens
    CHECK(u.find('-') != std::string::npos);
}

TEST_CASE("get_time: current implementation throws on valid input (inverted strptime check)")
{
    // NOTE: get_time has a condition bug: strptime returns non-null on success,
    // but the code throws in that branch. This test documents current behavior.
    CHECK_THROWS(get_time("2024-01-15T12:30:00+0100"));
}

TEST_CASE("replace_emoticons converts :-) to emoji")
{
    CHECK(replace_emoticons("Hello :-) world") == "Hello 😊 world");
}

TEST_CASE("replace_emoticons converts multiple emoticons")
{
    CHECK(replace_emoticons(":-) :-( ;-) :D") == "😊 😢 😉 😀");
}

TEST_CASE("replace_emoticons leaves non-emoticon text alone")
{
    CHECK(replace_emoticons("hello world") == "hello world");
}

TEST_CASE("replace_emoticons does not match partial emoticons")
{
    // "a:-)b" should NOT match because ':' is not at a boundary
    CHECK(replace_emoticons("a:-)b") == "a:-)b");
}

TEST_CASE("replace_emoticons converts GitHub shortcodes")
{
    CHECK(replace_emoticons("Nice :thumbsup:") == "Nice 👍");
    CHECK(replace_emoticons(":+1: :smile: :heart:") == "👍 😄 ❤️");
}

TEST_CASE("replace_emoticons leaves unknown shortcodes alone")
{
    CHECK(replace_emoticons(":not_a_real_emoji_shortcode:") == ":not_a_real_emoji_shortcode:");
}

TEST_CASE("replace_emoticons does not match shortcodes without boundaries")
{
    CHECK(replace_emoticons("foo:thumbsup:bar") == "foo:thumbsup:bar");
}

TEST_CASE("resolve_emoji_shortcode converts known aliases")
{
    CHECK(resolve_emoji_shortcode(":thumbsup:") == "👍");
    CHECK(resolve_emoji_shortcode(":+1:") == "👍");
}

TEST_CASE("resolve_emoji_shortcode passes through raw emoji and unknown codes")
{
    CHECK(resolve_emoji_shortcode("👍") == "👍");
    CHECK(resolve_emoji_shortcode(":unknown_xyz:") == ":unknown_xyz:");
}

TEST_CASE("emoji_shortcode_completions filters by prefix")
{
    const auto capped = emoji_shortcode_completions("", 64);
    CHECK(capped.size() == 64);
    CHECK(std::ranges::is_sorted(capped));

    const auto thumb = emoji_shortcode_completions("thumb", 16);
    CHECK(!thumb.empty());
    CHECK(thumb.front().starts_with(":thumb"));
}

TEST_CASE("emoji_shortcode_completion_prefix parses /react and chat input")
{
    CHECK(emoji_shortcode_completion_prefix("/react :thumb") == "thumb");
    CHECK(emoji_shortcode_completion_prefix("/react :thumbsup:") == "thumbsup");
    CHECK(emoji_shortcode_completion_prefix("/react  :heart") == "heart");
    CHECK(emoji_shortcode_completion_prefix("/react") == "");
    CHECK(emoji_shortcode_completion_prefix("/react ") == "");
    CHECK(!emoji_shortcode_completion_prefix("/react 👍"));

    CHECK(emoji_shortcode_completion_prefix("sounds good :thumb") == "thumb");
    CHECK(emoji_shortcode_completion_prefix(":heart") == "heart");
    CHECK(!emoji_shortcode_completion_prefix("hello world"));
    CHECK(!emoji_shortcode_completion_prefix("ratio 3:1"));
}

TEST_CASE("is_image_mime_type accepts image MIME types")
{
    CHECK(is_image_mime_type("image/png") == true);
    CHECK(is_image_mime_type("image/jpeg") == true);
    CHECK(is_image_mime_type("image/gif") == true);
    CHECK(is_image_mime_type("image/webp") == true);
}

TEST_CASE("is_image_mime_type rejects non-image MIME types")
{
    CHECK(is_image_mime_type("application/pdf") == false);
    CHECK(is_image_mime_type("text/plain") == false);
    CHECK(is_image_mime_type("video/mp4") == false);
    CHECK(is_image_mime_type("audio/ogg") == false);
    CHECK(is_image_mime_type("image") == false);
    CHECK(is_image_mime_type("") == false);
}

TEST_CASE("collect_bob_image_refs parses Movim XHTML-IM cid sticker")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *msg = xmpp_stanza_new_from_string(env.ctx,
        "<message xmlns='jabber:client' type='groupchat' "
        "from='room@conference.example/Alice'>"
        "<body>Sticker notification</body>"
        "<html xmlns='http://jabber.org/protocol/xhtml-im'>"
        "<body xmlns='http://www.w3.org/1999/xhtml'>"
        "<p><img alt='Sticker' "
        "src='cid:sha-256+75b3af0d3dd8b6d6edff67cacaf4e2a00d1781802a1d3eb8f5c2464a02442e0b@bob.xmpp.org'/>"
        "</p></body></html>"
        "</message>");
    REQUIRE(msg != nullptr);

    CHECK(xmpp::message_has_xhtml_bob_images(xmpp::StanzaView(msg)));

    const auto refs = xmpp::collect_bob_image_refs(xmpp::StanzaView(msg));
    REQUIRE(refs.size() == 1);
    CHECK(refs[0].cid
        == "sha-256+75b3af0d3dd8b6d6edff67cacaf4e2a00d1781802a1d3eb8f5c2464a02442e0b@bob.xmpp.org");
    CHECK(refs[0].inline_b64.empty());

    xmpp_stanza_release(msg);
}

TEST_CASE("collect_bob_image_refs parses inline XEP-0231 data element")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *msg = xmpp_stanza_new_from_string(env.ctx,
        "<message xmlns='jabber:client' type='chat'>"
        "<data xmlns='urn:xmpp:bob' "
        "cid='sha1+abc@bob.xmpp.org' type='image/png'>aGVsbG8=</data>"
        "</message>");
    REQUIRE(msg != nullptr);

    const auto refs = xmpp::collect_bob_image_refs(xmpp::StanzaView(msg));
    REQUIRE(refs.size() == 1);
    CHECK(refs[0].cid == "sha1+abc@bob.xmpp.org");
    CHECK(refs[0].mime == "image/png");
    CHECK(refs[0].inline_b64 == "aGVsbG8=");

    xmpp_stanza_release(msg);
}

TEST_CASE("bob_make_cid uses sha1+hex@bob.xmpp.org form")
{
    const std::vector<std::uint8_t> data = {'h', 'i'};
    const std::string cid = xmpp::bob_make_cid(data);
    CHECK(cid.starts_with("sha1+"));
    CHECK(cid.ends_with("@bob.xmpp.org"));
    CHECK(cid.size() == 58);
}

TEST_CASE("handle_bob_iq_get returns BoB payload for hosted cid")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *iq = xmpp_stanza_new_from_string(env.ctx,
        "<iq xmlns='jabber:client' type='get' id='bob1' "
        "from='peer@example.com/resource' to='me@example.com'>"
        "<data xmlns='urn:xmpp:bob' "
        "cid='sha1+abc@bob.xmpp.org'/>"
        "</iq>");
    REQUIRE(iq != nullptr);

    const xmpp::BobHostedPayload hosted{
        "image/png",
        {0x89, 0x50, 0x4e, 0x47},
    };

    auto reply = xmpp::handle_bob_iq_get(
        xmpp::StanzaView(iq), "me@example.com", &hosted);
    REQUIRE(reply.has_value());

    auto built = std::move(*reply).build(env.ctx);
    const std::string xml = stanza_to_xml(env.ctx, built.get());
    CHECK(xml.find("type=\"result\"") != std::string::npos);
    CHECK(xml.find("sha1+abc@bob.xmpp.org") != std::string::npos);
    CHECK(xml.find("image/png") != std::string::npos);

    const xmpp::StanzaView result_view(built.get());
    const xmpp::StanzaView data = result_view.child("data", xmpp::k_bob_ns);
    REQUIRE(data.valid());
    CHECK(!data.text().empty());

    xmpp_stanza_release(iq);
}

TEST_CASE("handle_bob_iq_get returns item-not-found when unhosted")
{
    unit_strophe_env env;
    REQUIRE(env.ctx != nullptr);

    xmpp_stanza_t *iq = xmpp_stanza_new_from_string(env.ctx,
        "<iq xmlns='jabber:client' type='get' id='bob2' "
        "from='peer@example.com' to='me@example.com'>"
        "<data xmlns='urn:xmpp:bob' cid='sha1+missing@bob.xmpp.org'/>"
        "</iq>");
    REQUIRE(iq != nullptr);

    auto reply = xmpp::handle_bob_iq_get(
        xmpp::StanzaView(iq), "me@example.com", nullptr);
    REQUIRE(reply.has_value());

    auto built = std::move(*reply).build(env.ctx);
    const std::string xml = stanza_to_xml(env.ctx, built.get());
    CHECK(xml.find("type=\"error\"") != std::string::npos);
    CHECK(xml.find("item-not-found") != std::string::npos);

    xmpp_stanza_release(iq);
}

