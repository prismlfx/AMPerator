#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <vector>

#include <juce_events/juce_events.h>

#include "assets/AssetIndex.h"
#include "online/DownloadManager.h"
#include "support/FakeHttp.h"

using namespace amp::online;
using amp_test::FakeHttp;

namespace
{
const juce::String api = "https://www.tone3000.com/api/v1";
const juce::String imageUrl = "https://example.test/img/t_456_a.jpg";

juce::String fixture(const char* name)
{
    return juce::File(AMP_FIXTURES_DIR).getChildFile("tone3000").getChildFile(name).loadFileAsString();
}

juce::MemoryBlock assetBytes(const char* name)
{
    juce::MemoryBlock block;
    juce::File(AMP_FIXTURES_DIR).getChildFile("assets").getChildFile(name).loadFileAsData(block);
    return block;
}

struct DownloadFixture
{
    juce::ScopedJuceInitialiser_GUI gui;
    juce::TemporaryFile root { juce::File::getSpecialLocation(juce::File::tempDirectory)
                                   .getChildFile("amp_download_" + juce::Uuid().toString()) };
    juce::File partialDir = root.getFile().getChildFile("partial");
    juce::File artDir = root.getFile().getChildFile("Art");
    FakeHttp http;
    MemorySecureStore store;
    std::atomic<juce::int64> clock { 1000000 };
    std::unique_ptr<Tone3000Auth> auth;
    std::unique_ptr<Tone3000Client> client;
    std::unique_ptr<DownloadManager> manager;
    std::vector<JobEvent> events;
    bool onMessageThread = true;

    DownloadFixture()
    {
        root.getFile().createDirectory();
        AuthConfig cfg;
        cfg.clientId = "cid";
        auth = std::make_unique<Tone3000Auth>(cfg, http, store, [this] { return clock.load(); });
        store.set("tokens", R"({"access":"AT","refresh":"RT","expiresAtMs":99000000})");
        client = std::make_unique<Tone3000Client>(*auth, http, "https://www.tone3000.com");
        manager = std::make_unique<DownloadManager>(*client, partialDir, artDir);
        manager->onEvent = [this](const JobEvent& e) {
            onMessageThread = onMessageThread && juce::MessageManager::getInstance()->existsAndIsCurrentThread();
            events.push_back(e);
        };
    }

    ~DownloadFixture()
    {
        manager.reset();
        root.getFile().deleteRecursively();
    }

    void serveMetadata()
    {
        http.on("GET", api + "/models/m_123/download", [](const HttpRequest&) {
            return FakeHttp::file(200, assetBytes("tiny.nam"));
        });
        http.on("GET", api + "/models/m_123",
                [](const HttpRequest&) { return FakeHttp::json(200, fixture("model.json")); });
        http.on("GET", api + "/tones/t_456",
                [](const HttpRequest&) { return FakeHttp::json(200, fixture("tone.json")); });
        http.on("GET", api + "/models?tone_id=t_456&architecture=2",
                [](const HttpRequest&) { return FakeHttp::json(200, fixture("models_a2.json")); });
        http.on("GET", api + "/models?tone_id=t_456&architecture=1",
                [](const HttpRequest&) { return FakeHttp::json(200, fixture("models_a1.json")); });
        http.on("GET", imageUrl, [](const HttpRequest&) { return FakeHttp::file(200, juce::MemoryBlock("img", 3)); });
    }

    bool pumpUntil(const std::function<bool()>& done)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + 10000;
        while (! done() && juce::Time::getMillisecondCounter() < deadline)
            juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        return done();
    }

    bool finished()
    {
        return ! events.empty() && (events.back().state == JobState::Done || events.back().state == JobState::Failed);
    }

    int count(JobState s) const
    {
        int n = 0;
        for (const auto& e : events)
            n += e.state == s ? 1 : 0;
        return n;
    }
};
} // namespace

