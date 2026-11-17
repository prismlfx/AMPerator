#include <catch2/catch_test_macros.hpp>

#include "online/ExternalLinks.h"

using amp::online::isAllowedExternalUrl;

TEST_CASE("online_external_links: the TONE3000 site is allowed", "[online_external_links]")
{
    CHECK(isAllowedExternalUrl("https://www.tone3000.com/"));
    CHECK(isAllowedExternalUrl("https://www.tone3000.com")); // the bare site root
    CHECK(isAllowedExternalUrl("https://www.tone3000.com/tones/123?x=1#y"));
}

TEST_CASE("online_external_links: anything else is refused", "[online_external_links]")
{
    CHECK_FALSE(isAllowedExternalUrl(""));
    CHECK_FALSE(isAllowedExternalUrl("https://www.tone3000.com.evil.example/"));
    CHECK_FALSE(isAllowedExternalUrl("https://www.tone3000.com@evil.example/"));
    CHECK_FALSE(isAllowedExternalUrl("https://www.tone3000.comevil/"));
    CHECK_FALSE(isAllowedExternalUrl("http://www.tone3000.com/"));
    CHECK_FALSE(isAllowedExternalUrl("HTTPS://www.tone3000.com/"));
    CHECK_FALSE(isAllowedExternalUrl("https://WWW.TONE3000.COM/"));
    CHECK_FALSE(isAllowedExternalUrl("https://tone3000.com/"));
    CHECK_FALSE(isAllowedExternalUrl("https://evil.example/https://www.tone3000.com/"));
    CHECK_FALSE(isAllowedExternalUrl(" https://www.tone3000.com/"));
    CHECK_FALSE(isAllowedExternalUrl("file:///etc/passwd"));
}
