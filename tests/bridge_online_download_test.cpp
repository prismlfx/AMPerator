#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <mutex>

#include "Engine.h"

#include "support/OnlineFixture.h"

using namespace amp;
using namespace amp_test;
using amp::online::HttpRequest;
using amp::online::HttpResponse;

namespace
{
const juce::String namTarget = juce::String::fromUTF8("Fake Crunch Rig — Crunch A1 Standard.nam");
const juce::String irTarget = juce::String::fromUTF8("Fake 4x12 Cab — Fake 4x12 SM57.wav");
const juce::String fetchRigId = "00000000-0000-4000-8000-0000000000f1";

juce::MemoryBlock assetBytes(const char* name)
{
    juce::MemoryBlock block;
    fixture(name).loadFileAsData(block);
    return block;
}

juce::String sha(const char* assetName) { return AssetIndex::sha256Of(fixture(assetName)); }

struct DownloadFixture : OnlineFixture
{
    std::mutex bytesLock;
    juce::MemoryBlock namBytes = assetBytes("tiny.nam"); // what /models/m_123/download serves
    std::atomic<bool> blockDownloads { false };          // model_url requests wait for their cancel (at most 5 s)

    DownloadFixture() { connect(); }

    void setNamBytes(const juce::MemoryBlock& b)
    {
        std::lock_guard<std::mutex> guard(bytesLock);
        namBytes = b;
    }

    HttpResponse modelFile(const HttpRequest& req, const juce::MemoryBlock& bytes)
    {
        if (blockDownloads.load())
        {
            const auto deadline = juce::Time::getMillisecondCounter() + 5000;
            while (! (req.shouldCancel && req.shouldCancel()) && juce::Time::getMillisecondCounter() < deadline)
                juce::Thread::sleep(5);
            HttpResponse r;
            r.error = "canceled";
            return r;
        }
        return FakeHttp::file(200, bytes);
    }

    // m_123 (NAM, tone t_456 by jane) and m_301 (IR, tone t_789 by jane), with the tone images.
    void serveModels()
    {
        // Download routes first: FakeHttp answers the first matching prefix.
        http->on("GET", api + "/models/m_123/download", [this](const HttpRequest& r)
                 {
                     juce::MemoryBlock bytes;
                     {
                         std::lock_guard<std::mutex> guard(bytesLock);
                         bytes = namBytes;
                     }
                     return modelFile(r, bytes);
                 });
        http->on("GET", api + "/models/m_301/download",
                 [this](const HttpRequest& r) { return modelFile(r, assetBytes("ir_48k.wav")); });
        serve("/models/m_123", "model.json");
        http->on("GET", api + "/models/m_301", [](const HttpRequest&)
                 {
                     return FakeHttp::json(200, R"({"id":"m_301","tone_id":"t_789","name":"Fake 4x12 SM57",)"
                                                R"("model_url":"https://www.tone3000.com/api/v1/models/m_301/download"})");
                 });
        serveTone();
        serve("/tones/t_789", "tone_ir.json");
        serve("/models?tone_id=t_789", "models_ir.json"); // an IR tone's models are listed without architecture
        for (auto* image : { "https://example.test/img/t_456_a.jpg", "https://example.test/img/t_789.jpg" })
            http->on("GET", image,
                     [](const HttpRequest&) { return FakeHttp::file(200, juce::MemoryBlock("\xff\xd8\xff", 3)); });
    }

    std::vector<juce::var> jobEvents(const juce::String& jobId) const
    {
        std::vector<juce::var> out;
        for (auto& e : events("tone3000Download"))
            if (e["jobId"].toString() == jobId)
                out.push_back(e);
        return out;
    }

    // Pumps until the job is done or failed; returns that event.
    juce::var waitJob(const juce::String& jobId)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + 10000;
        while (juce::Time::getMillisecondCounter() < deadline)
        {
            for (auto& e : jobEvents(jobId))
                if (e["state"].toString() == "done" || e["state"].toString() == "failed")
                    return e;
            pump(10);
        }
        FAIL("job " << jobId << " did not finish");
        return {};
    }

    juce::String download(const juce::var& args)
    {
        const auto r = ok("tone3000Download", args);
        const auto id = r["jobId"].toString();
        REQUIRE(id.isNotEmpty());
        return id;
    }

    juce::var node(const juce::String& id)
    {
        for (auto& n : *state()["rig"]["nodes"].getArray())
            if (n["id"].toString() == id)
                return n;
        return {};
    }

