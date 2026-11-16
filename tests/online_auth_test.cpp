#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>

#include "support/FakeHttp.h"
#include "online/Pkce.h"
#include "online/Tone3000Auth.h"

using namespace amp::online;
using amp_test::FakeHttp;

namespace
{
const char* const tokenJson =
    R"({"access_token":"A1","refresh_token":"R1","token_type":"Bearer","expires_in":3600})";

struct AuthFixture
{
    FakeHttp http;
    MemorySecureStore store;
    std::atomic<juce::int64> clock { 1000000 };
    AuthConfig cfg;
    std::unique_ptr<Tone3000Auth> auth;

    AuthFixture()
    {
        cfg.clientId = "cid";
        auth = std::make_unique<Tone3000Auth>(cfg, http, store, [this] { return clock.load(); });
        http.on("POST", "https://www.tone3000.com/api/v1/oauth/token", [](const HttpRequest&) {
            return FakeHttp::json(200, tokenJson);
        });
    }

    static juce::StringPairArray form(const HttpRequest& r) { return pkce::parseQuery(r.body); }

    Tokens stored()
    {
        auto parsed = juce::JSON::parse(*store.get("tokens"));
        Tokens t;
        t.access = parsed["access"].toString();
        t.refresh = parsed["refresh"].toString();
        t.expiresAtMs = static_cast<juce::int64>(parsed["expiresAtMs"]);
        return t;
    }

    void signIn()
    {
        auto s = auth->begin();
        REQUIRE(auth->complete("code=c&state=" + s.state).signedIn);
    }
};
} // namespace

TEST_CASE("online_auth: begin builds the authorize url")
{
    AuthFixture f;
    juce::StringPairArray extra;
    extra.set("tone_id", "t9");
    auto start = f.auth->begin(extra);

    REQUIRE(start.url.startsWith("https://www.tone3000.com/api/v1/oauth/authorize?"));
    auto q = pkce::parseQuery(start.url);
    REQUIRE(q["response_type"] == "code");
    REQUIRE(q["client_id"] == "cid");
    REQUIRE(q["redirect_uri"] == "http://127.0.0.1/callback");
    REQUIRE(q["scope"] == "read");
    REQUIRE(q["code_challenge_method"] == "S256");
    REQUIRE(q["state"] == start.state);
    REQUIRE(q["tone_id"] == "t9");

    // The verifier is only observable through the exchange request.
    REQUIRE(f.auth->complete("code=c&state=" + start.state).signedIn);
    auto reqs = f.http.snapshot();
    REQUIRE(reqs.size() == 1);
    auto body = AuthFixture::form(reqs[0]);
    REQUIRE(pkce::challengeFor(body["code_verifier"]) == q["code_challenge"]);
    REQUIRE(body["grant_type"] == "authorization_code");
    REQUIRE(body["client_id"] == "cid");
    REQUIRE(body["redirect_uri"] == "http://127.0.0.1/callback");
    REQUIRE(reqs[0].headers.getValue("Content-Type", "") == "application/x-www-form-urlencoded");
}

TEST_CASE("online_auth: callback with wrong state is rejected")
{
    AuthFixture f;
    f.auth->begin();
    auto r = f.auth->complete("code=c&state=nope");
    REQUIRE_FALSE(r.stateOk);
    REQUIRE_FALSE(r.signedIn);
    REQUIRE(f.http.requestCount() == 0);

    auto none = Tone3000Auth(f.cfg, f.http, f.store).complete("code=c&state=x"); // no pending flow
    REQUIRE_FALSE(none.stateOk);
    REQUIRE(f.http.requestCount() == 0);
}

TEST_CASE("online_auth: callback parameters are decoded")
{
    AuthFixture f;
    auto s = f.auth->begin();
    auto r = f.auth->complete("tone_id=t%5F1&state=" + s.state + "&code=c%2Bd");
    REQUIRE(r.stateOk);
    REQUIRE(r.toneId == "t_1");
    REQUIRE(r.hadCode);
    auto reqs = f.http.snapshot();
    REQUIRE(reqs.size() == 1);
    REQUIRE(reqs[0].body.contains("code=c%2Bd"));
}

