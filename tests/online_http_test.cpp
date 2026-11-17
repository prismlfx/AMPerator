#include <catch2/catch_test_macros.hpp>

#include "online/JuceHttpTransport.h"
#include "support/LoopbackHttpServer.h"

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
    auto d = http::decideRedirect("https://a.example/x", "https://a.example/x", "https://A.example:443/y", true, false);
    REQUIRE(d.action == http::RedirectDecision::Action::follow);
    REQUIRE(d.sendAuthorization);
}

TEST_CASE("online_http: a cross-origin redirect drops Authorization for that hop and every later one")
{
    for (const char* other : { "https://cdn.example/y", "https://a.example:8443/y", "http://a.example/y" })
    {
        auto d = http::decideRedirect("https://a.example/x", "https://a.example/x", other, true, false);
        if (juce::String(other).startsWith("http://"))
        {
            REQUIRE(d.action == http::RedirectDecision::Action::refuse); // downgrade
            continue;
        }
        REQUIRE(d.action == http::RedirectDecision::Action::follow);
        REQUIRE_FALSE(d.sendAuthorization);
    }

    // Hop 2 goes back to the original origin: the header stays dropped.
    auto back = http::decideRedirect("https://a.example/x", "https://cdn.example/y", "https://a.example/z", false, false);
    REQUIRE(back.action == http::RedirectDecision::Action::follow);
    REQUIRE_FALSE(back.sendAuthorization);
}

TEST_CASE("online_http: an https to http redirect is refused")
{
    auto d = http::decideRedirect("https://a.example/x", "https://a.example/x", "http://a.example/x", true, false);
    REQUIRE(d.action == http::RedirectDecision::Action::refuse);
    REQUIRE(d.error == "refused insecure redirect");
    REQUIRE_FALSE(d.sendAuthorization);

    // http to https is an upgrade and fine (cross-origin, so the header goes).
    auto up = http::decideRedirect("http://a.example/x", "http://a.example/x", "https://a.example/x", true, false);
    REQUIRE(up.action == http::RedirectDecision::Action::follow);
}

