#include <catch2/catch_test_macros.hpp>

#include <atomic>

#include "support/FakeHttp.h"
#include "online/Tone3000Client.h"

using namespace amp::online;
using amp_test::FakeHttp;

namespace
{
const juce::String api = "https://www.tone3000.com/api/v1";

juce::String fixture(const char* name)
{
    return juce::File(AMP_FIXTURES_DIR).getChildFile("tone3000").getChildFile(name).loadFileAsString();
}

juce::var fixtureVar(const char* name) { return juce::JSON::parse(fixture(name)); }

struct ClientFixture
{
    FakeHttp http;
    MemorySecureStore store;
    std::atomic<juce::int64> clock { 1000000 };
    std::unique_ptr<Tone3000Auth> auth;
    std::unique_ptr<Tone3000Client> client;

    explicit ClientFixture(bool signedIn = true)
    {
        AuthConfig cfg;
        cfg.clientId = "cid";
        auth = std::make_unique<Tone3000Auth>(cfg, http, store, [this] { return clock.load(); });
        if (signedIn)
            store.set("tokens", R"({"access":"AT","refresh":"RT","expiresAtMs":99000000})");
        client = std::make_unique<Tone3000Client>(*auth, http, "https://www.tone3000.com");
    }

    void serve(const juce::String& path, const char* file)
    {
        http.on("GET", api + path, [body = fixture(file)](const HttpRequest&) { return FakeHttp::json(200, body); });
    }
};

juce::String header(const HttpRequest& r, const char* name) { return r.headers[name]; }
} // namespace

TEST_CASE("online_client: parses the user fixture")
{
    auto u = parseUser(fixtureVar("user.json"));
    REQUIRE(u.id == "42"); // numeric ids become strings without a decimal point
    REQUIRE(u.username == "jane");
    REQUIRE(u.displayName == "Jane Test");
    REQUIRE(u.avatarUrl == "https://example.test/avatars/jane.png");
    REQUIRE(u.url == "https://www.tone3000.com/jane");
}

TEST_CASE("online_client: parses a tone field by field")
{
    auto t = parseTone(fixtureVar("tone.json"));
    REQUIRE(t.id == "t_456");
    REQUIRE(t.title == "Fake Crunch Rig");
    REQUIRE(t.gear == "amp");
    REQUIRE(t.format == "nam");
    REQUIRE(t.license == "cc-by");
    REQUIRE(t.image == "https://example.test/img/t_456_a.jpg"); // images[0]
    REQUIRE(t.images.size() == 2);
    REQUIRE(t.url == "https://www.tone3000.com/tones/t_456");
    REQUIRE(t.creator.id == "u_1");
    REQUIRE(t.creator.username == "jane");
    REQUIRE(t.creator.displayName == ""); // missing display_name
    REQUIRE(t.modelsCount == 3);
    REQUIRE(t.downloadsCount == 120);
    REQUIRE(t.favoritesCount == 9);
    REQUIRE(t.description == "A made-up tone for tests.");
    REQUIRE(t.publishedAt == "2026-01-02T03:04:05Z");
    REQUIRE(t.makes == juce::StringArray({ "Fakemarshall", "Fenderish" }));
    REQUIRE(t.tags == juce::StringArray({ "crunch", "rock" }));
}

TEST_CASE("online_client: parses models including null and numeric architecture")
{
    auto a2 = fixtureVar("models_a2.json")["data"];
    auto m = parseModel(a2[0]);
    REQUIRE(m.id == "m_201");
    REQUIRE(m.toneId == "t_456");
    REQUIRE(m.name == "Crunch A2 Standard");
    REQUIRE(m.size == "standard");
    REQUIRE(m.architecture == "2");
    REQUIRE(m.modelUrl == "https://www.tone3000.com/api/v1/models/m_201/download");
    REQUIRE(parseModel(a2[1]).architecture == "custom");

    REQUIRE(parseModel(fixtureVar("models_a1.json")["data"][0]).architecture == "1");
    REQUIRE(parseModel(fixtureVar("models_a1.json")["data"][1]).architecture == "1");

    auto ir = parseModel(fixtureVar("models_ir.json")["data"][0]);
    REQUIRE(ir.id == "m_301");
    REQUIRE(ir.architecture == ""); // architecture_version: null
    REQUIRE(ir.size == "");

    REQUIRE(parseModel(fixtureVar("model.json")).id == "m_123");
}