TEST_CASE("online_auth: canceled callback with a code still signs in")
{
    AuthFixture f;
    auto s = f.auth->begin();
    auto r = f.auth->complete("canceled=true&code=x&state=" + s.state);
    REQUIRE(r.canceled);
    REQUIRE(r.signedIn);
    REQUIRE(f.auth->isConnected());
}

TEST_CASE("online_auth: error and code-less callbacks make no request, and the flow is consumed")
{
    AuthFixture f;
    auto s = f.auth->begin();
    auto r = f.auth->complete("error=access_denied&state=" + s.state);
    REQUIRE(r.stateOk);
    REQUIRE(r.error == "access_denied");
    REQUIRE_FALSE(r.signedIn);
    REQUIRE(f.http.requestCount() == 0);
    REQUIRE_FALSE(f.auth->complete("code=c&state=" + s.state).stateOk); // consumed

    auto s2 = f.auth->begin();
    auto c = f.auth->complete("canceled=true&state=" + s2.state);
    REQUIRE(c.canceled);
    REQUIRE_FALSE(c.hadCode);
    REQUIRE(f.http.requestCount() == 0);
}

TEST_CASE("online_auth: a new begin replaces the pending flow")
{
    AuthFixture f;
    auto first = f.auth->begin();
    auto second = f.auth->begin();
    REQUIRE_FALSE(f.auth->complete("code=c&state=" + first.state).stateOk);
    REQUIRE(f.auth->complete("code=c&state=" + second.state).signedIn);
}

TEST_CASE("online_auth: exchange stores tokens with expiry")
{
    AuthFixture f;
    f.signIn();
    REQUIRE(f.auth->isConnected());
    auto t = f.stored();
    REQUIRE(t.access == "A1");
    REQUIRE(t.refresh == "R1");
    REQUIRE(t.expiresAtMs == 1000000 + 3600000);
}

TEST_CASE("online_auth: missing expires_in defaults to one hour")
{
    FakeHttp http;
    MemorySecureStore store;
    http.on("POST", "https://www.tone3000.com/api/v1/oauth/token", [](const HttpRequest&) {
        return FakeHttp::json(200, R"({"access_token":"A","refresh_token":"R"})");
    });
    Tone3000Auth auth({ "cid" }, http, store, [] { return juce::int64(5000); });
    auto s = auth.begin();
    REQUIRE(auth.complete("code=c&state=" + s.state).signedIn);
    REQUIRE(juce::JSON::parse(*store.get("tokens"))["expiresAtMs"] == juce::var(juce::int64(5000 + 3600000)));
}

TEST_CASE("online_auth: failed exchange reports an error and stays signed out")
{
    FakeHttp http;
    MemorySecureStore store;
    http.on("POST", "https://www.tone3000.com/", [](const HttpRequest&) {
        return FakeHttp::json(400, R"({"error":"invalid_request"})");
    });
    Tone3000Auth auth({ "cid" }, http, store);
    auto s = auth.begin();
    auto r = auth.complete("code=c&state=" + s.state);
    REQUIRE_FALSE(r.signedIn);
    REQUIRE(r.error == "invalid_request");
    REQUIRE_FALSE(auth.isConnected());
}

TEST_CASE("online_auth: accessToken returns the stored token while fresh")
{
    AuthFixture f;
    f.signIn();
    juce::String err;
    REQUIRE(f.auth->accessToken(err) == "A1");
    REQUIRE(err.isEmpty());
    REQUIRE(f.http.requestCount() == 1); // only the exchange

    juce::String err2;
    MemorySecureStore empty;
    REQUIRE(Tone3000Auth(f.cfg, f.http, empty).accessToken(err2).isEmpty());
    REQUIRE(err2.isNotEmpty());
}

