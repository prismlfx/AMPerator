#include <catch2/catch_test_macros.hpp>

#include <set>

#include "support/FakeHttp.h"
#include "online/HttpTransport.h"
#include "online/Pkce.h"

using namespace amp::online;

TEST_CASE("online_pkce: challengeFor matches RFC 7636 appendix B")
{
    REQUIRE(pkce::challengeFor("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk")
            == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
}

TEST_CASE("online_pkce: randomUrlSafe is 43 url-safe chars and differs per call")
{
    auto a = pkce::randomUrlSafe(32);
    auto b = pkce::randomUrlSafe(32);
    REQUIRE(a.length() == 43);
    REQUIRE(a.containsOnly("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"));
    REQUIRE(a != b);
}

TEST_CASE("online_pkce: parseQuery decodes a bare query")
{
    auto q = pkce::parseQuery("?code=a%20b&state=x&canceled=true");
    REQUIRE(q.size() == 3);
    REQUIRE(q["code"] == "a b");
    REQUIRE(q["state"] == "x");
    REQUIRE(q["canceled"] == "true");
}

TEST_CASE("online_pkce: parseQuery on a full URL ignores everything before the question mark")
{
    auto q = pkce::parseQuery("http://127.0.0.1/callback?code=abc&state=s1");
    REQUIRE(q.size() == 2);
    REQUIRE(q["code"] == "abc");
    REQUIRE(q["state"] == "s1");
    REQUIRE(pkce::parseQuery("a=1&a=2")["a"] == "2");
}

TEST_CASE("online_pkce: encodeQuery round-trips through parseQuery")
{
    juce::StringPairArray in;
    in.set("redirect_uri", "http://127.0.0.1/callback");
    in.set("v", "a&b=c/d+e f");
    auto text = pkce::encodeQuery(in);
    REQUIRE_FALSE(text.contains("+"));
    REQUIRE(text.indexOfChar('/') < 0);
    auto out = pkce::parseQuery(text);
    REQUIRE(out.size() == 2);
    REQUIRE(out["redirect_uri"] == in["redirect_uri"]);
    REQUIRE(out["v"] == in["v"]);
}

TEST_CASE("online_pkce: FakeHttp matches by method and first registered prefix, else errors")
{
    amp_test::FakeHttp http;
    http.on("GET", "https://x.test/a", [](const HttpRequest&) { return amp_test::FakeHttp::json(200, "first"); });
    http.on("GET", "https://x.test/a/b", [](const HttpRequest&) { return amp_test::FakeHttp::json(200, "second"); });
    http.on("POST", "https://x.test/", [](const HttpRequest& r) {
        return amp_test::FakeHttp::json(201, r.body, [] { juce::StringPairArray h; h.set("X-Test", "yes"); return h; }());
    });

    HttpRequest get;
    get.url = "https://x.test/a/b";
    REQUIRE(http.request(get).body == "first");

    HttpRequest post;
    post.method = "POST";
    post.url = "https://x.test/z";
    post.body = "k=v";
    auto r = http.request(post);
    REQUIRE(r.ok());
    REQUIRE(r.status == 201);
    REQUIRE(r.body == "k=v");
    REQUIRE(r.header("x-test") == "yes");

    HttpRequest miss;
    miss.url = "https://other.test/";
    auto m = http.request(miss);
    REQUIRE(m.status == 0);
    REQUIRE_FALSE(m.ok());
    REQUIRE(m.error == "no fake response for GET https://other.test/");
    REQUIRE(http.requestCount() == 3);
    REQUIRE(http.snapshot()[2].url == "https://other.test/");
}

TEST_CASE("online_pkce: FakeHttp::file writes to toFile when set")
{
    amp_test::FakeHttp http;
    juce::MemoryBlock bytes("hello", 5);
    http.on("GET", "https://x.test/f", [&](const HttpRequest&) { return amp_test::FakeHttp::file(200, bytes); });

    auto f = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("amp_fakehttp_test", ".bin");
    f.deleteFile();
    HttpRequest req;
    req.url = "https://x.test/f";
    req.toFile = f;
    auto r = http.request(req);
    REQUIRE(r.ok());
    REQUIRE(r.body.isEmpty());
    REQUIRE(f.loadFileAsString() == "hello");
    f.deleteFile();
}

TEST_CASE("online_pkce: FakeHttp::file built ahead of time and captured still writes toFile")
{
    amp_test::FakeHttp http;
    juce::MemoryBlock bytes("abc", 3);
    const auto prebuilt = amp_test::FakeHttp::file(200, bytes); // built on this thread, before any request
    http.on("GET", "https://x.test/f", [prebuilt](const HttpRequest&) { return prebuilt; });

    auto f = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("amp_fakehttp_pre", ".bin");
    HttpRequest req;
    req.url = "https://x.test/f";
    req.toFile = f;
    auto r = http.request(req);
    REQUIRE(r.ok());
    REQUIRE(r.header("X-Fake-File-Id").isEmpty());
    REQUIRE(f.loadFileAsString() == "abc");
    f.deleteFile();

    HttpRequest noFile;
    noFile.url = "https://x.test/f";
    auto again = http.request(noFile);
    REQUIRE(again.status == 200);
    REQUIRE(again.body == "abc");
}

TEST_CASE("online_pkce: FakeHttp does not match a POST against a GET route")
{
    amp_test::FakeHttp http;
    http.on("GET", "https://x.test/", [](const HttpRequest&) { return amp_test::FakeHttp::json(200, "{}"); });
    HttpRequest post;
    post.method = "POST";
    post.url = "https://x.test/a";
    REQUIRE(http.request(post).status == 0);
}