    juce::var rigNode(const juce::var& rigLoaded, const juce::String& id)
    {
        for (auto& n : *rigLoaded["rig"]["nodes"].getArray())
            if (n["id"].toString() == id)
                return n;
        return {};
    }

    juce::File namFile() const { return lib.getChildFile("Models/TONE3000/jane").getChildFile(namTarget); }
    juce::File irFile() const { return lib.getChildFile("IRs/TONE3000/jane").getChildFile(irTarget); }

    // A saved rig whose n3 (NamAmp, m_123) and, optionally, n4 (CabIR, m_301) are TONE3000 files not in the
    // library, plus n5, a missing IR from elsewhere.
    void writeFetchRig(const juce::String& namSha, bool withIr = false)
    {
        auto irNode = withIr ? juce::String(R"({ "id": "n4", "type": "CabIR", "asset": { "kind": "ir", "file": ")")
                                   + irTarget + R"(", "sha256": ")" + sha("ir_48k.wav")
                                   + R"(", "source": { "provider": "tone3000", "id": "m_301" } } },)"
                             : juce::String(R"({ "id": "n4", "type": "Gate" },)");
        const auto text = juce::String(R"({ "format": "amperator.rig", "version": 1, "id": ")") + fetchRigId
                          + R"(", "name": "Fetch", "band": "My Tones", "nodes": [
            { "id": "n1", "type": "Input" },
            { "id": "n2", "type": "Gate" },
            { "id": "n3", "type": "NamAmp", "asset": { "kind": "nam", "file": ")"
                          + namTarget + R"(", "sha256": ")" + namSha
                          + R"(", "source": { "provider": "tone3000", "id": "m_123" } } },
            )" + irNode + R"(
            { "id": "n5", "type": "CabIR", "asset": { "kind": "ir", "file": "elsewhere.wav", "sha256": "abc" } },
            { "id": "n6", "type": "Output" } ],
          "connections": [["n1","n2"],["n2","n3"],["n3","n4"],["n4","n5"],["n5","n6"]] })";
        const auto file = rigFile("My Tones", "Fetch");
        REQUIRE(file.getParentDirectory().createDirectory());
        REQUIRE(file.replaceWithText(text));
    }
};

juce::StringArray strings(const juce::var& v)
{
    juce::StringArray out;
    if (auto* a = v.getArray())
        for (auto& x : *a)
            out.add(x.toString());
    return out;
}
} // namespace

TEST_CASE("bridge_online_download: tone3000Download puts the file in the library and on the node")
{
    DownloadFixture f;
    f.serveModels();
    const auto amp = f.nodeOfType("NamAmp");
    const auto rigId = f.state()["rig"]["id"].toString();

    const auto job = f.download(obj({ { "modelId", "m_123" }, { "toneId", "t_456" }, { "nodeId", amp } }));
    CHECK(f.jobEvents(job).empty()); // nothing re-entrant inside the command
    const auto done = f.waitJob(job);
    INFO(juce::JSON::toString(done, true));
    REQUIRE(done["state"].toString() == "done");
    CHECK(f.jobEvents(job).front()["state"].toString() == "queued");
    CHECK(done["modelId"].toString() == "m_123");
    CHECK(done["toneId"].toString() == "t_456");
    CHECK(done["nodeId"].toString() == amp);
    CHECK(done["file"].toString() == f.namFile().getFullPathName());
    CHECK(done["sha256"].toString() == sha("tiny.nam"));
    CHECK_FALSE(done.hasProperty("updated")); // no expected sha in the request
    REQUIRE(f.namFile().existsAsFile());
    CHECK(f.data.getChildFile("tone3000/index.json").existsAsFile());
    CHECK(f.data.getChildFile("tone3000/.partial").findChildFiles(juce::File::findFiles, false).isEmpty());

    // The node was assigned through setAsset: rigLoaded (same rig) and rigDirty.
    const auto loaded = f.lastEvent("rigLoaded");
    CHECK(loaded["rig"]["id"].toString() == rigId);
    const auto n = f.rigNode(loaded, amp);
    CHECK(n["asset"]["sha256"].toString() == sha("tiny.nam"));
    CHECK(n["asset"]["source"]["provider"].toString() == "tone3000");
    CHECK(n["asset"]["source"]["id"].toString() == "m_123");
    const auto image = f.lib.getChildFile("Art/tone3000/t_456.jpg");
    CHECK(n["art"]["providerImage"].toString() == image.getFullPathName());
    CHECK(n["art"]["providerImage"].toString().endsWith("Art/tone3000/t_456.jpg"));
    CHECK(static_cast<bool>(f.lastEvent("rigDirty")["dirty"]));
    CHECK_FALSE(strings(loaded["report"]["placeholderNodeIds"]).contains(amp));
    CHECK(f.engine->artFileForRequest("/art/tone3000/t_456.jpg") == image); // served to the web view

    // listAssets carries the provenance, with the image as an absolute path.
    juce::var row;
    for (auto& a : *f.ok("listAssets", obj({ { "kind", "nam" } }))["assets"].getArray())
        if (a["file"].toString() == f.namFile().getFullPathName())
            row = a;
    REQUIRE(row.isObject());
    CHECK(row["source"]["provider"].toString() == "tone3000");
    CHECK(row["source"]["id"].toString() == "m_123");
    CHECK(row["source"]["toneId"].toString() == "t_456");
    CHECK(row["source"]["title"].toString() == "Fake Crunch Rig");
    CHECK(row["source"]["creator"]["username"].toString() == "jane");
    CHECK(row["source"]["license"].toString() == "cc-by");
    CHECK(row["source"]["url"].toString() == "https://www.tone3000.com/tones/t_456");
    CHECK(row["source"]["image"].toString() == image.getFullPathName());

    // The source survives save and load.
    f.ok("saveRig", obj({ { "name", "Downloaded" } }));
    f.ok("loadRig", obj({ { "id", rigId } }));
    CHECK(f.node(amp)["asset"]["source"]["id"].toString() == "m_123");
    CHECK(f.rigFile("My Tones", "Downloaded").loadFileAsString().contains("\"m_123\""));
}