TEST_CASE("online_client: parses summaries with numeric ids and no images")
{
    auto s = parseToneSummary(fixtureVar("list_favorited.json")["data"][1]);
    REQUIRE(s.id == "77");
    REQUIRE(s.image == "");
    REQUIRE(s.creator.username == "bob");
    REQUIRE(s.license == "t3k");
}

TEST_CASE("online_client: toVar shapes match the bridge spec")
{
    auto tone = parseTone(fixtureVar("tone.json"));
    ModelInfo a2;
    a2.id = "m_201";
    a2.name = "Crunch A2 Standard";
    a2.size = "standard";
    a2.architecture = "2";
    a2.modelUrl = "https://x.test/never-exposed";
    tone.models.push_back(a2);

    auto user = toVar(tone.creator);
    juce::StringArray userKeys;
    for (const auto& p : user.getDynamicObject()->getProperties())
        userKeys.add(p.name.toString());
    userKeys.sort(false);
    REQUIRE(userKeys == juce::StringArray({ "avatarUrl", "displayName", "url", "username" }));

    auto summary = toVar(static_cast<const ToneSummary&>(tone));
    auto full = toVar(tone);
    for (const auto* key : { "id", "title", "gear", "format", "license", "creator", "image", "url", "modelsCount",
                             "downloadsCount", "favoritesCount" })
    {
        REQUIRE(summary.hasProperty(key));
        REQUIRE(full.hasProperty(key));
    }
    REQUIRE(summary.getDynamicObject()->getProperties().size() == 11);
    REQUIRE(full.getDynamicObject()->getProperties().size() == 17);
    REQUIRE(full["id"].toString() == "t_456");
    REQUIRE(full["modelsCount"].operator int() == 3);
    REQUIRE(full["creator"]["username"].toString() == "jane");
    REQUIRE(full["description"].toString() == "A made-up tone for tests.");
    REQUIRE(full["makes"].size() == 2);
    REQUIRE(full["tags"][0].toString() == "crunch");
    REQUIRE(full["images"].size() == 2);
    REQUIRE(full["publishedAt"].toString() == "2026-01-02T03:04:05Z");
    REQUIRE(full["models"].size() == 1);
    auto model = full["models"][0];
    REQUIRE(model.getDynamicObject()->getProperties().size() == 4);
    REQUIRE(model["id"].toString() == "m_201");
    REQUIRE(model["name"].toString() == "Crunch A2 Standard");
    REQUIRE(model["size"].toString() == "standard");
    REQUIRE(model["architecture"].toString() == "2");
}

TEST_CASE("online_client: user() sends the Bearer token")
{
    ClientFixture f;
    f.serve("/user", "user.json");
    auto r = f.client->user();
    REQUIRE_FALSE(r.error);
    REQUIRE(r.value->username == "jane");
    auto reqs = f.http.snapshot();
    REQUIRE(reqs.size() == 1);
    REQUIRE(reqs[0].url == api + "/user");
    REQUIRE(header(reqs[0], "Authorization") == "Bearer AT");
    REQUIRE(header(reqs[0], "Accept") == "application/json");
}

TEST_CASE("online_client: not connected without tokens, no request is made")
{
    ClientFixture f(false);
    auto r = f.client->user();
    REQUIRE(r.error.kind == ClientError::Kind::NotConnected);
    REQUIRE_FALSE(r.value.has_value());
    REQUIRE(f.http.requestCount() == 0);
}

