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

    void start(bool withFakeDevice = false, bool deferAudio = false)
    {
        engine.reset();
        Engine::Options o;
        o.deferAudioStart = deferAudio;
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

    r = f.raw(obj({ { "v", 1 }, { "cmd", "getState" }, { "id", 100 }, { "args", juce::Array<juce::var> { 1, 2 } } }));
    CHECK_FALSE(static_cast<bool>(r["ok"])); // an array is not an args object

    r = f.call("setParam", obj({ { "id", f.nodeOfType("Gate") }, { "name", "" }, { "value", 1.0 } }));
    CHECK_FALSE(static_cast<bool>(r["ok"]));
    CHECK(r["error"].toString().contains("empty"));

    r = f.call("loadRig", obj({ { "id", "" } }));
    CHECK_FALSE(static_cast<bool>(r["ok"]));

    r = f.call("addNode", obj({ { "type", "Plugin" }, { "index", 1 }, { "plugin", juce::Array<juce::var> { "x" } } }));
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

TEST_CASE("bridge: assignFootswitchKey maps the next raw key, replaces the old one and persists")
{
    Fixture f;
    CHECK_FALSE(static_cast<bool>(f.call("assignFootswitchKey", obj({ { "control", "midi:x:cc1" } }))["ok"]));

    // A key seen without an assign pending only updates lastKey.
    f.engine->injectRawKey(0x2a);
    CHECK_FALSE(static_cast<bool>(f.lastEvent("controlStatus")["lastKey"].isVoid()));

    f.ok("assignFootswitchKey", obj({ { "control", "footswitch:R" } }));
    CHECK(f.lastEvent("controlStatus")["assigning"].toString() == "footswitch:R");
    f.engine->injectRawKey(0x2b);
    auto cs = f.lastEvent("controlStatus");
    CHECK(cs["assigning"].isVoid());
    CHECK(static_cast<int>(cs["lastKey"]) == 0x2b);

    // The old R key (default F15 / 0x06 etc.) is gone; 0x2b is R.
    auto keys = f.ok("getSettings")["footswitchKeys"];
    CHECK(keys["0x2b"].toString() == "footswitch:R");
    int rCount = 0;
    for (auto& p : keys.getDynamicObject()->getProperties())
        rCount += p.value.toString() == "footswitch:R";
    CHECK(rCount == 1);

    // Persisted to settings.json.
    CHECK(Settings::load(f.data.getChildFile("settings.json")).footswitchKeys.at(0x2b) == "footswitch:R");

    // Cancel clears a pending assign without changing the map.
    f.ok("assignFootswitchKey", obj({ { "control", "footswitch:L" } }));
    f.ok("cancelAssignFootswitchKey");
    CHECK(f.lastEvent("controlStatus")["assigning"].isVoid());
    f.engine->injectRawKey(0x30);
    CHECK(f.ok("getSettings")["footswitchKeys"]["0x30"].isVoid());
}

TEST_CASE("bridge: a pending footswitch assign is dropped when the pedal disconnects")
{
    Fixture f;
    f.engine->injectHidStatus(HidFootswitch::Status::Connected);
    f.ok("assignFootswitchKey", obj({ { "control", "footswitch:L" } }));
    CHECK(f.lastEvent("controlStatus")["assigning"].toString() == "footswitch:L");

    f.engine->injectHidStatus(HidFootswitch::Status::NotFound);
    CHECK(f.lastEvent("controlStatus")["assigning"].isVoid());

    f.engine->injectHidStatus(HidFootswitch::Status::Connected); // reconnect: the next stomp is a normal press
    f.engine->injectRawKey(0x41);
    CHECK(f.ok("getSettings")["footswitchKeys"]["0x41"].isVoid());
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

    // Restoring over a readable file keeps that file too.
    const auto current = file.loadFileAsString();
    f.ok("restoreBackup", obj({ { "id", id } }));
    CHECK(file.getSiblingFile("Fragile.amprig.replaced").loadFileAsString() == current);
}

TEST_CASE("bridge: rig corrupted between sessions is reported at startup and restorable")
{
    Fixture f;
    f.ok("newRig", obj({ { "name", "Fragile" }, { "band", "My Tones" } }));
    const auto id = f.state()["rig"]["id"].toString();
    f.ok("saveRig");
    f.ok("saveRig"); // Fragile.amprig.bak.1
    const auto file = f.rigFile("My Tones", "Fragile");
    REQUIRE(file.replaceWithText("{ this is not json"));

    f.start(); // fresh process state: nothing remembered from the session that saved it
    auto s = f.state();
    CHECK(s["rig"]["id"].toString() != id); // fell back to a new default rig
    REQUIRE(s["startupErrors"].getArray()->size() == 1);
    auto e = s["startupErrors"][0];
    CHECK(e["rigId"].toString() == id);
    CHECK(e["context"].toString() == "loadRig");
    CHECK(e["message"].toString().isNotEmpty());
    // The same problem also arrived as an error event, queued until the UI attached.
    REQUIRE(f.events("error").size() == 1);
    CHECK(f.events("error")[0]["rigId"].toString() == id);
    CHECK(file.loadFileAsString() == "{ this is not json");

    f.ok("restoreBackup", obj({ { "id", id } }));
    CHECK(f.lastEvent("rigLoaded")["rig"]["id"].toString() == id);
    CHECK(f.state()["rig"]["name"].toString() == "Fragile");
    CHECK(file.getSiblingFile("Fragile.amprig.corrupt").existsAsFile());
}

TEST_CASE("bridge: learnMapping replaces an armed learn and rejects unknown nodes")
{
    Fixture f;
    const auto gate = f.nodeOfType("Gate");
    f.ok("learnMapping", obj({ { "id", gate } }));
    CHECK_FALSE(static_cast<bool>(f.call("learnMapping", obj({ { "id", "no-such-node" } }))["ok"]));
    CHECK_FALSE(static_cast<bool>(f.state()["learning"]));
    f.engine->injectControl({ "footswitch:M", true });
    CHECK(f.events("learned").empty());
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
    CHECK(r["availableInputChannels"].size() == 1);
    CHECK(r["availableInputChannels"][0].toString() == "In 1");
    CHECK(r["availableOutputChannels"].size() == 2);
    CHECK(r["availableOutputChannels"][1].toString() == "Out 2");
    CHECK(f.engine->planSpec().sampleRate == 44100.0);
    CHECK(f.engine->planSpec().maxBlock == 256);
    CHECK(f.events("error").empty());
    CHECK(Settings::load(f.data.getChildFile("settings.json")).sampleRate == 44100.0);
}

namespace
{
void saveFakeSoloSettings(Fixture& f)
{
    Settings s;
    s.audioDeviceType = "Fake";
    s.audioDeviceName = "Fake Solo";
    s.sampleRate = 48000;
    s.bufferSize = 128;
    REQUIRE(f.data.createDirectory());
    REQUIRE(s.save(f.data.getChildFile("settings.json")));
}
} // namespace

TEST_CASE("bridge: deferred audio start opens the device only when startAudio is called")
{
    Fixture f;
    saveFakeSoloSettings(f);
    f.start(true, true);
    CHECK(f.state()["deviceStatus"]["status"].toString() == "NoDevice");

    f.engine->startAudio(true);
    CHECK(f.state()["deviceStatus"]["status"].toString() == "Running");
    CHECK(f.engine->planSpec().maxBlock == 128);
    CHECK(f.state()["startupErrors"].size() == 0);

    f.engine->startAudio(true); // only the first call counts
    CHECK(f.state()["deviceStatus"]["status"].toString() == "Running");
}

TEST_CASE("bridge: denied microphone permission is a startup error and leaves audio closed")
{
    Fixture f;
    saveFakeSoloSettings(f);
    f.start(true, true);
    f.engine->startAudio(false);

    auto st = f.state();
    CHECK(st["deviceStatus"]["status"].toString() == "NoDevice");
    REQUIRE(st["startupErrors"].size() == 1);
    CHECK(st["startupErrors"][0]["context"].toString() == "audioPermission");
    CHECK(st["startupErrors"][0]["message"].toString().contains("Privacy & Security"));
    CHECK(f.lastEvent("error")["context"].toString() == "audioPermission");
}

TEST_CASE("bridge: saveRig with name and band renames the rig and moves it to that band")
{
    Fixture f;
    const auto id = f.ok("newRig", obj({ { "name", "Draft" }, { "band", "Scratch" } }))["id"].toString();
    f.ok("saveRig");
    REQUIRE(f.rigFile("Scratch", "Draft").existsAsFile());

    auto r = f.ok("saveRig", obj({ { "name", "  Black Dog " }, { "band", "Zeppelin" } }));
    CHECK(r["id"].toString() == id);
    CHECK(r["name"].toString() == "Black Dog");
    CHECK(r["band"].toString() == "Zeppelin");
    CHECK(f.rigFile("Zeppelin", "Black Dog").existsAsFile());
    CHECK_FALSE(f.rigFile("Scratch", "Draft").existsAsFile());
    CHECK(f.state()["rig"]["name"].toString() == "Black Dog");

    auto list = f.ok("listRigs");
    REQUIRE(list["rigs"].getArray()->size() == 1);
    CHECK(list["rigs"][0]["band"].toString() == "Zeppelin");

    // Blank values keep the current name/band.
    r = f.ok("saveRig", obj({ { "name", " " }, { "band", "" } }));
    CHECK(r["name"].toString() == "Black Dog");
    CHECK(r["band"].toString() == "Zeppelin");
}

TEST_CASE("bridge: beforeNodesReplaced fires before rig loads, structural edits, not live edits")
{
    std::vector<bool> calls; // outlives the engine: ~Engine calls the hook too
    Fixture f;
    f.engine->beforeNodesReplaced = [&](bool sameRig) { calls.push_back(sameRig); };

    f.ok("setParam", obj({ { "id", f.nodeOfType("Gate") }, { "name", "threshold" }, { "value", -50.0 } }));
    f.ok("toggleBypass", obj({ { "id", f.nodeOfType("Gate") } }));
    CHECK(calls.empty());

    f.ok("addNode", obj({ { "type", "EQ" }, { "index", 1 } }));
    REQUIRE(calls.size() == 1);
    CHECK(calls.back() == true); // the same rig, rebuilt

    f.ok("newRig");
    REQUIRE(calls.size() == 2);
    CHECK(calls.back() == false); // another rig

    CHECK_FALSE(static_cast<bool>(f.call("moveNode", obj({ { "id", "nope" }, { "index", 1 } }))["ok"]));
    CHECK(calls.size() == 2); // a failed edit replaces nothing

    f.engine.reset();
    REQUIRE(calls.size() == 3);
    CHECK(calls.back() == false); // the engine going away destroys the nodes too
}

TEST_CASE("bridge: openPluginEditor hands the live plugin instance and node id to the app")
{
    Fixture f;
    CHECK_FALSE(static_cast<bool>(f.call("openPluginEditor", obj({ { "id", f.nodeOfType("Gate") } }))["ok"]));

    f.ok("rescanPlugins");
    juce::var gain;
    for (auto& p : *f.ok("listPlugins")["plugins"].getArray())
        if (p["name"].toString() == "FakeGain")
            gain = p;
    REQUIRE(gain.isObject());
    const auto id = f.ok("addNode", obj({ { "type", "Plugin" }, { "index", 0 }, { "plugin", gain } }))["id"].toString();

    CHECK_FALSE(static_cast<bool>(f.call("openPluginEditor", obj({ { "id", id } }))["ok"])); // no app hook yet

    juce::AudioPluginInstance* opened = nullptr;
    juce::String openedId;
    f.engine->onOpenPluginEditor = [&](juce::AudioPluginInstance* p, const juce::String& nodeId)
    {
        opened = p;
        openedId = nodeId;
    };
    f.ok("openPluginEditor", obj({ { "id", id } }));
    REQUIRE(opened != nullptr);
    CHECK(opened->getName() == "FakeGain");
    CHECK(openedId == id);
}

TEST_CASE("bridge: rigLoaded includes art.archetype for nodes that set it")
{
    Fixture f;
    const auto amp = f.nodeOfType("NamAmp");
    f.ok("setNodeArt", obj({ { "id", amp }, { "archetype", "high-gain-rectifier" } }));
    f.ok("moveNode", obj({ { "id", f.nodeOfType("Gate") }, { "index", 2 } })); // structural: rigLoaded

    for (auto& n : *f.lastEvent("rigLoaded")["rig"]["nodes"].getArray())
    {
        INFO(juce::JSON::toString(n, true));
        const auto art = n["art"];
        REQUIRE(art.isObject());
        CHECK(art["archetype"].toString() == (n["id"].toString() == amp ? "high-gain-rectifier" : ""));
        CHECK(art.hasProperty("image"));
        CHECK(art.hasProperty("snapshot"));
        CHECK(art["snapshot"].toString().isEmpty()); // no plugin nodes here
    }
}

TEST_CASE("bridge: plugin nodes carry a cached editor snapshot in their art")
{
    Fixture f;
    const amp_test::ScopedFakeEditor editor(amp_test::FakeEditorKind::Painted);
    f.ok("rescanPlugins");
    juce::var gain;
    for (auto& p : *f.ok("listPlugins")["plugins"].getArray())
        if (p["name"].toString() == "FakeGain")
            gain = p;
    REQUIRE(gain.isObject());
    const auto id = f.ok("addNode", obj({ { "type", "Plugin" }, { "index", 0 }, { "plugin", gain } }))["id"].toString();

    juce::String snapshot;
    for (auto& n : *f.lastEvent("rigLoaded")["rig"]["nodes"].getArray())
        if (n["id"].toString() == id)
            snapshot = n["art"]["snapshot"].toString();
    const juce::File png(snapshot);
    REQUIRE(png.existsAsFile());
    CHECK(png.getParentDirectory() == f.lib.getChildFile("Art").getChildFile("cache"));
    // ...and the web view can fetch it through the art resource path.
    CHECK(f.engine->artFileForRequest("/art/cache/" + png.getFileName()) == png);
}

TEST_CASE("bridge: art requests resolve only to images inside Art/")
{
    Fixture f;
    const auto art = f.lib.getChildFile("Art");
    REQUIRE(art.getChildFile("photo 1.png").replaceWithText("png"));
    REQUIRE(art.getChildFile("cache").getChildFile("p.png").create());
    REQUIRE(f.lib.getChildFile("secret.png").replaceWithText("x"));
    REQUIRE(f.data.getChildFile("settings.json").existsAsFile());

    CHECK(f.engine->artFileForRequest("/art/photo 1.png") == art.getChildFile("photo 1.png"));
    CHECK(f.engine->artFileForRequest("/art/cache/p.png") == art.getChildFile("cache").getChildFile("p.png"));

    for (auto bad : { "/art/../secret.png", "/art/cache/../../secret.png", "/art/..", "/art/", "/art",
                      "/art/user-art.json", "/art/missing.png", "/art//etc/passwd", "/art/~/x.png",
                      "/secret.png", "/art/..\\secret.png", "/art/C:/x.png", "/index.html" })
    {
        INFO(bad);
        CHECK(f.engine->artFileForRequest(bad) == juce::File());
    }

    // A symlink inside Art/ must not lead outside it.
    const auto link = art.getChildFile("link.png");
    REQUIRE(f.lib.getChildFile("secret.png").createSymbolicLink(link, true));
    CHECK(f.engine->artFileForRequest("/art/link.png") == juce::File());
    const auto dirLink = art.getChildFile("outside");
    REQUIRE(f.lib.createSymbolicLink(dirLink, true));
    CHECK(f.engine->artFileForRequest("/art/outside/secret.png") == juce::File());
}

TEST_CASE("bridge: user art images get a safe ASCII file name that the art path serves")
{
    Fixture f;
    const auto amp = f.nodeOfType("NamAmp");
    const auto image = f.dir.getChildFile(juce::String::fromUTF8("my photo \xc3\xa9.png"));
    REQUIRE(image.replaceWithText("png"));
    auto r = f.ok("setNodeArt", obj({ { "id", amp }, { "imageFile", image.getFullPathName() } }));
    const juce::File stored(r["art"]["image"].toString());
    REQUIRE(stored.existsAsFile());
    CHECK(stored.getParentDirectory() == f.lib.getChildFile("Art"));
    CHECK(stored.hasFileExtension("png"));
    const auto name = stored.getFileName();
    CHECK(name.containsOnly("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-"));
    CHECK(name.contains(amp));
    CHECK(f.engine->artFileForRequest("/art/" + name) == stored);

    // A second assignment never overwrites the first file.
    auto r2 = f.ok("setNodeArt", obj({ { "id", amp }, { "imageFile", image.getFullPathName() } }));
    CHECK(r2["art"]["image"].toString() != stored.getFullPathName());
}