TEST_CASE("bridge_online_download: a file from elsewhere has no source and no provider image")
{
    DownloadFixture f;
    const auto file = f.lib.getChildFile("Models/plain.nam");
    REQUIRE(fixture("tiny.nam").copyFileTo(file));
    const auto amp = f.nodeOfType("NamAmp");
    f.ok("setAsset", obj({ { "id", amp }, { "file", file.getFullPathName() } }));
    const auto n = f.node(amp);
    CHECK_FALSE(n["asset"].hasProperty("source"));
    CHECK(n["art"]["providerImage"].toString().isEmpty());
    for (auto& a : *f.ok("listAssets", obj({ { "kind", "nam" } }))["assets"].getArray())
        CHECK_FALSE(a.hasProperty("source"));
}

TEST_CASE("bridge_online_download: setAsset with a downloaded file records its source")
{
    DownloadFixture f;
    f.serveModels();
    const auto job = f.download(obj({ { "modelId", "m_123" } })); // library only
    REQUIRE(f.waitJob(job)["state"].toString() == "done");
    const auto amp = f.nodeOfType("NamAmp");
    CHECK_FALSE(f.node(amp)["asset"].isObject()); // the rig was not touched

    f.ok("setAsset", obj({ { "id", amp }, { "file", f.namFile().getFullPathName() } }));
    CHECK(f.node(amp)["asset"]["source"]["id"].toString() == "m_123");

    // addNode with an asset records it too.
    const auto added = f.ok("addNode", obj({ { "type", "NamAmp" }, { "index", 1 }, { "asset", f.namFile().getFullPathName() } }));
    CHECK(f.node(added["id"].toString())["asset"]["source"]["provider"].toString() == "tone3000");
}

TEST_CASE("bridge_online_download: addNode places a new node with the download")
{
    DownloadFixture f;
    f.serveModels();
    const auto job = f.download(obj({ { "modelId", "m_301" }, { "addNode", obj({ { "type", "CabIR" }, { "index", 2 } }) } }));
    const auto done = f.waitJob(job);
    INFO(juce::JSON::toString(done, true));
    REQUIRE(done["state"].toString() == "done");
    CHECK(done["file"].toString() == f.irFile().getFullPathName());
    CHECK(f.irFile().existsAsFile());

    juce::var added;
    for (auto& n : *f.state()["rig"]["nodes"].getArray())
        if (n["type"].toString() == "CabIR" && n["asset"].isObject())
            added = n;
    REQUIRE(added.isObject());
    CHECK(f.state()["rig"]["nodes"].size() == 6);
    CHECK(added["asset"]["sha256"].toString() == sha("ir_48k.wav"));
    CHECK(added["asset"]["source"]["id"].toString() == "m_301");
    CHECK(added["art"]["providerImage"].toString().endsWith("Art/tone3000/t_789.jpg"));
    // Chain position 2 among the non-I/O nodes: Gate, NamAmp, the new cab, then the default cab.
    const auto& links = *f.state()["rig"]["connections"].getArray();
    bool afterAmp = false;
    for (auto& c : links)
        afterAmp = afterAmp || (c[0].toString() == f.nodeOfType("NamAmp") && c[1].toString() == added["id"].toString());
    CHECK(afterAmp);
    CHECK(f.events("error").empty());
}

