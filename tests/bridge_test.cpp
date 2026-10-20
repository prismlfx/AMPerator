#include <catch2/catch_test_macros.hpp>

#include <juce_events/juce_events.h>

#include "Engine.h"
#include "bridge/Bridge.h"
#include "storage/RigStore.h"

#include "support/FakeAudioDevice.h"
#include "support/FakePlugins.h"

using namespace amp;

namespace
{
juce::var obj(std::initializer_list<std::pair<const char*, juce::var>> props = {})
{
    auto* o = new juce::DynamicObject();
    for (auto& [k, v] : props)
        o->setProperty(k, v);
    return juce::var(o);
}

juce::File fixture(const char* name)
{
    return juce::File(AMP_FIXTURES_DIR).getChildFile("assets").getChildFile(name);
}

struct Fixture
{
    const juce::ScopedJuceInitialiser_GUI juce; // this thread becomes the message thread
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("amp_bridge_" + juce::Uuid().toString());
    juce::File lib = dir.getChildFile("lib");
    juce::File data = dir.getChildFile("data");
    std::unique_ptr<juce::AudioDeviceManager> manager; // only for the fake-device test
    std::unique_ptr<Engine> engine;
    std::vector<juce::var> sent;
    int nextId = 1;

    Fixture() { start(); }
    ~Fixture()
    {
        engine.reset();
        manager.reset();
        dir.deleteRecursively();
    }

    void start(bool withFakeDevice = false)
    {
        engine.reset();
        Engine::Options o;
        o.libraryRoot = lib;
        o.dataDir = data;
        o.startAudio = withFakeDevice;
        o.startControls = false;
        o.pluginFormats.push_back(std::make_unique<amp_test::FakeFormat>());
        o.addDefaultPluginFormats = false;
        if (withFakeDevice)
        {
            manager = std::make_unique<juce::AudioDeviceManager>();
            manager->addAudioDeviceType(std::make_unique<amp_test::FakeType>()); // never touches CoreAudio
            o.deviceManager = manager.get();
        }
        engine = std::make_unique<Engine>(std::move(o));
        sent.clear();
        engine->bridge().send = [this](const juce::var& m) { sent.push_back(m); };
    }

    juce::var raw(const juce::var& message)
    {
        const auto id = message["id"];
        engine->bridge().handle(message);
        for (auto& m : sent)
            if (m.hasProperty("reply") && m["reply"] == id)
                return m;
        FAIL("no reply for id " << id.toString());
        return {};
    }

    juce::var call(const juce::String& cmd, const juce::var& args = obj())
    {
        return raw(obj({ { "v", 1 }, { "cmd", cmd }, { "id", nextId++ }, { "args", args } }));
    }

    juce::var ok(const juce::String& cmd, const juce::var& args = obj())
    {
        auto r = call(cmd, args);
        INFO(cmd << " -> " << juce::JSON::toString(r, true));
        REQUIRE(static_cast<bool>(r["ok"]));
        return r["result"];
    }

    std::vector<juce::var> events(const juce::String& name) const
    {
        std::vector<juce::var> out;
        for (auto& m : sent)
            if (m["event"].toString() == name)
                out.push_back(m["data"]);
        return out;
    }

    juce::var lastEvent(const juce::String& name) const
    {
        auto e = events(name);
        REQUIRE(! e.empty());
        return e.back();
    }

    juce::var state() { return ok("getState"); }

    // Node id of the first node of `type` in the current rig.
    juce::String nodeOfType(const juce::String& type)
    {
        auto rig = state()["rig"];
        for (auto& n : *rig["nodes"].getArray())
            if (n["type"].toString() == type)
                return n["id"].toString();
        FAIL("no node of type " << type);
        return {};
    }

    juce::File rigFile(const juce::String& band, const juce::String& name) const
    {
        return lib.getChildFile("Rigs").getChildFile(band).getChildFile(name + ".amprig");
    }
};

juce::StringArray nodeTypes(const juce::var& rig)
{
    juce::StringArray out;
    for (auto& n : *rig["nodes"].getArray())
        out.add(n["type"].toString());
    return out;
}
} // namespace

TEST_CASE("bridge: unknown command replies ok=false")
{
    Fixture f;
    auto r = f.call("frobnicate");
    CHECK_FALSE(static_cast<bool>(r["ok"]));
    CHECK(r["v"] == juce::var(1));
    CHECK(r["error"].toString() == "unknown command: frobnicate");
}

