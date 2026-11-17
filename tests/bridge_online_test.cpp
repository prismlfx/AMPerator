#include <catch2/catch_test_macros.hpp>

#include <atomic>

#include "Engine.h"
#include "online/SecureStore.h"
#include "online/Tone3000Types.h"

#include "support/OnlineFixture.h"

using namespace amp;
using namespace amp_test;
using amp::online::HttpRequest;

TEST_CASE("bridge_online: getState reports a configured, disconnected TONE3000")
{
    OnlineFixture f;
    const auto t = f.state()["tone3000"];
    CHECK(static_cast<bool>(t["configured"]));
    CHECK(isFalse(t["connected"]));
    CHECK(isFalse(t["busy"]));
    CHECK(static_cast<bool>(t["remembered"]));
    CHECK(t["user"].isVoid());
    CHECK(juce::JSON::toString(f.status(), true) == juce::JSON::toString(t, true));
    CHECK(f.http->requestCount() == 0); // nothing is fetched while disconnected
}

TEST_CASE("bridge_online: tone3000Connect opens the sign-in window and replies after the callback")
{
    OnlineFixture f;
    const int id = f.startConnect();

    CHECK(f.window.url.startsWith("https://www.tone3000.com/api/v1/oauth/authorize?"));
    CHECK(f.window.redirectPrefix == "http://127.0.0.1/callback");
    CHECK(f.urlParam("client_id") == clientId);
    CHECK(f.urlParam("redirect_uri") == "http://127.0.0.1/callback");
    CHECK(f.urlParam("response_type") == "code");
    CHECK(f.urlParam("scope") == "read");
    CHECK(f.urlParam("code_challenge_method") == "S256");
    CHECK(f.urlParam("code_challenge").isNotEmpty());
    CHECK(f.urlParam("state").isNotEmpty());
    CHECK_FALSE(f.window.url.contains("prompt"));

    CHECK(static_cast<bool>(f.lastEvent("tone3000Status")["busy"]));
    Fixture::pump(100);
    CHECK(f.replyTo(id).isVoid()); // no reply while the window is open

    f.window.done("code=c&state=" + f.urlParam("state"));
    const auto r = f.waitReply(id);
    INFO(juce::JSON::toString(r, true));
    REQUIRE(static_cast<bool>(r["ok"]));
    CHECK(static_cast<bool>(r["result"]["connected"]));
    CHECK(r["result"]["user"]["username"].toString() == "jane");

    const auto ev = f.lastEvent("tone3000Status");
    CHECK(static_cast<bool>(ev["connected"]));
    CHECK(isFalse(ev["busy"]));
    CHECK(ev["user"]["username"].toString() == "jane");

    const auto stored = f.backing.get(clientId); // the store key is the client id
    REQUIRE(stored.has_value());
    CHECK(juce::JSON::parse(*stored)["access"].toString() == "A1");

    const auto t = f.state()["tone3000"];
    CHECK(static_cast<bool>(t["connected"]));
    CHECK(t["user"]["displayName"].toString() == "Jane Test");
}

TEST_CASE("bridge_online: closing the sign-in window replies canceled")
{
    OnlineFixture f;
    const int id = f.startConnect();
    f.window.done("");
    const auto r = f.waitReply(id);
    REQUIRE(static_cast<bool>(r["ok"]));
    CHECK(static_cast<bool>(r["result"]["canceled"]));
    CHECK(isFalse(f.lastEvent("tone3000Status")["busy"]));
    CHECK(isFalse(f.status()["connected"]));
    CHECK(f.http->requestCount() == 0);
    CHECK(f.events("error").empty()); // no error toast
}

TEST_CASE("bridge_online: a callback with the wrong state is refused")
{
    OnlineFixture f;
    const int id = f.startConnect();
    f.window.done("code=c&state=not-the-state");
    const auto r = f.waitReply(id);
    CHECK_FALSE(static_cast<bool>(r["ok"]));
    CHECK(r["error"].toString().contains("didn't match"));
    CHECK(f.lastEvent("error")["context"].toString() == "tone3000");
    CHECK(f.http->requestCount() == 0); // no token exchange
    CHECK(isFalse(f.status()["connected"]));
    CHECK(isFalse(f.status()["busy"]));
}