TEST_CASE("online_auth: accessToken refreshes when under 60 s remain")
{
    FakeHttp http;
    MemorySecureStore store;
    store.set("tokens", R"({"access":"A1","refresh":"R1","expiresAtMs":4600000})");
    http.on("POST", "https://www.tone3000.com/api/v1/oauth/token", [](const HttpRequest&) {
        return FakeHttp::json(200, R"({"access_token":"A2","refresh_token":"R2","expires_in":7200})");
    });
    std::atomic<juce::int64> clock { 4600000 - 61000 };
    Tone3000Auth auth({ "cid" }, http, store, [&] { return clock.load(); });

    juce::String err;
    REQUIRE(auth.accessToken(err) == "A1"); // 61 s left: still fresh
    REQUIRE(http.requestCount() == 0);

    clock = 4600000 - 59000;
    REQUIRE(auth.accessToken(err) == "A2");
    REQUIRE(err.isEmpty());
    auto reqs = http.snapshot();
    REQUIRE(reqs.size() == 1);
    auto body = pkce::parseQuery(reqs[0].body);
    REQUIRE(body["grant_type"] == "refresh_token");
    REQUIRE(body["refresh_token"] == "R1");
    REQUIRE(body["client_id"] == "cid");
    auto stored = juce::JSON::parse(*store.get("tokens"));
    REQUIRE(stored["refresh"].toString() == "R2");
    REQUIRE(static_cast<juce::int64>(stored["expiresAtMs"]) == clock.load() + 7200000);

    REQUIRE(auth.accessToken(err) == "A2"); // fresh again
    REQUIRE(http.requestCount() == 1);
}

TEST_CASE("online_auth: refresh keeps the old refresh token when none is rotated")
{
    FakeHttp http;
    MemorySecureStore store;
    store.set("tokens", R"({"access":"A","refresh":"R","expiresAtMs":1000})");
    http.on("POST", "https://www.tone3000.com/", [](const HttpRequest&) {
        return FakeHttp::json(200, R"({"access_token":"A2","expires_in":3600})");
    });
    Tone3000Auth auth({ "cid" }, http, store, [] { return juce::int64(2000); });
    juce::String err;
    REQUIRE(auth.accessToken(err) == "A2");
    REQUIRE(juce::JSON::parse(*store.get("tokens"))["refresh"].toString() == "R");
}

namespace
{
// Holds a handler until the test opens the gate, so concurrent callers are certain to overlap.
struct Gate
{
    std::atomic<bool> open { false };
    std::atomic<int> entered { 0 };
    void wait()
    {
        ++entered;
        while (! open)
            juce::Thread::sleep(2);
    }
    void waitEntered(int n)
    {
        while (entered < n)
            juce::Thread::sleep(2);
    }
};

const char* const expiredTokens = R"({"access":"A","refresh":"R","expiresAtMs":1000})";
const char* const tokenUrl = "https://www.tone3000.com/api/v1/oauth/token";

// Spawns n callers and lets them all reach accessToken before the gate opens.
template <typename Fn>
void runConcurrently(int n, Gate& gate, Fn fn)
{
    std::vector<std::thread> threads;
    for (int i = 0; i < n; ++i)
        threads.emplace_back(fn, i);
    gate.waitEntered(1); // the first caller is inside the fake transport
    juce::Thread::sleep(80); // the others have had time to queue on the refresh lock
    gate.open = true;
    for (auto& t : threads)
        t.join();
}
} // namespace

TEST_CASE("online_auth: refresh is single-flight")
{
    FakeHttp http;
    MemorySecureStore store;
    Gate gate;
    store.set("tokens", expiredTokens);
    http.on("POST", tokenUrl, [&](const HttpRequest&) {
        gate.wait();
        return FakeHttp::json(200, R"({"access_token":"A2","refresh_token":"R2","expires_in":3600})");
    });
    Tone3000Auth auth({ "cid" }, http, store, [] { return juce::int64(2000); });

    juce::String e[2], t[2];
    runConcurrently(2, gate, [&](int i) { t[i] = auth.accessToken(e[i]); });
    REQUIRE(t[0] == "A2");
    REQUIRE(t[1] == "A2");
    REQUIRE(http.requestCount() == 1);
}

TEST_CASE("online_auth: forced refresh refreshes with plenty of life left")
{
    FakeHttp http;
    MemorySecureStore store;
    store.set("tokens", R"({"access":"A","refresh":"R","expiresAtMs":3601000})");
    http.on("POST", tokenUrl, [](const HttpRequest&) {
        return FakeHttp::json(200, R"({"access_token":"A2","refresh_token":"R2","expires_in":3600})");
    });
    Tone3000Auth auth({ "cid" }, http, store, [] { return juce::int64(1000); });
    juce::String err;
    REQUIRE(auth.accessToken(err) == "A");
    REQUIRE(http.requestCount() == 0);
    REQUIRE(auth.accessToken(err, true) == "A2");
    REQUIRE(http.requestCount() == 1);
}