TEST_CASE("bridge: wrong protocol version replies error mentioning version")
{
    Fixture f;
    auto r = f.raw(obj({ { "v", 2 }, { "cmd", "getState" }, { "id", 7 } }));
    CHECK(r["reply"] == juce::var(7));
    CHECK_FALSE(static_cast<bool>(r["ok"]));
    CHECK(r["error"].toString().containsIgnoreCase("version"));
}

TEST_CASE("bridge: missing or invalid args reply ok=false")
{
    Fixture f;
    auto r = f.call("setParam", obj({ { "name", "threshold" }, { "value", 1.0 } }));
    CHECK_FALSE(static_cast<bool>(r["ok"]));
    CHECK(r["error"].toString().contains("id"));

    r = f.call("moveNode", obj({ { "id", "x" }, { "index", "two" } }));
    CHECK_FALSE(static_cast<bool>(r["ok"]));
    CHECK(r["error"].toString().contains("index"));

    r = f.call("toggleBypass", obj({ { "id", "no-such-node" } }));
    CHECK_FALSE(static_cast<bool>(r["ok"]));

    r = f.raw(obj({ { "v", 1 }, { "cmd", "getState" }, { "id", 99 }, { "args", "nope" } }));
    CHECK_FALSE(static_cast<bool>(r["ok"]));
}

TEST_CASE("bridge: getState reports protocol and a default rig on first start")
{
    Fixture f;
    auto s = f.state();
    CHECK(s["protocol"] == juce::var(1));
    CHECK(nodeTypes(s["rig"]) == juce::StringArray { "Input", "Gate", "NamAmp", "CabIR", "Output" });
    CHECK(s["rig"]["band"].toString() == "My Tones");
    CHECK_FALSE(static_cast<bool>(s["dirty"]));
    CHECK(s["deviceStatus"]["status"].toString() == "NoDevice");
    CHECK(s["controlStatus"]["hid"].toString() == "NotFound");
    CHECK(f.engine->planSpec().sampleRate == 48000.0);
    CHECK(f.engine->planSpec().maxBlock == 512);
}

TEST_CASE("bridge: newRig then saveRig then listRigs contains it")
{
    Fixture f;
    f.ok("newRig", obj({ { "name", "Interstate" }, { "band", "STP" } }));
    auto loaded = f.lastEvent("rigLoaded");
    const auto id = loaded["rig"]["id"].toString();
    CHECK(loaded["rig"]["name"].toString() == "Interstate");

    f.ok("saveRig");
    CHECK_FALSE(static_cast<bool>(f.lastEvent("rigDirty")["dirty"]));
    CHECK(f.rigFile("STP", "Interstate").existsAsFile());

    auto list = f.ok("listRigs");
    bool found = false;
    for (auto& r : *list["rigs"].getArray())
        if (r["id"].toString() == id)
            found = r["name"].toString() == "Interstate" && r["band"].toString() == "STP";
    CHECK(found);
    CHECK(list["bands"].getArray()->contains("STP"));
}

TEST_CASE("bridge: addNode emits rigLoaded with node in order")
{
    Fixture f;
    f.ok("addNode", obj({ { "type", "EQ" }, { "index", 1 } }));
    auto loaded = f.lastEvent("rigLoaded");
    CHECK(nodeTypes(loaded["rig"]) == juce::StringArray { "Input", "Gate", "EQ", "NamAmp", "CabIR", "Output" });
    CHECK(static_cast<bool>(f.lastEvent("rigDirty")["dirty"]));

    // Plugins come from the PluginHost; the added node runs live (not a placeholder).
    f.ok("rescanPlugins");
    auto plugins = f.ok("listPlugins");
    juce::var gain;
    for (auto& p : *plugins["plugins"].getArray())
        if (p["name"].toString() == "FakeGain")
            gain = p;
    REQUIRE(gain.isObject());
    f.ok("addNode", obj({ { "type", "Plugin" }, { "index", 99 }, { "plugin", gain } }));
    loaded = f.lastEvent("rigLoaded");
    CHECK(nodeTypes(loaded["rig"]) == juce::StringArray { "Input", "Gate", "EQ", "NamAmp", "CabIR", "Plugin", "Output" });
    CHECK(loaded["report"]["placeholderNodeIds"].getArray()->isEmpty());

    // move + remove
    const auto eq = f.nodeOfType("EQ");
    f.ok("moveNode", obj({ { "id", eq }, { "index", 3 } }));
    CHECK(nodeTypes(f.lastEvent("rigLoaded")["rig"])
          == juce::StringArray { "Input", "Gate", "NamAmp", "CabIR", "EQ", "Plugin", "Output" });
    f.ok("removeNode", obj({ { "id", eq } }));
    CHECK(nodeTypes(f.lastEvent("rigLoaded")["rig"])
          == juce::StringArray { "Input", "Gate", "NamAmp", "CabIR", "Plugin", "Output" });
}