TEST_CASE("bridge_online: a second tone3000Connect while the window is open is busy")
{
    OnlineFixture f;
    const int first = f.startConnect();
    const auto second = f.call("tone3000Connect");
    CHECK_FALSE(static_cast<bool>(second["ok"]));
    CHECK(second["error"].toString() == "busy");
    CHECK(f.window.opened == 1);

    f.window.done("");
    CHECK(static_cast<bool>(f.waitReply(first)["result"]["canceled"]));
    f.connect(); // free again
}

TEST_CASE("bridge_online: tone3000Connect without a window hook is not available")
{
    OnlineFixture f;
    f.engine->onOpenAuthWindow = nullptr;
    const auto r = f.callLater("tone3000Connect");
    CHECK_FALSE(static_cast<bool>(r["ok"]));
    CHECK(r["error"].toString() == "not available");
    CHECK(isFalse(f.status()["busy"]));
}

TEST_CASE("bridge_online: a completion called twice replies once")
{
    OnlineFixture f;
    const int id = f.startConnect();
    auto done = f.window.done;
    done("");
    f.waitReply(id);
    done(""); // the app's window reporting again must not produce a second reply
    Fixture::pump(50);
    int replies = 0;
    for (auto& m : f.sent)
        if (m.hasProperty("reply") && m["reply"] == juce::var(id))
            ++replies;
    CHECK(replies == 1);
}

TEST_CASE("bridge_online: tone3000Disconnect removes the tokens and clears the cookies")
{
    OnlineFixture f;
    f.connect();
    REQUIRE(f.backing.get(clientId).has_value());

    const auto r = f.ok("tone3000Disconnect");
    CHECK(r.getDynamicObject() != nullptr);
    CHECK_FALSE(f.backing.get(clientId).has_value());
    CHECK(f.cookiesCleared == 1);
    const auto ev = f.lastEvent("tone3000Status");
    CHECK(isFalse(ev["connected"]));
    CHECK(ev["user"].isVoid());
    CHECK(isFalse(f.state()["tone3000"]["connected"]));

    // Already disconnected: still answers and reports the status.
    const auto before = f.events("tone3000Status").size();
    f.ok("tone3000Disconnect");
    CHECK(f.events("tone3000Status").size() == before + 1);
    CHECK(f.cookiesCleared == 2);
}

TEST_CASE("bridge_online: without a client id every tone3000 command but status is not configured")
{
    if (compiledTone3000ClientId().isNotEmpty())
        SKIP("this build has a TONE3000 client id compiled in");

    OnlineFixture f("");
    const auto t = f.state()["tone3000"];
    CHECK(isFalse(t["configured"]));
    CHECK(isFalse(t["connected"]));
    CHECK(isFalse(t["busy"]));
    CHECK(static_cast<bool>(t["remembered"]));
    CHECK(t["user"].isVoid());
    CHECK(isFalse(f.status()["configured"]));

    for (const auto& [cmd, args] : { std::pair { "tone3000Connect", obj() }, std::pair { "tone3000Disconnect", obj() },
                                     std::pair { "tone3000Browse", obj() },
                                     std::pair { "tone3000List", obj({ { "list", "latest" } }) },
                                     std::pair { "tone3000Tone", obj({ { "id", "t_1" } }) } })
    {
        const auto r = f.call(cmd, args);
        CHECK_FALSE(static_cast<bool>(r["ok"]));
        CHECK(r["error"].toString() == "not configured");
    }
    CHECK(f.window.opened == 0);
    CHECK(f.cookiesCleared == 0);
    CHECK(f.http->requestCount() == 0);
    CHECK(f.storeAccesses.load() == 0);
}

TEST_CASE("bridge_online: a platform store that refuses writes keeps the session in memory")
{
    OnlineFixture f(clientId, true);
    const auto r = f.connect();
    CHECK(static_cast<bool>(r["connected"]));
    CHECK(r["user"]["username"].toString() == "jane");

    const auto t = f.status();
    CHECK(static_cast<bool>(t["connected"]));
    CHECK(isFalse(t["remembered"]));
    CHECK(isFalse(f.lastEvent("tone3000Status")["remembered"]));

    f.ok("tone3000Disconnect");
    CHECK(isFalse(f.status()["connected"]));
}