TEST_CASE("online_http: a redirect to a non-http scheme is refused as unsupported")
{
    auto d = http::decideRedirect("https://a.example/x", "https://a.example/x", "file:///etc/passwd", true, false);
    REQUIRE(d.action == http::RedirectDecision::Action::refuse);
    REQUIRE(d.error == "unsupported URL");
    auto e = http::decideRedirect("https://a.example/x", "https://a.example/x", "", true, false);
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

TEST_CASE("online_http: a cross-origin hop that would still carry the body is refused")
{
    auto d = http::decideRedirect("https://a.example/token", "https://a.example/token", "https://b.example/t", true,
                                  true);
    REQUIRE(d.action == http::RedirectDecision::Action::refuse);
    REQUIRE(d.error == "refused cross-origin redirect");
    auto same = http::decideRedirect("https://a.example/token", "https://a.example/token", "https://a.example/t2",
                                     true, true);
    REQUIRE(same.action == http::RedirectDecision::Action::follow);
    REQUIRE(same.sendAuthorization);
}

TEST_CASE("online_http: resolveRedirect only treats a valid scheme before :// as absolute")
{
    REQUIRE(http::resolveRedirect("https://a.example/x", "/login?next=https://x")
            == "https://a.example/login?next=https://x");
    REQUIRE(http::resolveRedirect("https://a.example/x", "p/q?u=http://y") == "https://a.example/p/q?u=http://y");
}

TEST_CASE("online_http: redirect targets with userinfo, backslashes or control characters are refused")
{
    for (const char* loc : { "https://evil.example@good.example/x", "https://evil.example\\@good.example/", "//u@h.example/x",
                             "https://a.example\tb/x" })
        REQUIRE(http::resolveRedirect("https://a.example/x", loc).isEmpty());
    auto d = http::decideRedirect("https://a.example/x", "https://a.example/x", "https://u@a.example/x", true, false);
    REQUIRE(d.action == http::RedirectDecision::Action::refuse);
    REQUIRE(d.error == "unsupported URL");
}

// ---- the stateful request() path against a loopback server (127.0.0.1, no real network) ----

namespace
{
using amp_test::LoopbackHttpServer;

LoopbackHttpServer::Reply redirect(int status, const juce::String& location)
{
    LoopbackHttpServer::Reply r;
    r.status = status;
    r.headers.set("Location", location);
    return r;
}

LoopbackHttpServer::Reply ok(const std::string& body = "ok")
{
    LoopbackHttpServer::Reply r;
    r.body = body;
    return r;
}

HttpRequest authed(const juce::String& url)
{
    HttpRequest r;
    r.url = url;
    r.timeoutMs = 5000;
    r.headers.set("Authorization", "Bearer secret");
    return r;
}

struct TempDir
{
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("amp_http_" + juce::Uuid().toString());
    ~TempDir() { dir.deleteRecursively(); }
};
} // namespace

TEST_CASE("online_http: a cross-origin 302 drops Authorization on the next origin")
{
    LoopbackHttpServer a, b;
    b.on("/dest", ok("from b"));
    a.on("/start", redirect(302, b.url("/dest")));
    auto res = JuceHttpTransport().request(authed(a.url("/start")));
    REQUIRE(res.ok());
    REQUIRE(res.body == "from b");
    REQUIRE(a.requestsFor("/start").size() == 1);
    REQUIRE(a.requestsFor("/start")[0].hasHeader("Authorization"));
    REQUIRE(b.requestsFor("/dest").size() == 1);
    REQUIRE_FALSE(b.requestsFor("/dest")[0].hasHeader("Authorization"));
}

TEST_CASE("online_http: a same-origin 302 keeps Authorization")
{
    LoopbackHttpServer a;
    a.on("/start", redirect(302, "/dest"));
    a.on("/dest", ok());
    auto res = JuceHttpTransport().request(authed(a.url("/start")));
    REQUIRE(res.ok());
    REQUIRE(a.requestsFor("/dest").size() == 1);
    REQUIRE(a.requestsFor("/dest")[0].hasHeader("Authorization"));
}

TEST_CASE("online_http: a cross-origin 307 on a POST is refused and the other origin sees nothing")
{
    LoopbackHttpServer a, b;
    b.on("/dest", ok());
    a.on("/token", redirect(307, b.url("/dest")));
    HttpRequest r = authed(a.url("/token"));
    r.method = "POST";
    r.body = "code=secret&code_verifier=v";
    auto res = JuceHttpTransport().request(r);
    REQUIRE(res.status == 0);
    REQUIRE(res.error == "refused cross-origin redirect");
    REQUIRE(b.requests().empty());
}

TEST_CASE("online_http: a same-origin 307 on a POST re-sends the body")
{
    LoopbackHttpServer a;
    a.on("/token", redirect(307, "/token2"));
    a.on("/token2", ok());
    HttpRequest r = authed(a.url("/token"));
    r.method = "POST";
    r.body = "x=1";
    REQUIRE(JuceHttpTransport().request(r).ok());
    auto seen = a.requestsFor("/token2");
    REQUIRE(seen.size() == 1);
    REQUIRE(seen[0].method == "POST");
    REQUIRE(seen[0].body == "x=1");
}

TEST_CASE("online_http: a 303 on a POST becomes a GET without a body, even across origins")
{
    LoopbackHttpServer a, b;
    b.on("/dest", ok());
    a.on("/token", redirect(303, b.url("/dest")));
    HttpRequest r = authed(a.url("/token"));
    r.method = "POST";
    r.body = "code=secret";
    auto res = JuceHttpTransport().request(r);
    REQUIRE(res.ok());
    auto seen = b.requestsFor("/dest");
    REQUIRE(seen.size() == 1);
    REQUIRE(seen[0].method == "GET");
    REQUIRE(seen[0].body.isEmpty());
    REQUIRE_FALSE(seen[0].hasHeader("Authorization"));
}

TEST_CASE("online_http: more than five redirects fail")
{
    LoopbackHttpServer a;
    a.on("/loop", redirect(302, "/loop"));
    auto res = JuceHttpTransport().request(authed(a.url("/loop")));
    REQUIRE(res.status == 0);
    REQUIRE(res.error == "too many redirects");
    REQUIRE(a.requestsFor("/loop").size() == (size_t) JuceHttpTransport::maxRedirects + 1);
}

TEST_CASE("online_http: canceling mid-body with toFile reports canceled and leaves no file")
{
    LoopbackHttpServer a;
    LoopbackHttpServer::Reply slow;
    slow.generatedBytes = 4 * 1024 * 1024;
    slow.chunkDelayMs = 10;
    a.on("/big", slow);
    TempDir t;
    auto target = t.dir.getChildFile("sub").getChildFile("big.bin");
    std::atomic<bool> cancel { false };
    HttpRequest r = authed(a.url("/big"));
    r.toFile = target;
    r.onProgress = [&](juce::int64, juce::int64) { cancel = true; };
    r.shouldCancel = [&] { return cancel.load(); };
    auto res = JuceHttpTransport().request(r);
    REQUIRE(res.status == 0);
    REQUIRE(res.error == "canceled");
    REQUIRE_FALSE(target.exists());
}

TEST_CASE("online_http: a body over the 8 MiB cap is too large, exactly the cap is fine")
{
    LoopbackHttpServer a;
    LoopbackHttpServer::Reply exact, over;
    exact.generatedBytes = JuceHttpTransport::maxBodyBytes;
    over.generatedBytes = JuceHttpTransport::maxBodyBytes + 1;
    a.on("/exact", exact);
    a.on("/over", over);
    auto good = JuceHttpTransport().request(authed(a.url("/exact")));
    REQUIRE(good.ok());
    REQUIRE((juce::int64) good.body.getNumBytesAsUTF8() == JuceHttpTransport::maxBodyBytes);
    auto bad = JuceHttpTransport().request(authed(a.url("/over")));
    REQUIRE(bad.error == "response too large");
    REQUIRE(bad.body.isEmpty());
}

TEST_CASE("online_http: a 404 with toFile leaves no file and returns a bounded body")
{
    LoopbackHttpServer a;
    LoopbackHttpServer::Reply nf;
    nf.status = 404;
    nf.generatedBytes = 200 * 1024;
    a.on("/missing", nf);
    TempDir t;
    auto target = t.dir.getChildFile("x.bin");
    HttpRequest r = authed(a.url("/missing"));
    r.toFile = target;
    auto res = JuceHttpTransport().request(r);
    REQUIRE(res.status == 404);
    REQUIRE(res.error.isEmpty());
    REQUIRE_FALSE(target.exists());
    REQUIRE((juce::int64) res.body.getNumBytesAsUTF8() <= JuceHttpTransport::maxErrorBodyBytes);
    REQUIRE(res.body.isNotEmpty());
}

TEST_CASE("online_http: a streamed 200 to toFile writes every byte and reports progress with the total")
{
    LoopbackHttpServer a;
    LoopbackHttpServer::Reply big;
    big.generatedBytes = 300 * 1000;
    a.on("/file", big);
    TempDir t;
    auto target = t.dir.getChildFile("deep").getChildFile("f.bin");
    int calls = 0;
    juce::int64 lastReceived = 0, lastTotal = 0;
    HttpRequest r = authed(a.url("/file"));
    r.toFile = target;
    r.onProgress = [&](juce::int64 received, juce::int64 total)
    {
        ++calls;
        lastReceived = received;
        lastTotal = total;
    };
    auto res = JuceHttpTransport().request(r);
    REQUIRE(res.ok());
    REQUIRE(res.body.isEmpty());
    REQUIRE(target.getSize() == 300 * 1000);
    REQUIRE(calls >= 1);
    REQUIRE(lastReceived == 300 * 1000);
    REQUIRE(lastTotal == 300 * 1000);
    juce::MemoryBlock bytes;
    REQUIRE(target.loadFileAsData(bytes));
    for (size_t i = 0; i < bytes.getSize(); ++i)
        REQUIRE(bytes[i] == 'a');
}

TEST_CASE("online_http: a first-hop URL with userinfo, a backslash or control characters is refused before connecting")
{
    LoopbackHttpServer a;
    a.on("/x", ok());
    const auto hostPort = "127.0.0.1:" + juce::String(a.port());
    for (const auto& url : { "http://user:pass@" + hostPort + "/x", "http://" + hostPort + "@" + hostPort + "/x",
                             "http://" + hostPort + "\\@" + hostPort + "/x", "http://127.0.0.1\t:" + juce::String(a.port()) + "/x" })
    {
        auto res = JuceHttpTransport().request(authed(url));
        INFO(url);
        REQUIRE(res.status == 0);
        REQUIRE(res.error == "unsupported URL");
    }
    REQUIRE(a.requests().empty());
    REQUIRE_FALSE(http::hasPlainAuthority("https://www.tone3000.com:443@evil.example/x"));
    REQUIRE(http::hasPlainAuthority("https://www.tone3000.com:443/x"));
}

TEST_CASE("online_http: requests ask caches not to store them")
{
    LoopbackHttpServer a;
    a.on("/x", ok());
    REQUIRE(JuceHttpTransport().request(authed(a.url("/x"))).ok());
    REQUIRE(a.requestsFor("/x").size() == 1);
    REQUIRE(a.requestsFor("/x")[0].headers.getValue("Cache-Control", {}) == "no-store");
}