TEST_CASE("bridge_online_download: a download of the wrong format stays in the library and reports why")
{
    DownloadFixture f;
    f.serveModels();
    const auto job = f.download(obj({ { "modelId", "m_301" }, { "addNode", obj({ { "type", "NamAmp" }, { "index", 2 } }) } }));
    REQUIRE(f.waitJob(job)["state"].toString() == "done");
    CHECK(f.irFile().existsAsFile());
    CHECK(f.state()["rig"]["nodes"].size() == 5);
    CHECK(f.lastEvent("error")["message"].toString() == "that tone is an IR file and cannot be used on a NamAmp node");
    CHECK(f.lastEvent("error")["context"].toString() == "tone3000");

    // Same check for a node target.
    const auto amp = f.nodeOfType("NamAmp");
    const auto errorsBefore = f.events("error").size();
    const auto second = f.download(obj({ { "modelId", "m_301" }, { "nodeId", amp } }));
    REQUIRE(f.waitJob(second)["state"].toString() == "done");
    CHECK(f.events("error").size() == errorsBefore + 1);
    CHECK_FALSE(f.node(amp)["asset"].isObject());
}

TEST_CASE("bridge_online_download: duplicate downloads share one file")
{
    DownloadFixture f;
    f.serveModels();
    const auto first = f.nodeOfType("NamAmp");
    const auto second = f.ok("addNode", obj({ { "type", "NamAmp" }, { "index", 1 } }))["id"].toString();

    const auto a = f.download(obj({ { "modelId", "m_123" }, { "nodeId", first } }));
    const auto b = f.download(obj({ { "modelId", "m_123" }, { "nodeId", second } }));
    CHECK(a != b);
    const auto doneA = f.waitJob(a);
    const auto doneB = f.waitJob(b);
    REQUIRE(doneA["state"].toString() == "done");
    REQUIRE(doneB["state"].toString() == "done");
    CHECK(doneA["file"].toString() == doneB["file"].toString());
    CHECK(f.lib.getChildFile("Models/TONE3000/jane").findChildFiles(juce::File::findFiles, false).size() == 1);
    CHECK(f.node(first)["asset"]["sha256"].toString() == sha("tiny.nam"));
    CHECK(f.node(second)["asset"]["sha256"].toString() == sha("tiny.nam"));
}

TEST_CASE("bridge_online_download: a model already in the library is done without the network")
{
    DownloadFixture f;
    f.serveModels();
    REQUIRE(f.waitJob(f.download(obj({ { "modelId", "m_123" } })))["state"].toString() == "done");

    const auto requests = f.http->requestCount();
    const auto amp = f.nodeOfType("NamAmp");
    const auto job = f.download(obj({ { "modelId", "m_123" }, { "nodeId", amp } }));
    CHECK(f.jobEvents(job).empty()); // asynchronous, never inside the command
    const auto done = f.waitJob(job);
    REQUIRE(done["state"].toString() == "done");
    CHECK(f.jobEvents(job).front()["state"].toString() == "queued");
    CHECK(done["file"].toString() == f.namFile().getFullPathName());
    CHECK(done["sha256"].toString() == sha("tiny.nam"));
    CHECK(f.http->requestCount() == requests);
    CHECK(f.node(amp)["asset"]["source"]["id"].toString() == "m_123");
}