TEST_CASE("bridge_online: tokens in the store keep TONE3000 connected after a restart")
{
    OnlineFixture f;
    f.connect();

    f.start(); // a new Engine on the same store
    CHECK(static_cast<bool>(f.state()["tone3000"]["connected"]));

    const auto deadline = juce::Time::getMillisecondCounter() + 5000;
    while (f.events("tone3000Status").empty() && juce::Time::getMillisecondCounter() < deadline)
        Fixture::pump(10);
    REQUIRE_FALSE(f.events("tone3000Status").empty());
    CHECK(f.lastEvent("tone3000Status")["user"]["username"].toString() == "jane");
    CHECK(f.state()["tone3000"]["user"]["username"].toString() == "jane");
}

// ---- Browse ----

TEST_CASE("bridge_online: tone3000Browse opens the picker and replies the tone id with the node id")
{
    OnlineFixture f;
    f.connect();
    const int id = f.post("tone3000Browse", obj({ { "format", "ir" }, { "nodeId", "n3" } }));
    REQUIRE(f.window.opened == 2);
    CHECK(f.urlParam("prompt") == "select_tone");
    CHECK(f.urlParam("preview") == "true");
    CHECK(f.urlParam("menubar") == "true");
    CHECK(f.urlParam("architecture") == "2");
    CHECK(f.urlParam("format") == "ir");
    CHECK(f.urlParam("client_id") == clientId);
    CHECK(f.urlParam("state").isNotEmpty());
    CHECK(static_cast<bool>(f.status()["busy"]));
    Fixture::pump(50);
    CHECK(f.replyTo(id).isVoid());

    f.window.done("tone_id=t_1&state=" + f.urlParam("state"));
    const auto r = f.waitReply(id);
    INFO(juce::JSON::toString(r, true));
    REQUIRE(static_cast<bool>(r["ok"]));
    CHECK(r["result"]["toneId"].toString() == "t_1");
    CHECK(r["result"]["nodeId"].toString() == "n3");
    CHECK(isFalse(f.status()["busy"]));
}

TEST_CASE("bridge_online: tone3000Browse without a format or node sends neither")
{
    OnlineFixture f;
    f.connect();
    const int id = f.post("tone3000Browse");
    CHECK_FALSE(f.window.url.contains("format="));
    f.window.done("tone_id=t_2&state=" + f.urlParam("state"));
    const auto r = f.waitReply(id);
    CHECK(r["result"]["toneId"].toString() == "t_2");
    CHECK_FALSE(r["result"].hasProperty("nodeId"));
}

TEST_CASE("bridge_online: tone3000Browse canceled, closed or without a tone replies canceled")
{
    OnlineFixture f;
    f.connect();
    for (auto* query : { "canceled=true&state=", "", "state=" })
    {
        const int id = f.post("tone3000Browse");
        const juce::String q(query);
        f.window.done(q.isEmpty() ? q : q + f.urlParam("state"));
        const auto r = f.waitReply(id);
        INFO(query << " -> " << juce::JSON::toString(r, true));
        REQUIRE(static_cast<bool>(r["ok"]));
        CHECK(static_cast<bool>(r["result"]["canceled"]));
        CHECK(isFalse(f.status()["busy"]));
    }
}

TEST_CASE("bridge_online: tone3000Browse with loadTone opens load_tone")
{
    OnlineFixture f;
    f.connect();
    const int id = f.post("tone3000Browse", obj({ { "format", "nam" }, { "loadTone", "t_9" } }));
    CHECK(f.urlParam("prompt") == "load_tone");
    CHECK(f.urlParam("tone_id") == "t_9");
    CHECK(f.urlParam("format") == "nam");
    CHECK_FALSE(f.window.url.contains("preview="));
    CHECK_FALSE(f.window.url.contains("architecture="));
    f.window.done("tone_id=t_10&state=" + f.urlParam("state"));
    CHECK(f.waitReply(id)["result"]["toneId"].toString() == "t_10");
}

TEST_CASE("bridge_online: tone3000Browse while disconnected signs in first, then opens the picker")
{
    OnlineFixture f;
    const int id = f.post("tone3000Browse", obj({ { "format", "nam" } }));
    REQUIRE(f.window.opened == 1);
    CHECK_FALSE(f.window.url.contains("prompt"));
    f.window.done("code=c&state=" + f.urlParam("state"));

    f.waitForWindow(2);
    CHECK(f.urlParam("prompt") == "select_tone");
    CHECK(f.urlParam("format") == "nam");
    CHECK(f.replyTo(id).isVoid());
    CHECK(static_cast<bool>(f.status()["connected"]));

    f.window.done("tone_id=t_3&state=" + f.urlParam("state"));
    const auto r = f.waitReply(id);
    REQUIRE(static_cast<bool>(r["ok"]));
    CHECK(r["result"]["toneId"].toString() == "t_3");
    CHECK(f.backing.get(clientId).has_value());
}