TEST_CASE("online_download: happy path validates, hashes and caches the image")
{
    DownloadFixture f;
    f.serveMetadata();

    DownloadRequest req;
    req.modelId = "m_123";
    req.nodeId = "n1";
    auto addNode = new juce::DynamicObject();
    addNode->setProperty("type", "nam");
    req.addNode = juce::var(addNode);
    const auto id = f.manager->start(req);
    REQUIRE(id.isNotEmpty());
    REQUIRE(f.events.empty()); // Queued is never re-entrant

    REQUIRE(f.pumpUntil([&] { return f.finished(); }));
    REQUIRE(f.onMessageThread);
    REQUIRE(f.events.front().state == JobState::Queued);
    REQUIRE(f.events[1].state == JobState::Downloading);
    REQUIRE(f.events[f.events.size() - 2].state == JobState::Verifying);
    REQUIRE(f.count(JobState::Done) == 1);
    for (const auto& e : f.events)
        REQUIRE(e.jobId == id);

    const auto& done = f.events.back();
    REQUIRE(done.state == JobState::Done);
    REQUIRE(done.file == f.partialDir.getChildFile(id));
    REQUIRE(done.file.existsAsFile());
    REQUIRE(done.sha256 == amp::AssetIndex::sha256Of(done.file));
    REQUIRE(done.model.id == "m_123");
    REQUIRE(done.tone.id == "t_456");
    REQUIRE(done.request.nodeId == "n1");
    REQUIRE(done.request.addNode["type"].toString() == "nam");
    REQUIRE(done.image == f.artDir.getChildFile("tone3000").getChildFile("t_456.jpg"));
    REQUIRE(done.image.existsAsFile());

    auto v = toVar(done);
    REQUIRE(v["state"].toString() == "done");
    REQUIRE(v["modelId"].toString() == "m_123");
    REQUIRE(v["toneId"].toString() == "t_456");
    REQUIRE(v["nodeId"].toString() == "n1");
    REQUIRE(v["sha256"].toString() == done.sha256);
    REQUIRE(toVar(f.events.front())["state"].toString() == "queued");

    // A second job reuses the cached image.
    const auto before = f.http.requestCount();
    f.events.clear();
    f.manager->start(DownloadRequest { {}, "m_123", {}, {}, {}, {} });
    REQUIRE(f.pumpUntil([&] { return f.finished(); }));
    REQUIRE(f.events.back().state == JobState::Done);
    REQUIRE(f.events.back().image == done.image);
    int imageRequests = 0;
    for (const auto& r : f.http.snapshot())
        imageRequests += r.url == imageUrl ? 1 : 0;
    REQUIRE(imageRequests == 1);
    REQUIRE(f.http.requestCount() > before);
}

TEST_CASE("online_download: an image failure does not fail the job")
{
    DownloadFixture g;
    g.http.on("GET", imageUrl, [](const HttpRequest&) { return FakeHttp::json(404, "{}"); });
    g.serveMetadata();
    g.manager->start(DownloadRequest { {}, "m_123", {}, {}, {}, {} });
    REQUIRE(g.pumpUntil([&] { return g.finished(); }));
    REQUIRE(g.events.back().state == JobState::Done);
    REQUIRE(g.events.back().image == juce::File());
    REQUIRE(! g.artDir.getChildFile("tone3000").getChildFile("t_456.part").exists());
}

TEST_CASE("online_download: html body is not accepted as a model")
{
    DownloadFixture f;
    f.http.on("GET", api + "/models/m_123/download", [](const HttpRequest&) {
        const juce::String html = "<html><body>login</body></html>";
        return FakeHttp::file(200, juce::MemoryBlock(html.toRawUTF8(), html.getNumBytesAsUTF8()));
    });
    f.serveMetadata();

    const auto id = f.manager->start(DownloadRequest { {}, "m_123", {}, {}, {}, {} });
    REQUIRE(f.pumpUntil([&] { return f.finished(); }));
    REQUIRE(f.events.back().state == JobState::Failed);
    REQUIRE(f.events.back().error == "not a valid nam file");
    REQUIRE(! f.partialDir.getChildFile(id).exists());
    REQUIRE(toVar(f.events.back())["error"].toString() == "not a valid nam file");
}

TEST_CASE("online_download: a 404 on the model is gone")
{
    DownloadFixture f;
    f.http.on("GET", api + "/models/x", [](const HttpRequest&) { return FakeHttp::json(404, "{}"); });

    const auto id = f.manager->start(DownloadRequest { {}, "x", {}, {}, {}, {} });
    REQUIRE(f.pumpUntil([&] { return f.finished(); }));
    REQUIRE(f.events.back().state == JobState::Failed);
    REQUIRE(f.events.back().error == "gone");
    REQUIRE(! f.partialDir.getChildFile(id).exists());
}