TEST_CASE("bridge: toggleBypass emits bypassChanged")
{
    Fixture f;
    const auto gate = f.nodeOfType("Gate");
    auto r = f.ok("toggleBypass", obj({ { "id", gate } }));
    CHECK(static_cast<bool>(r["bypass"]));
    auto e = f.lastEvent("bypassChanged");
    CHECK(e["id"].toString() == gate);
    CHECK(static_cast<bool>(e["bypass"]));
    CHECK(static_cast<bool>(f.lastEvent("rigDirty")["dirty"]));

    f.ok("toggleBypass", obj({ { "id", gate } }));
    CHECK_FALSE(static_cast<bool>(f.lastEvent("bypassChanged")["bypass"]));
}

TEST_CASE("bridge: setParam acts on the live node and is saved")
{
    Fixture f;
    const auto gate = f.nodeOfType("Gate");
    f.ok("setParam", obj({ { "id", gate }, { "name", "threshold" }, { "value", -42.0 } }));
    auto e = f.lastEvent("paramChanged");
    CHECK(e["id"].toString() == gate);
    CHECK(e["name"].toString() == "threshold");
    CHECK(static_cast<double>(e["value"]) == -42.0);

    auto rig = f.state()["rig"];
    for (auto& n : *rig["nodes"].getArray())
        if (n["id"].toString() == gate)
            CHECK(static_cast<double>(n["params"]["threshold"]) == -42.0);

    f.ok("saveRig");
    const auto id = rig["id"].toString();
    auto parsed = RigStore(f.lib).load(id);
    REQUIRE(parsed.rig.has_value());
    for (auto& n : parsed.rig->nodes)
        if (n.id == gate)
            CHECK(static_cast<double>(n.params["threshold"]) == -42.0);
}

TEST_CASE("bridge: learnMapping then injected control press emits learned and saves mapping in rig on saveRig")
{
    Fixture f;
    const auto gate = f.nodeOfType("Gate");
    f.ok("learnMapping", obj({ { "id", gate } }));
    f.engine->injectControl({ "footswitch:L", true });

    auto ce = f.lastEvent("controlEvent");
    CHECK(ce["control"].toString() == "footswitch:L");
    CHECK(static_cast<bool>(ce["pressed"]));
    auto learned = f.lastEvent("learned");
    CHECK(learned["control"].toString() == "footswitch:L");
    CHECK(learned["node"].toString() == gate);
    CHECK(f.events("bypassChanged").empty()); // the learn press itself does not toggle

    // The next press toggles the node.
    f.engine->injectControl({ "footswitch:L", false });
    f.engine->injectControl({ "footswitch:L", true });
    auto e = f.lastEvent("bypassChanged");
    CHECK(e["id"].toString() == gate);
    CHECK(static_cast<bool>(e["bypass"]));

    f.ok("saveRig");
    const auto id = f.state()["rig"]["id"].toString();
    auto parsed = RigStore(f.lib).load(id);
    REQUIRE(parsed.rig.has_value());
    REQUIRE(parsed.rig->mappings.size() == 1);
    CHECK(parsed.rig->mappings[0].control == "footswitch:L");
    CHECK(parsed.rig->mappings[0].action == "toggleBypass");
    CHECK(parsed.rig->mappings[0].node == gate);
}