TEST_CASE("online_auth: concurrent forced refreshes are single-flight")
{
    FakeHttp http;
    MemorySecureStore store;
    Gate gate;
    store.set("tokens", R"({"access":"A","refresh":"R","expiresAtMs":3601000})");
    http.on("POST", tokenUrl, [&](const HttpRequest&) {
        gate.wait();
        return FakeHttp::json(200, R"({"access_token":"A2","refresh_token":"R2","expires_in":3600})");
    });
    Tone3000Auth auth({ "cid" }, http, store, [] { return juce::int64(1000); });
    juce::String e[2], t[2];
    runConcurrently(2, gate, [&](int i) { t[i] = auth.accessToken(e[i], true); });
    REQUIRE(t[0] == "A2");
    REQUIRE(t[1] == "A2");
    REQUIRE(http.requestCount() == 1);
}

TEST_CASE("online_auth: concurrent invalid_grant sends one request and signs out once")
{
    FakeHttp http;
    MemorySecureStore store;
    Gate gate;
    store.set("tokens", expiredTokens);
    http.on("POST", tokenUrl, [&](const HttpRequest&) {
        gate.wait();
        return FakeHttp::json(400, R"({"error":"invalid_grant"})");
    });
    Tone3000Auth auth({ "cid" }, http, store, [] { return juce::int64(2000); });
    std::atomic<int> signedOut { 0 };
    auth.onSignedOut = [&] { ++signedOut; };

    juce::String e[2], t[2];
    runConcurrently(2, gate, [&](int i) { t[i] = auth.accessToken(e[i]); });
    REQUIRE(t[0].isEmpty());
    REQUIRE(t[1].isEmpty());
    REQUIRE(e[0] == "signed out");
    REQUIRE(e[1] == "signed out");
    REQUIRE(http.requestCount() == 1);
    REQUIRE(signedOut == 1);
    REQUIRE_FALSE(auth.isConnected());
}

TEST_CASE("online_auth: a refresh finishing after sign-out does not resurrect the tokens")
{
    FakeHttp http;
    MemorySecureStore store;
    Gate gate;
    store.set("tokens", expiredTokens);
    http.on("POST", tokenUrl, [&](const HttpRequest&) {
        gate.wait();
        return FakeHttp::json(200, R"({"access_token":"A2","refresh_token":"R2","expires_in":3600})");
    });
    Tone3000Auth auth({ "cid" }, http, store, [] { return juce::int64(2000); });
    juce::String err, token;
    std::thread worker([&] { token = auth.accessToken(err); });
    gate.waitEntered(1);
    auth.clear();
    gate.open = true;
    worker.join();
    REQUIRE(token.isEmpty());
    REQUIRE_FALSE(auth.isConnected());
}

TEST_CASE("online_auth: a refresh success does not overwrite a newer sign-in")
{
    FakeHttp http;
    MemorySecureStore store;
    Gate gate;
    store.set("tokens", expiredTokens);
    http.on("POST", tokenUrl, [&](const HttpRequest& r) {
        if (pkce::parseQuery(r.body)["grant_type"] == "refresh_token")
        {
            gate.wait();
            return FakeHttp::json(200, R"({"access_token":"OLD2","refresh_token":"OLDR2","expires_in":3600})");
        }
        return FakeHttp::json(200, R"({"access_token":"NEW","refresh_token":"NEWR","expires_in":3600})");
    });
    Tone3000Auth auth({ "cid" }, http, store, [] { return juce::int64(2000); });
    juce::String err, token;
    std::thread worker([&] { token = auth.accessToken(err); });
    gate.waitEntered(1);
    auth.clear();
    auto s = auth.begin();
    REQUIRE(auth.complete("code=c&state=" + s.state).signedIn);
    gate.open = true;
    worker.join();
    REQUIRE(juce::JSON::parse(*store.get("tokens"))["access"].toString() == "NEW");
    REQUIRE(token == "NEW");
}