TEST_CASE("bridge_online: tone3000Browse while disconnected and the sign-in is closed replies canceled")
{
    OnlineFixture f;
    const int id = f.post("tone3000Browse");
    f.window.done("");
    CHECK(static_cast<bool>(f.waitReply(id)["result"]["canceled"]));
    CHECK(f.window.opened == 1);
    CHECK(isFalse(f.status()["busy"]));
}

TEST_CASE("bridge_online: a picker callback with the wrong state is refused")
{
    OnlineFixture f;
    f.connect();
    const int id = f.post("tone3000Browse");
    f.window.done("tone_id=t_1&state=wrong");
    const auto r = f.waitReply(id);
    CHECK_FALSE(static_cast<bool>(r["ok"]));
    CHECK(r["error"].toString() == "sign-in response didn't match the request");
}

TEST_CASE("bridge_online: tone3000Browse validates its arguments and shares busy with connect")
{
    OnlineFixture f;
    f.connect();
    auto r = f.call("tone3000Browse", obj({ { "format", "wav" } }));
    CHECK_FALSE(static_cast<bool>(r["ok"]));
    CHECK(r["error"].toString().contains("format"));

    const int first = f.startConnect();
    r = f.call("tone3000Browse");
    CHECK(r["error"].toString() == "busy");
    f.window.done("");
    f.waitReply(first);

    f.engine->onOpenAuthWindow = nullptr;
    CHECK(f.callLater("tone3000Browse")["error"].toString() == "not available");
}

// ---- Lists and tone detail ----

TEST_CASE("bridge_online: tone3000List replies the page of tones")
{
    OnlineFixture f;
    f.connect();
    f.serve("/tones/favorited", "list_favorited.json");
    const auto r = f.callLater("tone3000List", obj({ { "list", "favorited" }, { "page", 1 } }));
    INFO(juce::JSON::toString(r, true));
    REQUIRE(static_cast<bool>(r["ok"]));
    auto* tones = r["result"]["tones"].getArray();
    REQUIRE(tones != nullptr);
    REQUIRE(tones->size() == 2);
    CHECK((*tones)[0]["title"].toString() == "Fake Crunch Rig");
    CHECK((*tones)[0]["creator"]["username"].toString() == "jane");
    CHECK((*tones)[1]["creator"]["username"].toString() == "bob");
    CHECK(static_cast<int>(r["result"]["page"]) == 2); // as the fixture says
    CHECK(static_cast<int>(r["result"]["totalPages"]) == 2);
    CHECK(f.http->snapshot().back().url == api + "/tones/favorited?page=1&page_size=25");
}

TEST_CASE("bridge_online: tone3000List validates its arguments")
{
    OnlineFixture f;
    f.connect();
    for (const auto& args : { obj({ { "list", "search" } }), obj(), obj({ { "list", "latest" }, { "page", 0 } }),
                              obj({ { "list", "latest" }, { "page", 1.5 } }) })
    {
        const auto r = f.call("tone3000List", args);
        INFO(juce::JSON::toString(args, true));
        CHECK_FALSE(static_cast<bool>(r["ok"]));
    }
    CHECK(f.http->requestCount() == 2); // only the sign-in's token and user requests
}

TEST_CASE("bridge_online: tone3000List and tone3000Tone need a connection")
{
    OnlineFixture f;
    for (const auto& [cmd, args] : { std::pair { "tone3000List", obj({ { "list", "trending" } }) },
                                     std::pair { "tone3000Tone", obj({ { "id", "t_456" } }) } })
    {
        const auto r = f.call(cmd, args); // replied at once
        CHECK_FALSE(static_cast<bool>(r["ok"]));
        CHECK(r["error"].toString() == "not connected");
    }
    CHECK(f.http->requestCount() == 0);
}

