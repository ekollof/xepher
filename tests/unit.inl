// This Source Code Form is subject to the terms of the Mozilla Public
// License, version 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// Unit tests for pure and near-pure plugin functions.
//
// Calls the real symbols from xmpp.cov.so (coverage-instrumented plugin)
// so that gcovr reports non-zero coverage for the exercised source files.
//
// Functions under test:
//   • char_cmp, unescape                   → src/util.cpp
//   • message__htmldecode                  → src/message.cpp
//   • get_name, get_attribute, get_text    → src/xmpp/node.cpp
//   • jid::jid, jid::is_bare              → src/xmpp/node.cpp
//   • weechat::consistent_color            → src/color.cpp
//   • weechat::angle_to_weechat_color      → src/color.cpp

#include <doctest/doctest.h>

// ── plugin headers (real symbols declared here) ───────────────────────────────
#include "../src/util.hh"
#include "../src/message.hh"
#include "../src/color.hh"
#include "../src/strophe.hh"
#include "../src/xmpp/node.hh"

// ── stdlib ────────────────────────────────────────────────────────────────────
#include <cstring>
#include <string>

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
                        xmpp_string_guard text_g(env.ctx, xmpp_stanza_get_text(val));
                        CHECK(text_g.ptr != nullptr);
                        if (text_g.ptr)
                            CHECK(std::string_view(text_g.ptr) == "carol@example.org");
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
            xmpp_string_guard after_text_g(env.ctx, xmpp_stanza_get_text(after_el));
            CHECK(after_text_g.ptr != nullptr);
            if (after_text_g.ptr)
                CHECK(std::string_view(after_text_g.ptr) == "last-seen-uid-xyz");
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
    xmpp_stanza_t *ch = xmpp_stanza_get_children(el);
    while (ch) {
        if (xmpp_stanza_is_text(ch)) {
            char *t = xmpp_stanza_get_text(ch);
            if (t) {
                std::string result(t);
                xmpp_free(xmpp_stanza_get_context(ch), t);
                return result;
            }
        }
        ch = xmpp_stanza_get_next(ch);
    }
    return std::nullopt;
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