TEST_CASE("bridge_online_download: a rig with a missing TONE3000 file reports it fetchable")
{
    DownloadFixture f;
    f.writeFetchRig(sha("tiny.nam"));
    f.ok("loadRig", obj({ { "id", fetchRigId } }));
    const auto report = f.lastEvent("rigLoaded")["report"];
    CHECK(strings(report["placeholderNodeIds"]) == juce::StringArray { "n3", "n5" });
    CHECK(strings(report["fetchable"]) == juce::StringArray { "n3" });
    CHECK(strings(f.state()["report"]["fetchable"]) == juce::StringArray { "n3" });
    CHECK(f.events("tone3000Download").empty()); // auto-fetch is off

    f.ok("newRig");
    CHECK(f.lastEvent("rigLoaded")["report"]["fetchable"].isArray()); // always present
    CHECK(f.lastEvent("rigLoaded")["report"]["fetchable"].size() == 0);
}

TEST_CASE("bridge_online_download: tone3000Fetch assigns every fetchable node")
{
    DownloadFixture f;
    f.serveModels();
    f.writeFetchRig(sha("tiny.nam"), true);
    f.ok("loadRig", obj({ { "id", fetchRigId } }));
    REQUIRE(strings(f.state()["report"]["fetchable"]) == juce::StringArray { "n3", "n4" });

    const auto jobs = f.ok("tone3000Fetch")["jobs"];
    REQUIRE(jobs.size() == 2);
    const auto nam = f.waitJob(jobs[0].toString());
    const auto ir = f.waitJob(jobs[1].toString());
    REQUIRE(nam["state"].toString() == "done");
    REQUIRE(ir["state"].toString() == "done");
    CHECK(nam["nodeId"].toString() == "n3");
    CHECK(isFalse(nam["updated"])); // the rig's sha matches
    CHECK(isFalse(ir["updated"]));

    const auto report = f.state()["report"];
    CHECK(strings(report["placeholderNodeIds"]) == juce::StringArray { "n5" });
    CHECK(strings(report["fetchable"]).isEmpty());
    CHECK(f.node("n3")["asset"]["source"]["id"].toString() == "m_123");
    CHECK(f.node("n4")["asset"]["sha256"].toString() == sha("ir_48k.wav"));
}

TEST_CASE("bridge_online_download: tone3000Fetch says updated when TONE3000 serves other bytes")
{
    DownloadFixture f;
    f.serveModels();
    f.writeFetchRig(juce::String::repeatedString("0", 64));
    f.ok("loadRig", obj({ { "id", fetchRigId } }));

    const auto jobs = f.ok("tone3000Fetch", obj({ { "nodeIds", juce::Array<juce::var> { "n3" } } }))["jobs"];
    REQUIRE(jobs.size() == 1);
    const auto done = f.waitJob(jobs[0].toString());
    REQUIRE(done["state"].toString() == "done");
    CHECK(static_cast<bool>(done["updated"]));
    CHECK(f.node("n3")["asset"]["sha256"].toString() == sha("tiny.nam")); // the next save records the new sha
}

TEST_CASE("bridge_online_download: tone3000Fetch refuses a node that is not fetchable")
{
    DownloadFixture f;
    f.writeFetchRig(sha("tiny.nam"));
    f.ok("loadRig", obj({ { "id", fetchRigId } }));
    const auto r = f.call("tone3000Fetch", obj({ { "nodeIds", juce::Array<juce::var> { "n3", "n5" } } }));
    CHECK_FALSE(static_cast<bool>(r["ok"]));
    CHECK(r["error"].toString().contains("n5"));
    CHECK(f.events("tone3000Download").empty());
}

TEST_CASE("bridge_online_download: fetch while offline fails each job and keeps fetchable")
{
    DownloadFixture f; // no model routes: every request fails like a network error
    f.writeFetchRig(sha("tiny.nam"), true);
    f.ok("loadRig", obj({ { "id", fetchRigId } }));

    const auto jobs = f.ok("tone3000Fetch")["jobs"];
    REQUIRE(jobs.size() == 2);
    for (auto& job : *jobs.getArray())
    {
        const auto e = f.waitJob(job.toString());
        CHECK(e["state"].toString() == "failed");
        CHECK(e["error"].toString().isNotEmpty());
    }
    CHECK(isFalse(f.state()["tone3000"]["busy"]));
    CHECK(strings(f.state()["report"]["fetchable"]) == juce::StringArray { "n3", "n4" });
    CHECK(strings(f.state()["report"]["placeholderNodeIds"]).contains("n3"));
}