TEST_CASE("bridge_online: tone3000Tone replies the tone with A2 models first")
{
    OnlineFixture f;
    f.connect();
    f.serveTone();
    const auto r = f.callLater("tone3000Tone", obj({ { "id", "t_456" } }));
    INFO(juce::JSON::toString(r, true));
    REQUIRE(static_cast<bool>(r["ok"]));
    CHECK(r["result"]["title"].toString() == "Fake Crunch Rig");
    auto* models = r["result"]["models"].getArray();
    REQUIRE(models != nullptr);
    REQUIRE(models->size() == 3);
    CHECK((*models)[0]["id"].toString() == "m_201");
    CHECK((*models)[0]["architecture"].toString() == "2");
    CHECK((*models)[1]["id"].toString() == "m_123");

    CHECK_FALSE(static_cast<bool>(f.call("tone3000Tone", obj({ { "id", "" } }))["ok"]));
}

TEST_CASE("bridge_online: a rate-limited call says when to try again")
{
    OnlineFixture f;
    f.connect();
    juce::StringPairArray headers;
    headers.set("Retry-After", "7");
    f.http->on("GET", api + "/tones/latest",
               [headers](const HttpRequest&) { return FakeHttp::json(429, "{}", headers); });
    const auto r = f.callLater("tone3000List", obj({ { "list", "latest" } }));
    CHECK_FALSE(static_cast<bool>(r["ok"]));
    CHECK(r["error"].toString() == "TONE3000 rate limit; try again in 7 s");
}

TEST_CASE("bridge_online: client errors become readable messages")
{
    using amp::online::ClientError;
    auto error = [](ClientError::Kind kind, const juce::String& message, int retry = 0)
    {
        ClientError e;
        e.kind = kind;
        e.message = message;
        e.retryAfterSec = retry;
        return amp::online::userMessage(e);
    };
    CHECK(error(ClientError::Kind::RateLimited, "rate limited", 7) == "TONE3000 rate limit; try again in 7 s");
    CHECK(error(ClientError::Kind::RateLimited, "rate limited") == "TONE3000 rate limit; try again later");
    CHECK(error(ClientError::Kind::NotConnected, "x") == "not connected");
    CHECK(error(ClientError::Kind::NotFound, "not found") == "not found on TONE3000");
    CHECK(error(ClientError::Kind::Network, "timed out") == "Can't reach TONE3000: timed out");
    CHECK(error(ClientError::Kind::Http, "HTTP 500") == "HTTP 500");
}

TEST_CASE("bridge_online: a refused token refresh signs out and the call replies not connected")
{
    OnlineFixture f;
    f.connect();
    f.serve("/tones/trending", "list_trending.json");
    // Tokens about to expire, and the token endpoint refuses the refresh.
    REQUIRE(f.backing.set(clientId, R"({"access":"A1","refresh":"R1","expiresAtMs":1})"));
    f.tokenStatus = 400;

    const auto r = f.callLater("tone3000List", obj({ { "list", "trending" } }));
    CHECK_FALSE(static_cast<bool>(r["ok"]));
    CHECK(r["error"].toString() == "not connected");
    f.pump(50);
    const auto ev = f.lastEvent("tone3000Status");
    CHECK(isFalse(ev["connected"]));
    CHECK(ev["user"].isVoid());
    CHECK(isFalse(f.state()["tone3000"]["connected"]));
    CHECK_FALSE(f.backing.get(clientId).has_value());
}

// ---- Window flow edge cases ----

TEST_CASE("bridge_online: a window hook that throws is not available and clears busy")
{
    OnlineFixture f;
    f.engine->onOpenAuthWindow = [](const juce::String&, const juce::String&, std::function<void(const juce::String&)>)
    { throw std::runtime_error("no window"); };
    const auto r = f.callLater("tone3000Connect");
    CHECK_FALSE(static_cast<bool>(r["ok"]));
    CHECK(r["error"].toString() == "not available");
    CHECK(isFalse(f.status()["busy"]));
    CHECK(isFalse(f.lastEvent("tone3000Status")["busy"]));
}

TEST_CASE("bridge_online: disconnect abandons a pending sign-in")
{
    OnlineFixture f;
    const int id = f.startConnect();
    const auto staleDone = f.window.done;
    const auto staleState = f.urlParam("state");
    f.ok("tone3000Disconnect");
    CHECK(isFalse(f.status()["busy"]));

    staleDone("code=c&state=" + staleState); // the old window reports late
    const auto r = f.waitReply(id);
    REQUIRE(static_cast<bool>(r["ok"]));
    CHECK(static_cast<bool>(r["result"]["canceled"]));
    f.pump(50);
    CHECK(isFalse(f.status()["connected"]));
    CHECK(f.http->requestCount() == 0); // the code was never exchanged
    f.connect();                        // a new flow works
}