TEST_CASE("online_client: list favorited requests the page with page_size 25")
{
    ClientFixture f;
    f.serve("/tones/favorited", "list_favorited.json");
    auto r = f.client->list("favorited", 2);
    REQUIRE_FALSE(r.error);
    REQUIRE(r.value->page == 2);
    REQUIRE(r.value->totalPages == 2);
    REQUIRE(r.value->tones.size() == 2);
    REQUIRE(r.value->tones[0].title == "Fake Crunch Rig");
    auto reqs = f.http.snapshot();
    REQUIRE(reqs[0].url == api + "/tones/favorited?page=2&page_size=25");
    REQUIRE(header(reqs[0], "Authorization") == "Bearer AT");
}

TEST_CASE("online_client: list trending is page 1 of 1 and unpaged")
{
    ClientFixture f;
    f.serve("/tones/trending", "list_trending.json");
    auto r = f.client->list("trending", 5);
    REQUIRE_FALSE(r.error);
    REQUIRE(r.value->page == 1);
    REQUIRE(r.value->totalPages == 1);
    REQUIRE(r.value->tones.size() == 1);
    REQUIRE(r.value->tones[0].creator.displayName == "Carol Test");
    REQUIRE(f.http.snapshot()[0].url == api + "/tones/trending");
}

TEST_CASE("online_client: unknown list kinds never reach the network")
{
    ClientFixture f;
    REQUIRE(f.client->list("search", 1).error.kind == ClientError::Kind::Http);
    REQUIRE(f.http.requestCount() == 0);
}

TEST_CASE("online_client: tone() makes three requests, A2 before A1, custom dropped")
{
    ClientFixture f;
    f.serve("/models?tone_id=t_456&architecture=2&page_size=300", "models_a2.json");
    f.serve("/models?tone_id=t_456&architecture=1&page_size=300", "models_a1.json");
    f.serve("/tones/t_456", "tone.json");

    auto r = f.client->tone("t_456");
    REQUIRE_FALSE(r.error);
    auto reqs = f.http.snapshot();
    REQUIRE(reqs.size() == 3);
    REQUIRE(reqs[0].url == api + "/tones/t_456");
    REQUIRE(reqs[1].url == api + "/models?tone_id=t_456&architecture=2&page_size=300");
    REQUIRE(reqs[2].url == api + "/models?tone_id=t_456&architecture=1&page_size=300");

    REQUIRE(r.value->title == "Fake Crunch Rig");
    REQUIRE(r.value->models.size() == 3);
    REQUIRE(r.value->models[0].id == "m_201");
    REQUIRE(r.value->models[1].id == "m_123");
    REQUIRE(r.value->models[2].id == "m_124");
    for (const auto& m : r.value->models)
        REQUIRE(m.architecture != "custom");
}

TEST_CASE("online_client: tone() surfaces a failing models request")
{
    ClientFixture f;
    f.serve("/tones/t_456", "tone.json");
    f.http.on("GET", api + "/models", [](const HttpRequest&) { return FakeHttp::json(500, "oops"); });
    auto r = f.client->tone("t_456");
    REQUIRE(r.error.kind == ClientError::Kind::Http);
    REQUIRE(r.error.message.contains("500"));
    REQUIRE_FALSE(r.value.has_value());
}

TEST_CASE("online_client: model() parses a single model")
{
    ClientFixture f;
    f.serve("/models/m_123", "model.json");
    auto r = f.client->model("m_123");
    REQUIRE_FALSE(r.error);
    REQUIRE(r.value->modelUrl == "https://www.tone3000.com/api/v1/models/m_123/download");
}

const char* const refreshedTokens =
    R"({"access_token":"NEW","refresh_token":"RT2","token_type":"Bearer","expires_in":3600})";

size_t refreshRequests(const FakeHttp& http)
{
    size_t n = 0;
    for (const auto& r : http.snapshot())
        if (r.url.endsWith("/oauth/token"))
            ++n;
    return n;
}