TEST_CASE("bridge: nextRig wraps around")
{
    Fixture f;
    f.ok("newRig", obj({ { "name", "Alpha" }, { "band", "My Tones" } }));
    f.ok("saveRig");
    f.ok("newRig", obj({ { "name", "Bravo" }, { "band", "My Tones" } }));
    f.ok("saveRig");

    f.ok("nextRig");
    CHECK(f.lastEvent("rigLoaded")["rig"]["name"].toString() == "Alpha"); // wrapped past the end
    f.ok("nextRig");
    CHECK(f.lastEvent("rigLoaded")["rig"]["name"].toString() == "Bravo");
    f.ok("prevRig");
    CHECK(f.lastEvent("rigLoaded")["rig"]["name"].toString() == "Alpha");
    f.ok("prevRig");
    CHECK(f.lastEvent("rigLoaded")["rig"]["name"].toString() == "Bravo"); // wrapped past the start

    f.ok("newRig", obj({ { "name", "Unsaved" }, { "band", "My Tones" } }));
    f.ok("nextRig");
    CHECK(f.lastEvent("rigLoaded")["rig"]["name"].toString() == "Alpha"); // unsaved rig -> first
}

TEST_CASE("bridge: startup loads last rig from settings")
{
    Fixture f;
    f.ok("newRig", obj({ { "name", "Keeper" }, { "band", "Live" } }));
    f.ok("saveRig");
    const auto id = f.state()["rig"]["id"].toString();
    f.ok("newRig", obj({ { "name", "Throwaway" }, { "band", "Live" } })); // unsaved: not remembered

    f.start();
    auto s = f.state();
    CHECK(s["rig"]["id"].toString() == id);
    CHECK(s["rig"]["name"].toString() == "Keeper");
    CHECK(s["recents"].getArray()->getFirst().toString() == id);
}

TEST_CASE("bridge: loadRig with missing asset reports it in rigLoaded.report")
{
    Fixture f;
    auto doc = makeDefaultRig("Missing", "My Tones");
    for (auto& n : doc.nodes)
        if (n.type == NodeType::NamAmp)
            n.asset = AssetRef { "nam", "gone.nam", "00ff", std::nullopt };
    REQUIRE(RigStore(f.lib).save(doc).wasOk());

    f.ok("loadRig", obj({ { "id", doc.id } }));
    auto loaded = f.lastEvent("rigLoaded");
    CHECK(loaded["rig"]["id"].toString() == doc.id);
    CHECK(loaded["report"]["missingAssets"].getArray()->contains("gone.nam"));
    CHECK(loaded["report"]["placeholderNodeIds"].getArray()->size() == 1);
}

TEST_CASE("bridge: assets in the library can be assigned to nodes")
{
    Fixture f;
    REQUIRE(f.lib.getChildFile("IRs").createDirectory());
    const auto ir = f.lib.getChildFile("IRs").getChildFile("cab.wav");
    REQUIRE(fixture("ir_48k.wav").copyFileTo(ir));
    f.start(); // rescans the library at startup

    auto list = f.ok("listAssets", obj({ { "kind", "ir" } }));
    REQUIRE(list["assets"].getArray()->size() == 1);
    CHECK(list["assets"][0]["file"].toString() == ir.getFullPathName());
    CHECK(list["assets"][0]["sha256"].toString().length() == 64);

    const auto cab = f.nodeOfType("CabIR");
    f.ok("setAsset", obj({ { "id", cab }, { "file", ir.getFullPathName() } }));
    auto loaded = f.lastEvent("rigLoaded");
    CHECK(loaded["report"]["missingAssets"].getArray()->isEmpty());
    for (auto& n : *loaded["rig"]["nodes"].getArray())
        if (n["id"].toString() == cab)
            CHECK(n["asset"]["file"].toString() == "cab.wav");

    auto r = f.call("setAsset", obj({ { "id", cab }, { "file", "/nowhere/else.wav" } }));
    CHECK_FALSE(static_cast<bool>(r["ok"]));
}

TEST_CASE("bridge: corrupt rig loadRig replies error and restoreBackup recovers it")
{
    Fixture f;
    f.ok("newRig", obj({ { "name", "Fragile" }, { "band", "My Tones" } }));
    const auto id = f.state()["rig"]["id"].toString();
    f.ok("saveRig");
    f.ok("setParam", obj({ { "id", f.nodeOfType("Gate") }, { "name", "threshold" }, { "value", -30.0 } }));
    f.ok("saveRig"); // first version is now Fragile.amprig.bak.1

    const auto file = f.rigFile("My Tones", "Fragile");
    REQUIRE(file.replaceWithText("{ this is not json"));

    auto r = f.call("loadRig", obj({ { "id", id } }));
    CHECK_FALSE(static_cast<bool>(r["ok"]));
    CHECK(r["error"].toString().isNotEmpty());
    CHECK(file.loadFileAsString() == "{ this is not json"); // never overwritten by a failed load

    const auto loadsBefore = f.events("rigLoaded").size();
    f.ok("restoreBackup", obj({ { "id", id } }));
    CHECK(f.events("rigLoaded").size() == loadsBefore + 1);
    CHECK(f.lastEvent("rigLoaded")["rig"]["id"].toString() == id);
    CHECK(parseRig(file.loadFileAsString()).rig.has_value());
    CHECK(file.getSiblingFile("Fragile.amprig.corrupt").loadFileAsString() == "{ this is not json"); // parked, not lost
}

