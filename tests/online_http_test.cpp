#include <catch2/catch_test_macros.hpp>

#include "online/JuceHttpTransport.h"

using namespace amp::online;

TEST_CASE("online_http: originOf lower-cases and fills default ports")
{
    auto a = http::originOf("HTTPS://WWW.Tone3000.com/api/v1/x?y=1#f");
    REQUIRE(a.valid());
    REQUIRE(a.scheme == "https");
    REQUIRE(a.host == "www.tone3000.com");
    REQUIRE(a.port == 443);
    REQUIRE(http::originOf("http://a.b").port == 80);
    REQUIRE(http::originOf("http://a.b:8080/x").port == 8080);
    REQUIRE(http::originOf("https://user:pw@a.b:444/x").host == "a.b");
    REQUIRE(http::originOf("https://a.b:444/x").port == 444);
}

TEST_CASE("online_http: originOf rejects anything but http(s)")
{
    REQUIRE_FALSE(http::originOf("file:///etc/passwd").valid());
    REQUIRE_FALSE(http::originOf("ftp://a.b/x").valid());
    REQUIRE_FALSE(http::originOf("javascript:alert(1)").valid());
    REQUIRE_FALSE(http::originOf("").valid());
    REQUIRE_FALSE(http::originOf("https:///nohost").valid());
    REQUIRE_FALSE(http::originOf("https://a.b:notaport/").valid());
}

TEST_CASE("online_http: resolveRedirect handles absolute, relative and protocol-relative locations")
{
    const juce::String cur = "https://a.example/dir/sub/file.bin?x=1";
    REQUIRE(http::resolveRedirect(cur, "https://cdn.example/f?sig=2#frag") == "https://cdn.example/f?sig=2");
    REQUIRE(http::resolveRedirect(cur, "//cdn.example/f") == "https://cdn.example/f");
    REQUIRE(http::resolveRedirect(cur, "/root/f?q=1") == "https://a.example/root/f?q=1");
    REQUIRE(http::resolveRedirect(cur, "other.bin") == "https://a.example/dir/sub/other.bin");
    REQUIRE(http::resolveRedirect(cur, "../up.bin") == "https://a.example/dir/up.bin");
    REQUIRE(http::resolveRedirect(cur, "./same.bin") == "https://a.example/dir/sub/same.bin");
    REQUIRE(http::resolveRedirect(cur, "?y=2") == "https://a.example/dir/sub/file.bin?y=2");
    REQUIRE(http::resolveRedirect("http://a.example:81/x", "/y") == "http://a.example:81/y");
}

TEST_CASE("online_http: resolveRedirect refuses empty or malformed locations")
{
    REQUIRE(http::resolveRedirect("https://a.example/x", "").isEmpty());
    REQUIRE(http::resolveRedirect("https://a.example/x", "bad location").isEmpty());
    REQUIRE(http::resolveRedirect("not a url", "/y").isEmpty());
}

TEST_CASE("online_http: only 301 302 303 307 308 are followed")
{
    for (int s : { 301, 302, 303, 307, 308 })
        REQUIRE(http::isFollowedRedirectStatus(s));
    for (int s : { 200, 300, 304, 305, 306, 309, 401 })
        REQUIRE_FALSE(http::isFollowedRedirectStatus(s));
}

TEST_CASE("online_http: 303 and POST 301/302 become a GET, 307/308 keep the method")
{
    REQUIRE(http::redirectBecomesGet(303, "GET"));
    REQUIRE(http::redirectBecomesGet(303, "POST"));
    REQUIRE(http::redirectBecomesGet(301, "POST"));
    REQUIRE(http::redirectBecomesGet(302, "POST"));
    REQUIRE_FALSE(http::redirectBecomesGet(301, "GET"));
    REQUIRE_FALSE(http::redirectBecomesGet(307, "POST"));
    REQUIRE_FALSE(http::redirectBecomesGet(308, "POST"));
}

TEST_CASE("online_http: a same-origin redirect keeps Authorization")
{
    auto d = http::decideRedirect("https://a.example/x", "https://a.example/x", "https://A.example:443/y", true);
    REQUIRE(d.action == http::RedirectDecision::Action::follow);
    REQUIRE(d.sendAuthorization);
}

TEST_CASE("online_http: a cross-origin redirect drops Authorization for that hop and every later one")
{
    for (const char* other : { "https://cdn.example/y", "https://a.example:8443/y", "http://a.example/y" })
    {
        auto d = http::decideRedirect("https://a.example/x", "https://a.example/x", other, true);
        if (juce::String(other).startsWith("http://"))
        {
            REQUIRE(d.action == http::RedirectDecision::Action::refuse); // downgrade
            continue;
        }
        REQUIRE(d.action == http::RedirectDecision::Action::follow);
        REQUIRE_FALSE(d.sendAuthorization);
    }

    // Hop 2 goes back to the original origin: the header stays dropped.
    auto back = http::decideRedirect("https://a.example/x", "https://cdn.example/y", "https://a.example/z", false);
    REQUIRE(back.action == http::RedirectDecision::Action::follow);
    REQUIRE_FALSE(back.sendAuthorization);
}

TEST_CASE("online_http: an https to http redirect is refused")
{
    auto d = http::decideRedirect("https://a.example/x", "https://a.example/x", "http://a.example/x", true);
    REQUIRE(d.action == http::RedirectDecision::Action::refuse);
    REQUIRE(d.error == "refused insecure redirect");
    REQUIRE_FALSE(d.sendAuthorization);

    // http to https is an upgrade and fine (cross-origin, so the header goes).
    auto up = http::decideRedirect("http://a.example/x", "http://a.example/x", "https://a.example/x", true);
    REQUIRE(up.action == http::RedirectDecision::Action::follow);
}

TEST_CASE("online_http: a redirect to a non-http scheme is refused as unsupported")
{
    auto d = http::decideRedirect("https://a.example/x", "https://a.example/x", "file:///etc/passwd", true);
    REQUIRE(d.action == http::RedirectDecision::Action::refuse);
    REQUIRE(d.error == "unsupported URL");
    auto e = http::decideRedirect("https://a.example/x", "https://a.example/x", "", true);
    REQUIRE(e.action == http::RedirectDecision::Action::refuse);
}

TEST_CASE("online_http: unsupported URLs fail without touching the network")
{
    JuceHttpTransport t;
    for (const char* url : { "file:///etc/passwd", "ftp://a.example/x", "javascript:1", "", "/relative" })
    {
        HttpRequest r;
        r.url = url;
        auto res = t.request(r);
        REQUIRE(res.status == 0);
        REQUIRE(res.error == "unsupported URL");
    }
}

TEST_CASE("online_http: a canceled request answers canceled before connecting")
{
    JuceHttpTransport t;
    HttpRequest r;
    r.url = "http://127.0.0.1:9/never";
    r.shouldCancel = [] { return true; };
    auto res = t.request(r);
    REQUIRE(res.status == 0);
    REQUIRE(res.error == "canceled");
}