TEST_CASE("online_client: 401 forces one refresh and retries once with the new token")
{
    ClientFixture f; // the stored token has an hour left, so only a forced refresh can replace it
    f.http.on("POST", api + "/oauth/token", [](const HttpRequest&) { return FakeHttp::json(200, refreshedTokens); });
    auto calls = std::make_shared<std::atomic<int>>(0);
    f.http.on("GET", api + "/user", [calls, body = fixture("user.json")](const HttpRequest&) {
        return ++*calls == 1 ? FakeHttp::json(401, "{}") : FakeHttp::json(200, body);
    });
    auto r = f.client->user();
    REQUIRE_FALSE(r.error);
    REQUIRE(refreshRequests(f.http) == 1);

    std::vector<HttpRequest> gets;
    for (const auto& req : f.http.snapshot())
        if (req.url == api + "/user")
            gets.push_back(req);
    REQUIRE(gets.size() == 2);
    REQUIRE(header(gets[0], "Authorization") == "Bearer AT");
    REQUIRE(header(gets[1], "Authorization") == "Bearer NEW"); // re-sent, with the refreshed token
}

TEST_CASE("online_client: a second 401 after the forced refresh signs out, not a loop")
{
    ClientFixture f;
    int signedOut = 0;
    f.auth->onSignedOut = [&] { ++signedOut; };
    f.http.on("POST", api + "/oauth/token", [](const HttpRequest&) { return FakeHttp::json(200, refreshedTokens); });
    f.http.on("GET", api + "/user", [](const HttpRequest&) { return FakeHttp::json(401, "{}"); });
    auto r = f.client->user();
    REQUIRE(r.error.kind == ClientError::Kind::NotConnected);
    REQUIRE(refreshRequests(f.http) == 1);
    REQUIRE(f.http.requestCount() == 3); // GET, refresh, GET
    REQUIRE(signedOut == 1);
    REQUIRE_FALSE(f.auth->isConnected());
    REQUIRE_FALSE(f.store.get("tokens").has_value());
}

TEST_CASE("online_client: 429 reports Retry-After and is not retried")
{
    ClientFixture f;
    juce::StringPairArray headers;
    headers.set("Retry-After", "7");
    f.http.on("GET", api + "/user", [headers](const HttpRequest&) { return FakeHttp::json(429, "{}", headers); });
    auto r = f.client->user();
    REQUIRE(r.error.kind == ClientError::Kind::RateLimited);
    REQUIRE(r.error.retryAfterSec == 7);
    REQUIRE(f.http.requestCount() == 1);
}

TEST_CASE("online_client: status mapping")
{
    struct Case
    {
        int status;
        ClientError::Kind kind;
    };
    for (const auto& c : { Case { 404, ClientError::Kind::NotFound }, Case { 403, ClientError::Kind::Forbidden },
                           Case { 500, ClientError::Kind::Http } })
    {
        ClientFixture f;
        f.http.on("GET", api + "/user", [c](const HttpRequest&) { return FakeHttp::json(c.status, "{}"); });
        REQUIRE(f.client->user().error.kind == c.kind);
    }
}

TEST_CASE("online_client: transport failure is Network and bad JSON is Parse")
{
    {
        ClientFixture f; // no route: status 0 with an error
        REQUIRE(f.client->user().error.kind == ClientError::Kind::Network);
    }
    {
        ClientFixture f;
        f.http.on("GET", api + "/user", [](const HttpRequest&) { return FakeHttp::json(200, "<html>"); });
        REQUIRE(f.client->user().error.kind == ClientError::Kind::Parse);
    }
}

TEST_CASE("online_client: the deprecation header fires onDeprecation")
{
    ClientFixture f;
    juce::StringPairArray headers;
    headers.set("x-tone3000-deprecations", "GET /tones/search sunset 2027-01-01");
    f.http.on("GET", api + "/user", [headers, body = fixture("user.json")](const HttpRequest&) {
        return FakeHttp::json(200, body, headers);
    });
    juce::StringArray seen;
    f.client->onDeprecation = [&](const juce::String& s) { seen.add(s); };
    REQUIRE_FALSE(f.client->user().error);
    REQUIRE(seen == juce::StringArray({ "GET /tones/search sunset 2027-01-01" }));
}