TEST_CASE("bridge_online_download: a model gone from TONE3000 fails gone and leaves the placeholder")
{
    DownloadFixture f;
    f.http->on("GET", api + "/models/m_123", [](const HttpRequest&) { return FakeHttp::json(404, "{}"); });
    f.writeFetchRig(sha("tiny.nam"));
    f.ok("loadRig", obj({ { "id", fetchRigId } }));
    const auto jobs = f.ok("tone3000Fetch")["jobs"];
    REQUIRE(jobs.size() == 1);
    const auto e = f.waitJob(jobs[0].toString());
    CHECK(e["state"].toString() == "failed");
    CHECK(e["error"].toString() == "gone");
    CHECK(strings(f.state()["report"]["placeholderNodeIds"]).contains("n3"));
    CHECK(strings(f.state()["report"]["fetchable"]) == juce::StringArray { "n3" });
}

TEST_CASE("bridge_online_download: tone3000Cancel ends the job as canceled")
{
    DownloadFixture f;
    f.serveModels();
    f.blockDownloads = true;
    const auto job = f.download(obj({ { "modelId", "m_123" } }));
    const auto deadline = juce::Time::getMillisecondCounter() + 5000;
    while (f.jobEvents(job).size() < 2 && juce::Time::getMillisecondCounter() < deadline) // queued, downloading
        f.pump(10);

    CHECK(f.ok("tone3000Cancel", obj({ { "jobId", job } })).isObject());
    const auto e = f.waitJob(job);
    CHECK(e["state"].toString() == "failed");
    CHECK(e["error"].toString() == "canceled");
    CHECK_FALSE(f.namFile().exists());
    CHECK(f.data.getChildFile("tone3000/.partial").findChildFiles(juce::File::findFiles, false).isEmpty());

    CHECK(f.ok("tone3000Cancel", obj({ { "jobId", "no-such-job" } })).isObject()); // unknown: no-op
    CHECK_FALSE(static_cast<bool>(f.call("tone3000Cancel")["ok"]));
}

TEST_CASE("bridge_online_download: a quit cancels running downloads")
{
    DownloadFixture f;
    f.serveModels();
    f.blockDownloads = true;
    const auto job = f.download(obj({ { "modelId", "m_123" } }));
    f.pump(50);
    REQUIRE(f.engine->requestQuit()); // nothing unsaved: quit now
    const auto e = f.waitJob(job);
    CHECK(e["state"].toString() == "failed");
    CHECK(e["error"].toString() == "canceled");
}

TEST_CASE("bridge_online_download: download and fetch need a connection and validate their arguments")
{
    DownloadFixture f;
    f.ok("tone3000Disconnect");
    f.writeFetchRig(sha("tiny.nam"));
    f.ok("loadRig", obj({ { "id", fetchRigId } }));
    for (const auto& [cmd, args] : { std::pair { "tone3000Download", obj({ { "modelId", "m_123" } }) },
                                     std::pair { "tone3000Fetch", obj() } })
    {
        const auto r = f.call(cmd, args);
        CHECK_FALSE(static_cast<bool>(r["ok"]));
        CHECK(r["error"].toString() == "not connected");
    }

    f.connect();
    for (const auto& args : { obj(), obj({ { "modelId", "" } }),
                              obj({ { "modelId", "m_1" }, { "addNode", obj({ { "type", "Gate" }, { "index", 1 } }) } }),
                              obj({ { "modelId", "m_1" }, { "addNode", obj({ { "type", "CabIR" } }) } }),
                              obj({ { "modelId", "m_1" }, { "nodeId", "nope" } }) })
    {
        INFO(juce::JSON::toString(args, true));
        CHECK_FALSE(static_cast<bool>(f.call("tone3000Download", args)["ok"]));
    }
    CHECK(f.events("tone3000Download").empty());
}

TEST_CASE("bridge_online_download: without a client id the download commands are not configured")
{
    if (compiledTone3000ClientId().isNotEmpty())
        SKIP("this build has a TONE3000 client id compiled in");
    OnlineFixture f("");
    for (const auto& [cmd, args] : { std::pair { "tone3000Download", obj({ { "modelId", "m_123" } }) },
                                     std::pair { "tone3000Fetch", obj() },
                                     std::pair { "tone3000Cancel", obj({ { "jobId", "j" } }) } })
    {
        const auto r = f.call(cmd, args);
        CHECK_FALSE(static_cast<bool>(r["ok"]));
        CHECK(r["error"].toString() == "not configured");
    }
}

