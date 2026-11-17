#pragma once

// The TONE3000 bridge fixture shared by bridge_online_test.cpp and bridge_online_download_test.cpp: an Engine with a
// client id, a FakeHttp transport answering the sign-in endpoints, a forwarding in-memory secure store that survives
// Engine restarts, and a fake sign-in window hook.

#include <catch2/catch_test_macros.hpp>

#include <atomic>

#include "Engine.h"
#include "online/SecureStore.h"
#include "online/Tone3000Types.h"

#include "BridgeFixture.h"
#include "FakeHttp.h"

namespace amp_test
{
using amp::online::HttpRequest;
using amp::online::SecureStore;
using amp::Engine;

inline const juce::String api = "https://www.tone3000.com/api/v1";
inline const juce::String clientId = "t3k_pub_test";

inline juce::String t3kFixture(const char* name)
{
    return juce::File(AMP_FIXTURES_DIR).getChildFile("tone3000").getChildFile(name).loadFileAsString();
}

inline juce::String userJson() { return t3kFixture("user.json"); }

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
    std::atomic<int> tokenStatus { 200 }; // 400 = the token endpoint answers invalid_grant
    std::atomic<int> tokenCalls { 0 };    // requests to /oauth/token so far
    std::atomic<int> holdTokenCall { 0 }; // n > 0: the n-th token request waits until this changes (at most 5 s)
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
            h->on("POST", api + "/oauth/token", [this](const HttpRequest&)
                  {
                      const int call = ++tokenCalls;
                      const auto deadline = juce::Time::getMillisecondCounter() + 5000;
                      while (holdTokenCall.load() == call && juce::Time::getMillisecondCounter() < deadline)
                          juce::Thread::sleep(5);
                      if (tokenStatus.load() != 200)
                          return FakeHttp::json(tokenStatus.load(), R"({"error":"invalid_grant"})");
                      return FakeHttp::json(200, R"({"access_token":"A1","refresh_token":"R1","expires_in":3600})");
                  });
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

    // GET api + path answers the fixture file (on the current Engine's FakeHttp).
    void serve(const juce::String& path, const char* file)
    {
        http->on("GET", api + path,
                 [body = t3kFixture(file)](const HttpRequest&) { return FakeHttp::json(200, body); });
    }

    void serveTone()
    {
        serve("/tones/t_456", "tone.json");
        serve("/models?tone_id=t_456&architecture=2&page_size=300", "models_a2.json");
        serve("/models?tone_id=t_456&architecture=1&page_size=300", "models_a1.json");
    }

    // Pumps until the window has been opened `n` times.
    void waitForWindow(int n)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + 5000;
        while (window.opened < n && juce::Time::getMillisecondCounter() < deadline)
            pump(10);
        REQUIRE(window.opened == n);
    }

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

inline bool isFalse(const juce::var& v) { return v.isBool() && ! static_cast<bool>(v); }
} // namespace amp_test