TEST_CASE("online_client: fetch streams to the file with the Bearer header")
{
    ClientFixture f;
    juce::MemoryBlock bytes("namdata", 7);
    f.http.on("GET", "https://www.tone3000.com/api/v1/models/m_123/download",
              [bytes](const HttpRequest&) { return FakeHttp::file(200, bytes); });

    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("amp_client_test_" + juce::String(juce::Random().nextInt64()));
    dir.createDirectory();
    auto out = dir.getChildFile("m.nam");
    auto err = f.client->fetch("https://www.tone3000.com/api/v1/models/m_123/download", out,
                               [](juce::int64, juce::int64) {}, [] { return false; });
    REQUIRE_FALSE(err);
    REQUIRE(out.getSize() == 7);
    auto reqs = f.http.snapshot();
    REQUIRE(header(reqs[0], "Authorization") == "Bearer AT");
    REQUIRE(reqs[0].toFile == out);
    REQUIRE(reqs[0].onProgress);
    REQUIRE(reqs[0].shouldCancel);
    REQUIRE(reqs[0].timeoutMs == 20000); // an idle timeout: a stalled transfer ends within 20 s
    dir.deleteRecursively();
}

TEST_CASE("online_client: fetch never sends the token to another host and drops error bodies")
{
    ClientFixture f;
    f.http.on("GET", "https://cdn.example.test/", [](const HttpRequest&) { return FakeHttp::json(403, "denied"); });
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("amp_client_test_" + juce::String(juce::Random().nextInt64()));
    dir.createDirectory();
    auto out = dir.getChildFile("m.nam");
    out.replaceWithText("stale");
    auto err = f.client->fetch("https://cdn.example.test/m.nam", out, {}, {});
    REQUIRE(err.kind == ClientError::Kind::Forbidden);
    REQUIRE(header(f.http.snapshot()[0], "Authorization") == "");
    REQUIRE_FALSE(out.existsAsFile());
    dir.deleteRecursively();
}

TEST_CASE("online_client: an IR tone makes one unfiltered models request and keeps every model")
{
    ClientFixture f;
    f.serve("/tones/t_789", "tone_ir.json");
    f.serve("/models?tone_id=t_789&page_size=300", "models_ir.json");
    auto r = f.client->tone("t_789");
    REQUIRE_FALSE(r.error);
    auto reqs = f.http.snapshot();
    REQUIRE(reqs.size() == 2);
    REQUIRE(reqs[1].url == api + "/models?tone_id=t_789&page_size=300");
    REQUIRE(r.value->format == "ir");
    REQUIRE(r.value->models.size() == 1);
    REQUIRE(r.value->models[0].id == "m_301");
    REQUIRE(r.value->models[0].architecture == "");
}

TEST_CASE("online_client: fetch sends the token only to the same scheme, host and port")
{
    const juce::String bad[] = { "https://www.tone3000.com:8443/api/v1/models/m_1/download",
                                 "https://tone3000.com.evil.test/m.nam" };
    for (const auto& url : bad)
    {
        ClientFixture f;
        f.http.on("GET", "", [](const HttpRequest&) { return FakeHttp::json(200, "x"); });
        auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                       .getChildFile("amp_client_test_" + juce::String(juce::Random().nextInt64()));
        dir.createDirectory();
        f.client->fetch(url, dir.getChildFile("m.nam"), {}, {});
        auto reqs = f.http.snapshot();
        REQUIRE(reqs.size() == 1);
        REQUIRE(header(reqs[0], "Authorization") == "");
        dir.deleteRecursively();
    }

    ClientFixture f; // explicit default port on the same origin still counts as ours
    f.http.on("GET", "", [](const HttpRequest&) { return FakeHttp::json(200, "x"); });
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile("amp_client_test_" + juce::String(juce::Random().nextInt64()));
    dir.createDirectory();
    f.client->fetch("https://www.tone3000.com:443/api/v1/models/m_1/download", dir.getChildFile("m.nam"), {}, {});
    REQUIRE(header(f.http.snapshot()[0], "Authorization") == "Bearer AT");
    dir.deleteRecursively();
}