TEST_CASE("bridge_online_download: tone3000AutoFetch fetches after a load when connected")
{
    DownloadFixture f;
    f.serveModels();
    f.ok("setSettings", obj({ { "tone3000AutoFetch", true } }));
    f.writeFetchRig(sha("tiny.nam"));
    f.ok("loadRig", obj({ { "id", fetchRigId } }));

    const auto deadline = juce::Time::getMillisecondCounter() + 10000;
    while (f.events("tone3000Download").empty() && juce::Time::getMillisecondCounter() < deadline)
        f.pump(10);
    REQUIRE_FALSE(f.events("tone3000Download").empty());
    const auto done = f.waitJob(f.events("tone3000Download").front()["jobId"].toString());
    CHECK(done["state"].toString() == "done");
    CHECK(done["nodeId"].toString() == "n3");
    CHECK(strings(f.state()["report"]["fetchable"]).isEmpty());

    // A structural edit does not fetch again.
    const auto before = f.events("tone3000Download").size();
    f.ok("moveNode", obj({ { "id", "n2" }, { "index", 2 } }));
    f.pump(100);
    CHECK(f.events("tone3000Download").size() == before);
}

TEST_CASE("bridge_online_download: tone3000AutoFetch does nothing while disconnected")
{
    DownloadFixture f;
    f.ok("tone3000Disconnect");
    f.ok("setSettings", obj({ { "tone3000AutoFetch", true } }));
    f.writeFetchRig(sha("tiny.nam"));
    const auto opened = f.window.opened;
    f.ok("loadRig", obj({ { "id", fetchRigId } }));
    f.pump(100);
    CHECK(f.events("tone3000Download").empty());
    CHECK(f.window.opened == opened);
    CHECK(strings(f.state()["report"]["fetchable"]) == juce::StringArray { "n3" });
}

TEST_CASE("bridge_online_download: a download for a rig that is no longer loaded leaves the rig alone")
{
    DownloadFixture f;
    f.serveModels();
    const auto amp = f.nodeOfType("NamAmp");
    const auto job = f.download(obj({ { "modelId", "m_123" }, { "nodeId", amp } }));
    f.ok("newRig"); // the done event cannot arrive before the message loop runs
    const auto loaded = f.events("rigLoaded").size();
    REQUIRE(f.waitJob(job)["state"].toString() == "done");
    CHECK(f.events("rigLoaded").size() == loaded);
    CHECK(f.namFile().existsAsFile());
    CHECK(f.events("error").empty());
}

TEST_CASE("bridge_online_download: a download for a removed node leaves the rig alone")
{
    DownloadFixture f;
    f.serveModels();
    const auto amp = f.nodeOfType("NamAmp");
    const auto job = f.download(obj({ { "modelId", "m_123" }, { "nodeId", amp } }));
    f.ok("removeNode", obj({ { "id", amp } }));
    const auto loaded = f.events("rigLoaded").size();
    REQUIRE(f.waitJob(job)["state"].toString() == "done");
    CHECK(f.events("rigLoaded").size() == loaded);
    CHECK(f.namFile().existsAsFile());
    CHECK(f.events("error").empty());
}

TEST_CASE("bridge_online_download: the error for a NAM file on a cab node names the format")
{
    DownloadFixture f;
    f.serveModels();
    const auto job = f.download(obj({ { "modelId", "m_123" }, { "addNode", obj({ { "type", "CabIR" }, { "index", 2 } }) } }));
    REQUIRE(f.waitJob(job)["state"].toString() == "done");
    CHECK(f.lastEvent("error")["message"].toString() == "that tone is a NAM file and cannot be used on a CabIR node");
}

TEST_CASE("bridge_online_download: a 429 job says how long to wait")
{
    DownloadFixture f;
    juce::StringPairArray headers;
    headers.set("Retry-After", "30");
    f.http->on("GET", api + "/models/m_123", [headers](const HttpRequest&) { return FakeHttp::json(429, "{}", headers); });
    const auto e = f.waitJob(f.download(obj({ { "modelId", "m_123" } })));
    CHECK(e["state"].toString() == "failed");
    CHECK(e["error"].toString() == "TONE3000 rate limit; try again in 30 s");
}