TEST_CASE("bridge: deleteRig of the current rig loads another")
{
    Fixture f;
    f.ok("newRig", obj({ { "name", "Alpha" }, { "band", "My Tones" } }));
    f.ok("saveRig");
    f.ok("newRig", obj({ { "name", "Bravo" }, { "band", "My Tones" } }));
    f.ok("saveRig");
    const auto bravo = f.state()["rig"]["id"].toString();
    f.ok("toggleFavorite", obj({ { "id", bravo } }));
    CHECK(f.state()["favorites"].getArray()->contains(bravo));

    f.ok("deleteRig", obj({ { "id", bravo } }));
    CHECK_FALSE(f.rigFile("My Tones", "Bravo").exists());
    CHECK(f.lastEvent("rigLoaded")["rig"]["name"].toString() == "Alpha");
    CHECK_FALSE(f.state()["favorites"].getArray()->contains(bravo));
}

TEST_CASE("bridge: user art image is not written into the rig file")
{
    Fixture f;
    f.ok("newRig", obj({ { "name", "Arty" }, { "band", "My Tones" } }));
    const auto rigId = f.state()["rig"]["id"].toString();
    const auto amp = f.nodeOfType("NamAmp");

    const auto image = f.dir.getChildFile("my-amp-photo.png");
    REQUIRE(image.replaceWithText("not really a png"));
    auto r = f.ok("setNodeArt", obj({ { "id", amp }, { "archetype", "british-stack" }, { "imageFile", image.getFullPathName() } }));
    const juce::File stored(r["art"]["image"].toString());
    CHECK(stored.existsAsFile());
    CHECK(stored.getParentDirectory() == f.lib.getChildFile("Art"));
    CHECK(r["art"]["archetype"].toString() == "british-stack");

    f.ok("saveRig");
    const auto text = f.rigFile("My Tones", "Arty").loadFileAsString();
    CHECK(text.contains("british-stack"));             // the archetype belongs to the rig
    CHECK_FALSE(text.contains("my-amp-photo"));        // the user image does not
    CHECK_FALSE(text.contains(stored.getFileName()));

    auto userArt = juce::JSON::parse(f.lib.getChildFile("Art").getChildFile("user-art.json"));
    CHECK(userArt[juce::Identifier(rigId + "/" + amp)].toString() == stored.getFileName());

    for (auto& n : *f.state()["rig"]["nodes"].getArray())
        if (n["id"].toString() == amp)
            CHECK(n["art"]["image"].toString() == stored.getFullPathName());
}

TEST_CASE("bridge: device restart rebuilds the plan at the new sample rate")
{
    Fixture f;
    Settings s;
    s.audioDeviceType = "Fake";
    s.audioDeviceName = "Fake Solo";
    s.sampleRate = 48000;
    s.bufferSize = 128;
    REQUIRE(f.data.createDirectory());
    REQUIRE(s.save(f.data.getChildFile("settings.json")));

    f.start(true);
    CHECK(f.engine->planSpec().sampleRate == 48000.0);
    CHECK(f.engine->planSpec().maxBlock == 128);
    CHECK(f.state()["deviceStatus"]["status"].toString() == "Running");

    auto r = f.ok("setSettings", obj({ { "sampleRate", 44100.0 }, { "bufferSize", 256 } }));
    CHECK(static_cast<double>(r["sampleRate"]) == 44100.0);
    CHECK(f.engine->planSpec().sampleRate == 44100.0);
    CHECK(f.engine->planSpec().maxBlock == 256);
    CHECK(f.events("error").empty());
    CHECK(Settings::load(f.data.getChildFile("settings.json")).sampleRate == 44100.0);
}