TEST_CASE("online_download: cancel during the fetch ends in one canceled failure")
{
    DownloadFixture f;
    std::atomic<bool> inFetch { false };
    f.http.on("GET", api + "/models/m_123/download", [&](const HttpRequest& r) {
        r.toFile.getParentDirectory().createDirectory();
        r.toFile.replaceWithText("partial bytes");
        inFetch = true;
        for (int i = 0; i < 1000; ++i)
        {
            if (r.shouldCancel && r.shouldCancel())
            {
                HttpResponse out;
                out.error = "canceled";
                return out;
            }
            juce::Thread::sleep(5);
        }
        return FakeHttp::file(200, assetBytes("tiny.nam"));
    });
    f.serveMetadata();

    const auto id = f.manager->start(DownloadRequest { {}, "m_123", {}, {}, {}, {} });
    REQUIRE(f.pumpUntil([&] { return inFetch.load(); }));
    f.manager->cancel(id);
    REQUIRE(f.pumpUntil([&] { return f.finished(); }));
    REQUIRE(f.count(JobState::Failed) == 1);
    REQUIRE(f.events.back().error == "canceled");
    REQUIRE(! f.partialDir.getChildFile(id).exists());

    f.manager->cancel(id); // finished: no-op
    f.manager->cancel("unknown");
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    REQUIRE(f.count(JobState::Failed) == 1);
}

TEST_CASE("online_download: cancelAll stops running jobs and nothing arrives after destruction")
{
    DownloadFixture f;
    std::atomic<int> inFetch { 0 };
    f.http.on("GET", api + "/models/m_123/download", [&](const HttpRequest& r) {
        r.toFile.getParentDirectory().createDirectory();
        r.toFile.replaceWithText("partial bytes");
        ++inFetch;
        for (int i = 0; i < 1000; ++i)
        {
            if (r.shouldCancel && r.shouldCancel())
            {
                HttpResponse out;
                out.error = "canceled";
                return out;
            }
            juce::Thread::sleep(5);
        }
        return FakeHttp::file(200, assetBytes("tiny.nam"));
    });
    f.serveMetadata();

    f.manager->start(DownloadRequest { "a", "m_123", {}, {}, {}, {} });
    f.manager->start(DownloadRequest { "b", "m_123", {}, {}, {}, {} });
    REQUIRE(f.pumpUntil([&] { return inFetch.load() == 2; }));
    f.manager->cancelAll();
    REQUIRE(f.pumpUntil([&] { return f.count(JobState::Failed) == 2; }));
    REQUIRE(! f.partialDir.getChildFile("a").exists());
    REQUIRE(! f.partialDir.getChildFile("b").exists());

    // Destroying with events still queued delivers nothing.
    f.events.clear();
    f.manager->start(DownloadRequest { "c", "m_123", {}, {}, {}, {} });
    f.manager.reset();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    REQUIRE(f.events.empty());
}

TEST_CASE("online_download: progress events are throttled and carry received over total")
{
    DownloadFixture f;
    f.http.on("GET", api + "/models/m_123/download", [&](const HttpRequest& r) {
        for (int i = 1; i <= 100; ++i)
        {
            r.onProgress(i * 10, 1000);
            juce::Thread::sleep(10);
        }
        return FakeHttp::file(200, assetBytes("tiny.nam"));
    });
    f.serveMetadata();
    f.manager->start(DownloadRequest { {}, "m_123", {}, {}, {}, {} });
    REQUIRE(f.pumpUntil([&] { return f.finished(); }));
    REQUIRE(f.events.back().state == JobState::Done);

    int progressEvents = 0;
    double last = 0;
    for (const auto& e : f.events)
        if (e.state == JobState::Downloading && e.progress > 0)
        {
            ++progressEvents;
            REQUIRE(e.progress >= last);
            REQUIRE(e.progress <= 1.0);
            last = e.progress;
        }
    REQUIRE(progressEvents >= 1);
    REQUIRE(progressEvents <= 6); // ~1 s of progress at one event per 250 ms
}

TEST_CASE("online_download: cleanPartials removes leftovers")
{
    juce::TemporaryFile root;
    auto dir = root.getFile();
    dir.createDirectory();
    dir.getChildFile("a").replaceWithText("x");
    dir.getChildFile("sub").createDirectory();
    dir.getChildFile("sub").getChildFile("b").replaceWithText("y");
    DownloadManager::cleanPartials(dir);
    REQUIRE(dir.getNumberOfChildFiles(juce::File::findFilesAndDirectories) == 0);
    DownloadManager::cleanPartials(dir.getChildFile("missing")); // no throw
    dir.deleteRecursively();
}
