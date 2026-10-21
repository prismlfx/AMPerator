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
    amp_test::FakeType* fakeType = nullptr;              // owned by `manager`
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
        o.trashFile = [](const juce::File& f) { return f.deleteFile(); }; // never the user's Trash
        if (withFakeDevice)
        {
            manager = std::make_unique<juce::AudioDeviceManager>();
            auto type = std::make_unique<amp_test::FakeType>();
            fakeType = type.get();
            manager->addAudioDeviceType(std::move(type)); // never touches CoreAudio
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

TEST_CASE("bridge: listPlugins and rescanPlugins report category, instrument flag and channel counts")
{
    Fixture f;
    const auto byName = [](const juce::var& list, const juce::String& name)
    {
        for (auto& p : *list.getArray())
            if (p["name"].toString() == name)
                return p;
        return juce::var();
    };
    const auto rescanned = f.ok("rescanPlugins")["plugins"];
    const auto listed = f.ok("listPlugins")["plugins"];
    for (const auto& list : { rescanned, listed })
    {
        const auto gain = byName(list, "FakeGain");
        REQUIRE(gain.isObject());
        CHECK(gain["format"].toString() == "Fake");
        CHECK(gain["vendor"].toString() == "Test Vendor");
        CHECK(gain["uid"].toString().isNotEmpty());
        CHECK(gain["category"].toString() == "Effect"); // PluginDescription::category, verbatim
        CHECK(gain["isInstrument"].isBool());
        CHECK_FALSE(static_cast<bool>(gain["isInstrument"]));
        CHECK(static_cast<int>(gain["numInputChannels"]) == 2);
        CHECK(static_cast<int>(gain["numOutputChannels"]) == 2);

        const auto mono = byName(list, "FakeMono");
        REQUIRE(mono.isObject());
        CHECK(mono["category"].toString() == "Fx|Distortion");
        CHECK(static_cast<int>(mono["numInputChannels"]) == 1);
        CHECK(static_cast<int>(mono["numOutputChannels"]) == 1);

        const auto sidechain = byName(list, "FakeSidechain");
        REQUIRE(sidechain.isObject());
        CHECK(static_cast<int>(sidechain["numInputChannels"]) == 4); // main + sidechain bus
        CHECK(static_cast<int>(sidechain["numOutputChannels"]) == 2);

        const auto synth = byName(list, "FakeSynth");
        REQUIRE(synth.isObject());
        CHECK(synth["category"].toString() == "Instrument|Synth");
        CHECK(static_cast<bool>(synth["isInstrument"]));
        CHECK(static_cast<int>(synth["numInputChannels"]) == 0);
        CHECK(static_cast<int>(synth["numOutputChannels"]) == 2);
    }

    // The richer object is still accepted as addNode's plugin reference.
    f.ok("addNode", obj({ { "type", "Plugin" }, { "index", 0 }, { "plugin", byName(listed, "FakeMono") } }));
    CHECK(f.lastEvent("rigLoaded")["report"]["placeholderNodeIds"].getArray()->isEmpty());
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
    CHECK(f.events("rigDirty").empty()); // bypass is a performance state, not an edit
    CHECK_FALSE(static_cast<bool>(f.state()["dirty"]));

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

namespace
{
juce::var fakeGain(Fixture& f)
{
    f.ok("rescanPlugins");
    for (auto& p : *f.ok("listPlugins")["plugins"].getArray())
        if (p["name"].toString() == "FakeGain")
            return p;
    FAIL("FakeGain not found");
    return {};
}
} // namespace

TEST_CASE("bridge: a parameter changed in a plugin's editor marks the rig dirty")
{
    Fixture f;
    const auto id = f.ok("addNode", obj({ { "type", "Plugin" }, { "index", 0 }, { "plugin", fakeGain(f) } }))["id"].toString();
    f.ok("saveRig");
    REQUIRE_FALSE(static_cast<bool>(f.state()["dirty"]));
    f.engine->dispatchPendingPluginChanges(); // loading/restoring the plugin's state is not an edit
    REQUIRE_FALSE(static_cast<bool>(f.state()["dirty"]));

    juce::AudioPluginInstance* instance = nullptr;
    f.engine->onOpenPluginEditor = [&](juce::AudioPluginInstance* p, const juce::String&) { instance = p; };
    f.ok("openPluginEditor", obj({ { "id", id } }));
    REQUIRE(instance != nullptr);
    instance->getParameters()[0]->setValueNotifyingHost(0.8f); // what the plugin's editor does
    f.engine->dispatchPendingPluginChanges();                  // the message loop delivers the notification
    CHECK(static_cast<bool>(f.state()["dirty"]));
    CHECK(static_cast<bool>(f.lastEvent("rigDirty")["dirty"]));
}

TEST_CASE("bridge: quitting with unsaved changes asks the UI; confirmQuit saves, discards or cancels")
{
    Fixture f;
    int quits = 0;
    f.engine->onQuitConfirmed = [&] { ++quits; };
    CHECK(f.engine->requestQuit()); // clean: quit right away, nothing asked
    CHECK(f.events("quitRequested").empty());

    const auto rigId = f.ok("newRig", obj({ { "name", "Q" }, { "band", "B" } }))["id"].toString();
    f.ok("setParam", obj({ { "id", f.nodeOfType("Gate") }, { "name", "threshold" }, { "value", -40.0 } }));
    REQUIRE(static_cast<bool>(f.state()["dirty"]));

    CHECK_FALSE(f.engine->requestQuit());
    CHECK(f.events("quitRequested").size() == 1);
    CHECK(f.engine->quitPending());
    f.ok("confirmQuit", obj({ { "action", "cancel" } }));
    CHECK(quits == 0);
    CHECK_FALSE(f.engine->quitPending());

    CHECK_FALSE(static_cast<bool>(f.call("confirmQuit", obj({ { "action", "explode" } }))["ok"]));

    CHECK_FALSE(f.engine->requestQuit());
    f.ok("confirmQuit", obj({ { "action", "save" } }));
    CHECK(quits == 1);
    REQUIRE(f.rigFile("B", "Q").existsAsFile());
    auto parsed = RigStore(f.lib).load(rigId);
    REQUIRE(parsed.rig.has_value());

    f.ok("setParam", obj({ { "id", f.nodeOfType("Gate") }, { "name", "threshold" }, { "value", -30.0 } }));
    CHECK_FALSE(f.engine->requestQuit());
    f.ok("confirmQuit", obj({ { "action", "discard" } }));
    CHECK(quits == 2);
    for (auto& n : RigStore(f.lib).load(rigId).rig->nodes)
        if (n.type == NodeType::Gate)
            CHECK(static_cast<double>(n.params["threshold"]) == -40.0); // the discarded edit was not saved
}

TEST_CASE("bridge: saveRig asNew writes a copy with a fresh id and leaves the original untouched")
{
    Fixture f;
    const auto id = f.ok("newRig", obj({ { "name", "Lead" }, { "band", "Band" } }))["id"].toString();
    f.ok("saveRig");
    const auto originalText = f.rigFile("Band", "Lead").loadFileAsString();
    f.ok("setParam", obj({ { "id", f.nodeOfType("Gate") }, { "name", "threshold" }, { "value", -33.0 } }));

    auto r = f.ok("saveRig", obj({ { "asNew", true }, { "name", "Lead copy" }, { "band", "Other" } }));
    const auto copyId = r["id"].toString();
    CHECK(copyId.isNotEmpty());
    CHECK(copyId != id);
    CHECK(r["name"].toString() == "Lead copy");
    CHECK(f.rigFile("Band", "Lead").loadFileAsString() == originalText);
    REQUIRE(f.rigFile("Other", "Lead copy").existsAsFile());
    auto copy = RigStore(f.lib).load(copyId);
    REQUIRE(copy.rig.has_value());
    for (auto& n : copy.rig->nodes)
        if (n.type == NodeType::Gate)
            CHECK(static_cast<double>(n.params["threshold"]) == -33.0);

    auto st = f.state();
    CHECK(st["rig"]["id"].toString() == copyId); // the copy is now the current rig
    CHECK_FALSE(static_cast<bool>(st["dirty"]));
    CHECK(static_cast<bool>(st["saved"]));
    CHECK(f.lastEvent("rigLoaded")["rig"]["id"].toString() == copyId);
    CHECK(f.ok("listRigs")["rigs"].size() == 2);

    // Without a name the copy keeps the rig's name; the store picks a free file name.
    const auto third = f.ok("saveRig", obj({ { "asNew", true } }))["id"].toString();
    CHECK(third != copyId);
    CHECK(f.rigFile("Other", "Lead copy (2)").existsAsFile());
}

TEST_CASE("bridge: starting without the preferred device keeps it in the settings and switches back when it appears")
{
    Fixture f;
    Settings s;
    s.audioDeviceType = "Fake";
    s.audioDeviceName = "Fake Solo";
    s.outputFirstChannel = 1;
    s.sampleRate = 48000;
    s.bufferSize = 128;
    REQUIRE(f.data.createDirectory());
    REQUIRE(s.save(f.data.getChildFile("settings.json")));

    f.start(true, true);
    f.fakeType->removeDevice("Fake Solo");
    f.engine->startAudio(true);
    REQUIRE(f.manager->getCurrentAudioDevice() != nullptr);
    CHECK(f.manager->getCurrentAudioDevice()->getName() == "Fake Other"); // the fallback runs...
    CHECK(f.state()["deviceStatus"]["status"].toString() == "Running");
    auto saved = Settings::load(f.data.getChildFile("settings.json"));
    CHECK(saved.audioDeviceName == "Fake Solo"); // ...but the preferred device is kept
    CHECK(saved.outputFirstChannel == 1);
    CHECK(f.ok("getSettings")["audioDeviceName"].toString() == "Fake Solo");

    // A settings change that does not name a device keeps the preference too.
    f.ok("setSettings", obj({ { "bufferSize", 256 } }));
    saved = Settings::load(f.data.getChildFile("settings.json"));
    CHECK(saved.audioDeviceName == "Fake Solo");
    CHECK(saved.bufferSize == 256);

    f.fakeType->addDevice("Fake Solo");
    f.fakeType->notifyDeviceListChanged();
    f.manager->sendSynchronousChangeMessage();
    auto* dev = f.manager->getCurrentAudioDevice();
    REQUIRE(dev != nullptr);
    CHECK(dev->getName() == "Fake Solo");
    CHECK(dev->getActiveOutputChannels() == juce::BigInteger(2)); // the saved output channel, not the fallback's 0
    CHECK(f.state()["deviceStatus"]["status"].toString() == "Running");
}

TEST_CASE("bridge: setSettings retries opening the device when it is not running, even with unchanged fields")
{
    Fixture f;
    saveFakeSoloSettings(f);
    f.start(true, true);
    f.fakeType->failOpen = true;
    f.engine->startAudio(true);
    CHECK(f.state()["deviceStatus"]["status"].toString() == "NoDevice");

    f.fakeType->failOpen = false;
    f.ok("setSettings", obj({ { "audioDeviceName", "Fake Solo" } })); // same value as before
    REQUIRE(f.state()["deviceStatus"]["status"].toString() == "Running");
    CHECK(f.manager->getCurrentAudioDevice()->getName() == "Fake Solo");
}

TEST_CASE("bridge: assets dropped into the library after start are found without a relaunch")
{
    Fixture f;
    CHECK(f.ok("listAssets", obj({ { "kind", "nam" } }))["assets"].size() == 0);
    const auto model = f.lib.getChildFile("Models").getChildFile("late.nam");
    REQUIRE(fixture("tiny.nam").copyFileTo(model));
    auto assets = f.ok("listAssets", obj({ { "kind", "nam" } }))["assets"];
    REQUIRE(assets.size() == 1);
    CHECK(assets[0]["file"].toString() == model.getFullPathName());

    // setAsset resolves against a fresh scan too (no listAssets in between).
    const auto ir = f.lib.getChildFile("IRs").getChildFile("late.wav");
    REQUIRE(fixture("ir_48k.wav").copyFileTo(ir));
    f.ok("setAsset", obj({ { "id", f.nodeOfType("CabIR") }, { "file", ir.getFullPathName() } }));
    CHECK(f.lastEvent("rigLoaded")["report"]["missingAssets"].size() == 0);
}

TEST_CASE("bridge: a plugin blacklisted after a crash is reported at startup and can be retried")
{
    Fixture f;
    const auto gain = fakeGain(f); // scans: the plugin is known from now on
    const auto uid = gain["uid"].toString();
    CHECK(f.ok("listBlacklisted")["plugins"].size() == 0);

    f.engine.reset();
    REQUIRE(f.data.getChildFile("crash-marker").replaceWithText(uid)); // the last run died inside that plugin
    f.start();

    auto st = f.state();
    juce::var entry;
    for (auto& e : *st["startupErrors"].getArray())
        if (e["context"].toString() == "pluginBlacklisted")
            entry = e;
    REQUIRE(entry.isObject());
    CHECK(entry["message"].toString() == "FakeGain crashed last time and was disabled");
    CHECK(entry["pluginId"].toString() == uid);

    auto listed = f.ok("listBlacklisted")["plugins"];
    REQUIRE(listed.size() == 1);
    CHECK(listed[0]["id"].toString() == uid);
    CHECK(listed[0]["name"].toString() == "FakeGain");
    for (auto& p : *f.ok("listPlugins")["plugins"].getArray())
        CHECK(p["uid"].toString() != uid);

    auto r = f.ok("unblacklistPlugin", obj({ { "id", uid } }));
    CHECK(r["plugins"].size() == 0);
    CHECK(f.ok("listBlacklisted")["plugins"].size() == 0);
    bool available = false;
    for (auto& p : *f.ok("listPlugins")["plugins"].getArray())
        available = available || p["uid"].toString() == uid;
    CHECK(available);
    CHECK_FALSE(static_cast<bool>(f.call("unblacklistPlugin", obj({ { "id", "" } }))["ok"]));
}

TEST_CASE("bridge: the footswitch device can be picked manually and is persisted")
{
    Fixture f;
    CHECK(f.ok("listHidKeyboards")["keyboards"].size() == 0); // controls are off in tests: nothing is enumerated
    CHECK(f.ok("getSettings")["footswitchDevice"].isVoid());

    auto r = f.ok("setSettings", obj({ { "footswitchDevice", obj({ { "vid", 0x1234 }, { "pid", 0xabcd } }) } }));
    CHECK(static_cast<int>(r["footswitchDevice"]["vid"]) == 0x1234);
    CHECK(static_cast<int>(r["footswitchDevice"]["pid"]) == 0xabcd);
    auto saved = Settings::load(f.data.getChildFile("settings.json"));
    REQUIRE(saved.footswitchDevice.has_value());
    CHECK(saved.footswitchDevice->first == 0x1234);

    CHECK_FALSE(static_cast<bool>(f.call("setSettings", obj({ { "footswitchDevice", obj({ { "vid", 70000 }, { "pid", 1 } }) } }))["ok"]));

    r = f.ok("setSettings", obj({ { "footswitchDevice", juce::var() } }));
    CHECK(r["footswitchDevice"].isVoid());
    CHECK_FALSE(Settings::load(f.data.getChildFile("settings.json")).footswitchDevice.has_value());
}

TEST_CASE("bridge: stomping pedals never marks the rig dirty, but an explicit save keeps the bypass state")
{
    Fixture f;
    const auto id = f.ok("newRig", obj({ { "name", "Live" } }))["id"].toString();
    f.ok("saveRig");
    const auto gate = f.nodeOfType("Gate");
    f.ok("learnMapping", obj({ { "id", gate } }));
    f.engine->injectControl({ "footswitch:L", true }); // learned: an edit
    f.ok("saveRig");
    REQUIRE_FALSE(static_cast<bool>(f.state()["dirty"]));

    f.sent.clear();
    f.engine->injectControl({ "footswitch:L", true });
    CHECK(static_cast<bool>(f.lastEvent("bypassChanged")["bypass"]));
    CHECK(f.events("rigDirty").empty());
    CHECK_FALSE(static_cast<bool>(f.state()["dirty"]));
    CHECK(f.engine->requestQuit()); // nothing to ask about

    f.ok("saveRig");
    for (auto& n : RigStore(f.lib).load(id).rig->nodes)
        if (n.id == gate)
            CHECK(n.bypass);
}

TEST_CASE("bridge: clearMapping removes the rig's mapping of a control")
{
    Fixture f;
    const auto gate = f.nodeOfType("Gate");
    f.ok("learnMapping", obj({ { "id", gate } }));
    f.engine->injectControl({ "footswitch:M", true });
    f.ok("saveRig");
    REQUIRE(f.state()["rig"]["mappings"].size() == 1);

    f.ok("clearMapping", obj({ { "control", "footswitch:M" } }));
    CHECK(f.lastEvent("mappingCleared")["control"].toString() == "footswitch:M");
    CHECK(f.state()["rig"]["mappings"].size() == 0);
    CHECK(static_cast<bool>(f.state()["dirty"]));
    f.sent.clear();
    f.engine->injectControl({ "footswitch:M", true }); // no longer does anything
    CHECK(f.events("bypassChanged").empty());
    CHECK_FALSE(static_cast<bool>(f.call("clearMapping", obj({ { "control", "footswitch:M" } }))["ok"]));
}

TEST_CASE("bridge: removing the node an armed learn waits for emits learnCancelled")
{
    Fixture f;
    const auto gate = f.nodeOfType("Gate");
    f.ok("learnMapping", obj({ { "id", gate } }));
    f.ok("removeNode", obj({ { "id", gate } }));
    CHECK(f.events("learnCancelled").size() == 1);
    CHECK_FALSE(static_cast<bool>(f.state()["learning"]));
}

TEST_CASE("bridge: opening the device at the plan's rate with a smaller buffer does not rebuild the rig")
{
    Fixture f;
    saveFakeSoloSettings(f); // 48 kHz, 128 samples; the startup plan is 48 kHz / 512
    f.start(true, true);
    int rebuilds = 0;
    f.engine->beforeNodesReplaced = [&](bool) { ++rebuilds; };
    f.engine->startAudio(true);
    CHECK(f.state()["deviceStatus"]["status"].toString() == "Running");
    CHECK(rebuilds == 0);
    f.ok("setSettings", obj({ { "sampleRate", 44100.0 } })); // another rate still rebuilds
    CHECK(rebuilds == 1);
    f.engine->beforeNodesReplaced = nullptr;
}

TEST_CASE("bridge: the Output node's level is the master volume")
{
    Fixture f;
    const auto id = f.ok("newRig", obj({ { "name", "Master" } }))["id"].toString();
    const auto out = f.nodeOfType("Output");
    auto r = f.ok("setParam", obj({ { "id", out }, { "name", "level" }, { "value", -6.0 } }));
    CHECK(static_cast<double>(r["value"]) == -6.0);
    CHECK(static_cast<double>(f.ok("setParam", obj({ { "id", out }, { "name", "level" }, { "value", 50.0 } }))["value"]) == 12.0);
    f.ok("setParam", obj({ { "id", out }, { "name", "level" }, { "value", -6.0 } }));
    CHECK_FALSE(static_cast<bool>(f.call("toggleBypass", obj({ { "id", out } }))["ok"])); // never bypassed

    for (auto& n : *f.state()["rig"]["nodes"].getArray())
        if (n["id"].toString() == out)
            CHECK(static_cast<double>(n["params"]["level"]) == -6.0);
    f.ok("saveRig");
    for (auto& n : RigStore(f.lib).load(id).rig->nodes)
        if (n.id == out)
            CHECK(static_cast<double>(n.params["level"]) == -6.0);
}

namespace
{
juce::var tagList(std::initializer_list<juce::String> tags)
{
    juce::Array<juce::var> arr;
    for (auto& t : tags)
        arr.add(t);
    return arr;
}

juce::StringArray asStrings(const juce::var& arr)
{
    juce::StringArray out;
    if (auto* a = arr.getArray())
        for (auto& v : *a)
            out.add(v.toString());
    return out;
}
} // namespace

TEST_CASE("bridge: setTags trims and dedupes, marks the rig dirty, and the tags survive saveRig and a fresh load")
{
    Fixture f;
    const auto id = f.ok("newRig", obj({ { "name", "Peavey 5150 - Djent" }, { "band", "Metal" } }))["id"].toString();
    CHECK(asStrings(f.state()["rig"]["tags"]).isEmpty()); // the rig payload always carries tags
    f.ok("saveRig");
    f.sent.clear();

    auto r = f.ok("setTags", obj({ { "tags", tagList({ "  Parkway Drive ", "Wild Eyes", "parkway drive", "metal" }) } }));
    const juce::StringArray expected { "Parkway Drive", "Wild Eyes", "metal" };
    CHECK(asStrings(r["tags"]) == expected); // first spelling wins
    CHECK(asStrings(f.lastEvent("tagsChanged")["tags"]) == expected);
    CHECK(static_cast<bool>(f.lastEvent("rigDirty")["dirty"]));
    CHECK(asStrings(f.state()["rig"]["tags"]) == expected);
    CHECK(f.events("rigLoaded").empty()); // a tag edit does not rebuild the rig

    // Setting the same list again changes nothing.
    f.ok("saveRig");
    f.sent.clear();
    f.ok("setTags", obj({ { "tags", tagList({ "Parkway Drive", "Wild Eyes", "metal" }) } }));
    CHECK(f.events("rigDirty").empty());
    CHECK(f.events("tagsChanged").empty());

    REQUIRE(RigStore(f.lib).load(id).rig.has_value());
    CHECK(RigStore(f.lib).load(id).rig->tags == expected);

    f.start(); // a fresh engine loads the last rig from disk
    CHECK(f.state()["rig"]["id"].toString() == id);
    CHECK(asStrings(f.state()["rig"]["tags"]) == expected);
    f.ok("loadRig", obj({ { "id", id } }));
    CHECK(asStrings(f.lastEvent("rigLoaded")["rig"]["tags"]) == expected);

    // An empty list clears them.
    CHECK(asStrings(f.ok("setTags", obj({ { "tags", juce::Array<juce::var>() } }))["tags"]).isEmpty());
    CHECK(asStrings(f.state()["rig"]["tags"]).isEmpty());
}

TEST_CASE("bridge: setTags rejects invalid tags with a clear message and leaves the rig unchanged")
{
    Fixture f;
    f.ok("newRig", obj({ { "name", "Validate" } }));
    f.ok("setTags", obj({ { "tags", tagList({ "clean" }) } }));

    auto fails = [&](const juce::var& args, const juce::String& messagePart)
    {
        auto r = f.call("setTags", args);
        INFO(juce::JSON::toString(r));
        CHECK_FALSE(static_cast<bool>(r["ok"]));
        CHECK(r["error"].toString().containsIgnoreCase(messagePart));
    };
    fails(obj(), "tags");                                                            // missing
    fails(obj({ { "tags", "clean" } }), "array");                                     // not an array
    fails(obj({ { "tags", juce::Array<juce::var> { "ok", 3 } } }), "array of strings"); // not all strings
    fails(obj({ { "tags", tagList({ "ok", "   " }) } }), "empty");
    fails(obj({ { "tags", tagList({ juce::String::repeatedString("x", 41) }) } }), "40");
    fails(obj({ { "tags", tagList({ "line\nbreak" }) } }), "control");
    fails(obj({ { "tags", tagList({ juce::String("tab\there") }) } }), "control");

    juce::Array<juce::var> many;
    for (int i = 0; i < 33; ++i)
        many.add("tag " + juce::String(i));
    fails(obj({ { "tags", many } }), "32");

    CHECK(asStrings(f.state()["rig"]["tags"]) == juce::StringArray { "clean" });

    // The limits themselves are fine; duplicates do not count towards the maximum.
    many.removeLast(2);
    many.add("TAG 0");
    many.add(juce::String::repeatedString("y", 40));
    CHECK(f.ok("setTags", obj({ { "tags", many } }))["tags"].size() == 32);
}

TEST_CASE("bridge: listRigs carries each rig's tags and listTags counts tags across the library")
{
    Fixture f;
    auto saveTagged = [&](const juce::String& band, const juce::String& name, std::initializer_list<juce::String> tags)
    {
        f.ok("newRig", obj({ { "name", name }, { "band", band } }));
        f.ok("setTags", obj({ { "tags", tagList(tags) } }));
        f.ok("saveRig");
    };
    saveTagged("A Band", "One", { "Clean", "70s" });
    saveTagged("B Band", "Two", { "clean", "Metal" });
    saveTagged("C Band", "Three", { "CLEAN", "metal", "Wild Eyes" });
    f.ok("newRig", obj({ { "name", "Unsaved" } })); // not in the library
    f.ok("setTags", obj({ { "tags", tagList({ "zzz" }) } }));

    auto rigs = f.ok("listRigs")["rigs"];
    REQUIRE(rigs.size() == 3);
    CHECK(rigs[0]["name"].toString() == "One");
    CHECK(asStrings(rigs[0]["tags"]) == juce::StringArray { "Clean", "70s" });
    CHECK(asStrings(rigs[2]["tags"]) == juce::StringArray { "CLEAN", "metal", "Wild Eyes" });

    auto tags = f.ok("listTags")["tags"];
    REQUIRE(tags.size() == 4);
    auto entry = [&](int i) { return tags[i]["tag"].toString() + "=" + tags[i]["count"].toString(); };
    CHECK(entry(0) == "Clean=3"); // first-seen spelling (library order: band, then name)
    CHECK(entry(1) == "Metal=2");
    CHECK(entry(2) == "70s=1");   // equal counts: by name
    CHECK(entry(3) == "Wild Eyes=1");
}
