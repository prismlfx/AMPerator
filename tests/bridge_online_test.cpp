#include <catch2/catch_test_macros.hpp>

#include <atomic>

#include "Engine.h"
#include "online/SecureStore.h"

#include "support/BridgeFixture.h"
#include "support/FakeHttp.h"

using namespace amp;
using namespace amp_test;
using amp::online::HttpRequest;
using amp::online::SecureStore;

namespace
{
const juce::String api = "https://www.tone3000.com/api/v1";
const juce::String clientId = "t3k_pub_test";

juce::String userJson()
{
    return juce::File(AMP_FIXTURES_DIR).getChildFile("tone3000").getChildFile("user.json").loadFileAsString();
}

// The Engine's store: forwards to a store the fixture owns, so tokens survive Engine restarts. Counts accesses.
class ForwardingStore final : public SecureStore
{
public:
    ForwardingStore(SecureStore& t, std::atomic<int>& n) : target(t), accesses(n) {}
    bool set(const juce::String& k, const juce::String& v) override { ++accesses; return target.set(k, v); }
    std::optional<juce::String> get(const juce::String& k) override { ++accesses; return target.get(k); }
    bool remove(const juce::String& k) override { ++accesses; return target.remove(k); }

private:
    SecureStore& target;
    std::atomic<int>& accesses;
};

// A platform store that refuses every write (e.g. Keychain access denied).
class RefusingStore final : public SecureStore
{
public:
    bool set(const juce::String&, const juce::String&) override { return false; }
    std::optional<juce::String> get(const juce::String&) override { return std::nullopt; }
    bool remove(const juce::String&) override { return true; }
};

struct OnlineFixture : Fixture
{
    amp::online::MemorySecureStore backing; // outlives Engine restarts
    std::atomic<int> storeAccesses { 0 };
    FakeHttp* http = nullptr; // owned by the Engine; replaced on every start()
    bool refuseWrites = false;

    struct Window
    {
        int opened = 0;
        juce::String url, redirectPrefix;
        std::function<void(const juce::String&)> done;
    } window;
    int cookiesCleared = 0;

    explicit OnlineFixture(juce::String id = clientId, bool refuse = false) : Fixture(false), refuseWrites(refuse)
    {
        configureOptions = [this, id](Engine::Options& o)
        {
            auto h = std::make_unique<FakeHttp>();
            http = h.get();
            h->on("POST", api + "/oauth/token", [](const HttpRequest&)
                  { return FakeHttp::json(200, R"({"access_token":"A1","refresh_token":"R1","expires_in":3600})"); });
            h->on("GET", api + "/user", [](const HttpRequest&) { return FakeHttp::json(200, userJson()); });
            o.httpTransport = std::move(h);
            if (refuseWrites)
                o.secureStore = std::make_unique<RefusingStore>();
            else
                o.secureStore = std::make_unique<ForwardingStore>(backing, storeAccesses);
            o.tone3000ClientId = id;
        };
        configureEngine = [this](Engine& e)
        {
            e.onOpenAuthWindow = [this](const juce::String& url, const juce::String& prefix,
                                        std::function<void(const juce::String&)> done)
            {
                ++window.opened;
                window.url = url;
                window.redirectPrefix = prefix;
                window.done = std::move(done);
            };
            e.onClearAuthCookies = [this] { ++cookiesCleared; };
        };
        start();
    }

    ~OnlineFixture() { engine.reset(); } // before `backing` goes

    juce::var status() { return ok("tone3000Status"); }

    // A query parameter of the authorize URL the window was opened with.
    juce::String urlParam(const juce::String& name) const
    {
        const juce::URL u(window.url);
        const int i = u.getParameterNames().indexOf(name);
        return i < 0 ? juce::String() : u.getParameterValues()[i];
    }

    // Starts tone3000Connect and checks the window opened; returns the command id.
    int startConnect()
    {
        const int opened = window.opened;
        const int id = post("tone3000Connect");
        REQUIRE(window.opened == opened + 1);
        return id;
    }

    // Connects with a valid callback; returns the reply's result.
    juce::var connect()
    {
        const int id = startConnect();
        window.done("code=c&state=" + urlParam("state"));
        const auto r = waitReply(id);
        INFO(juce::JSON::toString(r, true));
        REQUIRE(static_cast<bool>(r["ok"]));
        return r["result"];
    }
};

bool isFalse(const juce::var& v) { return v.isBool() && ! static_cast<bool>(v); }
} // namespace

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

    for (auto* cmd : { "tone3000Connect", "tone3000Disconnect" })
    {
        const auto r = f.call(cmd);
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