TEST_CASE("bridge_online: disconnect replies an abandoned sign-in at once")
{
    OnlineFixture f;
    const int id = f.startConnect();
    const auto staleDone = f.window.done;
    f.ok("tone3000Disconnect");
    const auto r = f.replyTo(id); // no window callback needed
    REQUIRE_FALSE(r.isVoid());
    CHECK(static_cast<bool>(r["ok"]));
    CHECK(static_cast<bool>(r["result"]["canceled"]));

    staleDone(""); // the window closing later replies nothing more
    f.pump(50);
    int replies = 0;
    for (auto& m : f.sent)
        if (m.hasProperty("reply") && m["reply"] == juce::var(id))
            ++replies;
    CHECK(replies == 1);
}

TEST_CASE("bridge_online: a late code exchange of an abandoned sign-in never undoes a newer session")
{
    OnlineFixture f;
    f.holdTokenCall = 1; // the first exchange hangs until released
    const int abandoned = f.startConnect();
    f.window.done("code=c&state=" + f.urlParam("state"));
    const auto deadline = juce::Time::getMillisecondCounter() + 5000;
    while (f.tokenCalls.load() < 1 && juce::Time::getMillisecondCounter() < deadline)
        f.pump(10);
    REQUIRE(f.tokenCalls.load() == 1);

    f.ok("tone3000Disconnect");
    CHECK(static_cast<bool>(f.replyTo(abandoned)["result"]["canceled"]));

    f.connect(); // a newer sign-in completes first
    REQUIRE(static_cast<bool>(f.status()["connected"]));

    f.holdTokenCall = 0; // the abandoned exchange finishes now
    f.pump(200);
    CHECK(static_cast<bool>(f.status()["connected"]));
    CHECK(static_cast<bool>(f.lastEvent("tone3000Status")["connected"]));
    CHECK(f.backing.get(clientId).has_value());
}

TEST_CASE("bridge_online: a late code exchange of an abandoned sign-in is undone when nothing newer signed in")
{
    OnlineFixture f;
    f.holdTokenCall = 1;
    f.startConnect();
    f.window.done("code=c&state=" + f.urlParam("state"));
    const auto deadline = juce::Time::getMillisecondCounter() + 5000;
    while (f.tokenCalls.load() < 1 && juce::Time::getMillisecondCounter() < deadline)
        f.pump(10);
    f.ok("tone3000Disconnect");

    f.holdTokenCall = 0;
    f.pump(200);
    CHECK(isFalse(f.status()["connected"]));
    CHECK(isFalse(f.lastEvent("tone3000Status")["connected"]));
    CHECK_FALSE(f.backing.get(clientId).has_value());
}

TEST_CASE("bridge_online: a picker code exchange that reconnects reports the status and loads the user")
{
    OnlineFixture f;
    f.connect();
    f.serve("/tones/trending", "list_trending.json");
    const int browse = f.post("tone3000Browse");
    const auto pickerState = f.urlParam("state");

    // The session ends while the picker is open (refresh refused).
    REQUIRE(f.backing.set(clientId, R"({"access":"A1","refresh":"R1","expiresAtMs":1})"));
    f.tokenStatus = 400;
    CHECK(f.callLater("tone3000List", obj({ { "list", "trending" } }))["error"].toString() == "not connected");
    f.pump(50);
    REQUIRE(isFalse(f.status()["connected"]));

    f.tokenStatus = 200;
    const auto before = f.events("tone3000Status").size();
    f.window.done("code=c2&tone_id=t_1&state=" + pickerState);
    CHECK(f.waitReply(browse)["result"]["toneId"].toString() == "t_1");
    const auto deadline = juce::Time::getMillisecondCounter() + 5000;
    while (f.status()["user"].isVoid() && juce::Time::getMillisecondCounter() < deadline)
        f.pump(10);
    CHECK(static_cast<bool>(f.status()["connected"]));
    CHECK(f.status()["user"]["username"].toString() == "jane");
    bool sawConnected = false;
    const auto all = f.events("tone3000Status");
    for (size_t i = before; i < all.size(); ++i)
        sawConnected = sawConnected || static_cast<bool>(all[i]["connected"]);
    CHECK(sawConnected);
}
