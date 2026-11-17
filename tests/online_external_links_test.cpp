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

TEST_CASE("online_external_links: callbackQuery matches only the callback itself", "[online_external_links]")
{
    using amp::online::callbackQuery;
    const juce::String prefix("http://127.0.0.1/callback");

    CHECK(callbackQuery(prefix + "?code=a&state=b", prefix) == juce::String("code=a&state=b"));
    CHECK(callbackQuery(prefix + "?a=b#f", prefix) == juce::String("a=b"));
    CHECK(callbackQuery(prefix, prefix) == juce::String());
    CHECK(callbackQuery(prefix + "#x", prefix) == juce::String());
    CHECK(callbackQuery(prefix + "?", prefix) == juce::String());

    CHECK_FALSE(callbackQuery(prefix + "evil?code=a&state=b", prefix).has_value());
    CHECK_FALSE(callbackQuery(prefix + ".evil.example", prefix).has_value());
    CHECK_FALSE(callbackQuery(prefix + "/", prefix).has_value());
    CHECK_FALSE(callbackQuery("http://127.0.0.1:8080/callback?code=a", prefix).has_value());
    CHECK_FALSE(callbackQuery("https://127.0.0.1/callback?code=a", prefix).has_value());
    CHECK_FALSE(callbackQuery("http://evil.example/callback?code=a", prefix).has_value());
    CHECK_FALSE(callbackQuery(prefix + "?code=a", {}).has_value());
    // a "?" inside the fragment is not a query
    const auto inFragment = callbackQuery(prefix + "#x?code=a&state=b", prefix);
    REQUIRE(inFragment.has_value());
    CHECK(inFragment->isEmpty());
}
