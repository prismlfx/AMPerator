#include <catch2/catch_test_macros.hpp>

#include "online/JuceHttpTransport.h"

// Real network: skipped unless AMP_NETWORK_TESTS=1. Only unauthenticated GETs to www.tone3000.com.

using namespace amp::online;

namespace
{
bool networkTestsEnabled() { return juce::SystemStats::getEnvironmentVariable("AMP_NETWORK_TESTS", "") == "1"; }
} // namespace

TEST_CASE("online_http_live: tone3000 latest without a token answers 401", "[live]")
{
    if (! networkTestsEnabled())
        SKIP("set AMP_NETWORK_TESTS=1");

    JuceHttpTransport transport;
    HttpRequest req;
    req.url = "https://www.tone3000.com/api/v1/tones/latest";
    const auto res = transport.request(req);
    INFO(res.status << " " << res.body.substring(0, 200));
    REQUIRE(res.error.isEmpty());
    REQUIRE((res.status == 401 || res.status == 403));
}

TEST_CASE("online_http_live: robots.txt streams to a file with progress", "[live]")
{
    if (! networkTestsEnabled())
        SKIP("set AMP_NETWORK_TESTS=1");

    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile("amp-http-live-" + juce::String::toHexString(juce::Random::getSystemRandom().nextInt64()));
    const auto target = dir.getChildFile("nested").getChildFile("robots.txt");

    int progressCalls = 0;
    juce::int64 last = 0;
    JuceHttpTransport transport;
    HttpRequest req;
    req.url = "https://www.tone3000.com/robots.txt";
    req.toFile = target;
    req.onProgress = [&](juce::int64 received, juce::int64)
    {
        ++progressCalls;
        last = received;
    };
    const auto res = transport.request(req);
    INFO(res.status << " " << res.error);
    REQUIRE(res.ok());
    REQUIRE(res.body.isEmpty());
    REQUIRE(target.existsAsFile());
    REQUIRE(target.getSize() > 0);
    REQUIRE(progressCalls >= 1);
    REQUIRE(last == target.getSize());
    dir.deleteRecursively();
}