TEST_CASE("bridge_online_download: tone3000Disconnect during a stalled download replies at once")
{
    DownloadFixture f;
    std::atomic<bool> inFetch { false }, release { false }, handlerDone { false };
    f.http->on("GET", api + "/models/m_123/download", [&](const HttpRequest&)
               {
                   inFetch = true;
                   const auto deadline = juce::Time::getMillisecondCounter() + 5000;
                   while (! release.load() && juce::Time::getMillisecondCounter() < deadline) // a dead connection
                       juce::Thread::sleep(5);
                   handlerDone = true;
                   return FakeHttp::file(200, assetBytes("tiny.nam"));
               });
    f.serveModels();
    const auto job = f.download(obj({ { "modelId", "m_123" } }));
    const auto deadline = juce::Time::getMillisecondCounter() + 5000;
    while (! inFetch.load() && juce::Time::getMillisecondCounter() < deadline)
        f.pump(10);
    REQUIRE(inFetch.load());

    f.ok("tone3000Disconnect");
    CHECK_FALSE(handlerDone.load()); // the reply did not wait for the network
    release = true;
    const auto e = f.waitJob(job);
    CHECK(e["state"].toString() == "failed");
    CHECK(e["error"].toString() == "canceled");
    CHECK_FALSE(f.namFile().exists());
}

TEST_CASE("bridge_online_download: the token never follows a model or image URL off the origin")
{
    DownloadFixture f;
    const juce::String hostile[] = { "https://www.tone3000.com:443@evil.example/x", "https://user:pass@www.tone3000.com/x",
                                     "http://www.tone3000.com/x" };
    f.serveModels();
    juce::StringArray models;
    for (const auto& url : hostile)
        for (const bool asModel : { true, false })
        {
            const auto model = "m_h" + juce::String(models.size()), tone = "t_h" + juce::String(models.size());
            models.add(model);
            const auto modelUrl = asModel ? url : api + "/models/m_123/download";
            f.http->on("GET", api + "/models/" + model, [model, tone, modelUrl](const HttpRequest&)
                       {
                           return FakeHttp::json(200, R"({"id":")" + model + R"(","tone_id":")" + tone
                                                          + R"(","name":"H","model_url":")" + modelUrl + R"("})");
                       });
            f.http->on("GET", api + "/tones/" + tone, [tone, url](const HttpRequest&)
                       {
                           return FakeHttp::json(200, R"({"id":")" + tone + R"(","title":"H","platform":"nam",)"
                                                          + R"("user":{"username":"jane"},"images":[")" + url + R"("]})");
                       });
            f.http->on("GET", api + "/models?tone_id=" + tone,
                       [](const HttpRequest&) { return FakeHttp::json(200, R"({"data":[]})"); });
        }
    f.http->on("GET", "", [](const HttpRequest&) { return FakeHttp::file(200, assetBytes("tiny.nam")); }); // anything else
    for (const auto& model : models)
        f.waitJob(f.download(obj({ { "modelId", model } })));

    for (const auto& r : f.http->snapshot())
        if (! r.url.startsWith(api))
        {
            INFO(r.url);
            CHECK(r.headers["Authorization"].isEmpty());
        }
}

TEST_CASE("bridge_online_download: Replace resolves the node's tone from the provenance index")
{
    DownloadFixture f;
    f.serveModels();
    REQUIRE(f.waitJob(f.download(obj({ { "modelId", "m_123" } })))["state"].toString() == "done");
    REQUIRE(f.namFile().deleteFile()); // the record outlives the file
    f.writeFetchRig(sha("tiny.nam"));
    f.ok("loadRig", obj({ { "id", fetchRigId } }));

    const int opened = f.window.opened;
    const int id = f.post("tone3000Browse", obj({ { "format", "nam" }, { "nodeId", "n3" }, { "replace", true } }));
    REQUIRE(f.window.opened == opened + 1);
    CHECK(f.urlParam("prompt") == "load_tone");
    CHECK(f.urlParam("tone_id") == "t_456");
    f.window.done("tone_id=t_456&state=" + f.urlParam("state"));
    CHECK(f.waitReply(id)["result"]["nodeId"].toString() == "n3");

    // A node without a known TONE3000 source gets the plain picker.
    const int other = f.post("tone3000Browse", obj({ { "format", "ir" }, { "nodeId", "n5" }, { "replace", true } }));
    CHECK(f.urlParam("prompt") == "select_tone");
    CHECK_FALSE(f.window.url.contains("tone_id="));
    f.window.done("");
    f.waitReply(other);

    // Without replace, a node id alone never turns into load_tone.
    const int plain = f.post("tone3000Browse", obj({ { "format", "nam" }, { "nodeId", "n3" } }));
    CHECK(f.urlParam("prompt") == "select_tone");
    f.window.done("");
    f.waitReply(plain);
}