TEST_CASE("online_client: fetch reports a 429 with Retry-After and does not retry")
{
    ClientFixture f;
    juce::StringPairArray headers;
    headers.set("Retry-After", "11");
    f.http.on("GET", api + "/models/", [headers](const HttpRequest&) { return FakeHttp::json(429, "slow", headers); });
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile("amp_client_test_" + juce::String(juce::Random().nextInt64()));
    dir.createDirectory();
    auto err = f.client->fetch(api + "/models/m_1/download", dir.getChildFile("m.nam"), {}, {});
    REQUIRE(err.kind == ClientError::Kind::RateLimited);
    REQUIRE(err.retryAfterSec == 11);
    REQUIRE(f.http.requestCount() == 1);
    dir.deleteRecursively();
}

TEST_CASE("online_client: fetch never sends the token through userinfo or cleartext http")
{
    // JUCE's URL parser reads the first of these as host www.tone3000.com, port 443, but the connection goes to
    // evil.example; the same parser as the transport decides here.
    const juce::String hostile[] = { "https://www.tone3000.com:443@evil.example/x", "https://user:pass@www.tone3000.com/x",
                                     "http://www.tone3000.com/x" };
    for (const auto& url : hostile)
    {
        INFO(url);
        ClientFixture f;
        f.http.on("GET", "", [](const HttpRequest&) { return FakeHttp::json(200, "x"); });
        auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                       .getChildFile("amp_client_test_" + juce::String(juce::Random().nextInt64()));
        dir.createDirectory();
        const auto err = f.client->fetch(url, dir.getChildFile("m.nam"), {}, {});
        REQUIRE(err); // refused: never fetched as if it were ours
        for (const auto& req : f.http.snapshot())
            REQUIRE(header(req, "Authorization") == "");
        dir.deleteRecursively();
    }
}

TEST_CASE("online_client: fetch to an http URL is allowed when the base itself is http")
{
    FakeHttp http;
    MemorySecureStore store;
    store.set("tokens", R"({"access":"AT","refresh":"RT","expiresAtMs":99000000})");
    AuthConfig cfg;
    cfg.clientId = "cid";
    cfg.baseUrl = "http://127.0.0.1:8080";
    Tone3000Auth auth(cfg, http, store, [] { return (juce::int64) 1000000; });
    Tone3000Client client(auth, http, "http://127.0.0.1:8080");
    http.on("GET", "", [](const HttpRequest&) { return FakeHttp::json(200, "x"); });
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile("amp_client_test_" + juce::String(juce::Random().nextInt64()));
    dir.createDirectory();
    REQUIRE_FALSE(client.fetch("http://127.0.0.1:8080/m.nam", dir.getChildFile("a.nam"), {}, {}));
    REQUIRE_FALSE(client.fetch("http://cdn.example.test/m.jpg", dir.getChildFile("b.jpg"), {}, {}));
    const auto reqs = http.snapshot();
    REQUIRE(reqs.size() == 2);
    REQUIRE(header(reqs[0], "Authorization") == "Bearer AT");
    REQUIRE(header(reqs[1], "Authorization") == "");
    dir.deleteRecursively();
}

TEST_CASE("online_client: model() and tone() pass the cancel callback to their requests")
{
    ClientFixture f;
    f.serve("/models/m_123", "model.json");
    f.serve("/tones/t_789", "tone_ir.json");
    f.serve("/models?tone_id=t_789&page_size=300", "models_ir.json");
    bool asked = false;
    auto cancel = [&asked] { asked = true; return false; };
    REQUIRE_FALSE(f.client->model("m_123", cancel).error);
    REQUIRE_FALSE(f.client->tone("t_789", cancel).error);
    const auto reqs = f.http.snapshot();
    REQUIRE(reqs.size() == 3);
    for (const auto& r : reqs)
    {
        REQUIRE(r.shouldCancel);
        r.shouldCancel();
        REQUIRE(r.timeoutMs == 15000);
    }
    REQUIRE(asked);
}