TEST_CASE("online_auth: a refresh revoked after a newer sign-in leaves the new tokens alone")
{
    FakeHttp http;
    MemorySecureStore store;
    Gate gate;
    store.set("tokens", expiredTokens);
    http.on("POST", tokenUrl, [&](const HttpRequest& r) {
        if (pkce::parseQuery(r.body)["grant_type"] == "refresh_token")
        {
            gate.wait();
            return FakeHttp::json(400, R"({"error":"invalid_grant"})");
        }
        return FakeHttp::json(200, R"({"access_token":"NEW","refresh_token":"NEWR","expires_in":3600})");
    });
    Tone3000Auth auth({ "cid" }, http, store, [] { return juce::int64(2000); });
    std::atomic<int> signedOut { 0 };
    auth.onSignedOut = [&] { ++signedOut; };
    juce::String err, token;
    std::thread worker([&] { token = auth.accessToken(err); });
    gate.waitEntered(1);
    auth.clear(); // signedOut == 1
    auto s = auth.begin();
    REQUIRE(auth.complete("code=c&state=" + s.state).signedIn);
    gate.open = true;
    worker.join();
    REQUIRE(auth.isConnected());
    REQUIRE(juce::JSON::parse(*store.get("tokens"))["access"].toString() == "NEW");
    REQUIRE(signedOut == 1); // only the explicit clear()
}

TEST_CASE("online_auth: begin ignores extra params that collide with the OAuth ones")
{
    AuthFixture f;
    juce::StringPairArray extra;
    extra.set("client_id", "evil");
    extra.set("state", "forged");
    extra.set("tone_id", "t1");
    auto start = f.auth->begin(extra);
    auto q = pkce::parseQuery(start.url);
    REQUIRE(q["client_id"] == "cid");
    REQUIRE(q["state"] == start.state);
    REQUIRE(q["tone_id"] == "t1");
}

TEST_CASE("online_auth: invalid_grant signs out")
{
    FakeHttp http;
    MemorySecureStore store;
    store.set("tokens", R"({"access":"A","refresh":"R","expiresAtMs":1000})");
    http.on("POST", "https://www.tone3000.com/", [](const HttpRequest&) {
        return FakeHttp::json(400, R"({"error":"invalid_grant"})");
    });
    Tone3000Auth auth({ "cid" }, http, store, [] { return juce::int64(2000); });
    int signedOut = 0;
    auth.onSignedOut = [&] { ++signedOut; };

    juce::String err;
    REQUIRE(auth.accessToken(err).isEmpty());
    REQUIRE(err == "signed out");
    REQUIRE_FALSE(auth.isConnected());
    REQUIRE(signedOut == 1);
}

TEST_CASE("online_auth: network failure keeps tokens")
{
    FakeHttp http; // no route: transport error
    MemorySecureStore store;
    store.set("tokens", R"({"access":"A","refresh":"R","expiresAtMs":1000})");
    Tone3000Auth auth({ "cid" }, http, store, [] { return juce::int64(2000); });
    int signedOut = 0;
    auth.onSignedOut = [&] { ++signedOut; };

    juce::String err;
    REQUIRE(auth.accessToken(err).isEmpty());
    REQUIRE(err.isNotEmpty());
    REQUIRE(err != "signed out");
    REQUIRE(auth.isConnected());
    REQUIRE(signedOut == 0);
}

TEST_CASE("online_auth: a 5xx refresh keeps tokens")
{
    FakeHttp http;
    MemorySecureStore store;
    store.set("tokens", R"({"access":"A","refresh":"R","expiresAtMs":1000})");
    http.on("POST", "https://www.tone3000.com/", [](const HttpRequest&) { return FakeHttp::json(503, "{}"); });
    Tone3000Auth auth({ "cid" }, http, store, [] { return juce::int64(2000); });
    juce::String err;
    REQUIRE(auth.accessToken(err).isEmpty());
    REQUIRE(auth.isConnected());
}

TEST_CASE("online_auth: clear fires onSignedOut only when connected")
{
    AuthFixture f;
    int signedOut = 0;
    f.auth->onSignedOut = [&] { ++signedOut; };
    f.auth->clear();
    REQUIRE(signedOut == 0);
    f.signIn();
    f.auth->clear();
    REQUIRE_FALSE(f.auth->isConnected());
    REQUIRE(signedOut == 1);
    f.auth->clear();
    REQUIRE(signedOut == 1);
}
