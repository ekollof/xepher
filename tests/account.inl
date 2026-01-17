#include <doctest/doctest.h>

#include "../src/account.hh"

TEST_CASE("create account")
{
    weechat::xmpp::account acc("demo");

    CHECK(acc.name == "demo");
}
