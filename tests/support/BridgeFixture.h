#pragma once

// The bridge test fixture shared by bridge_test.cpp and bridge_online_test.cpp: an Engine on temp directories with
// fake plugins (and optionally a fake audio device), every bridge message captured in `sent`.

#include <catch2/catch_test_macros.hpp>

#include <juce_events/juce_events.h>

#include "Engine.h"
#include "bridge/Bridge.h"

#include "FakeAudioDevice.h"
#include "FakePlugins.h"

namespace amp_test
{
inline juce::var obj(std::initializer_list<std::pair<const char*, juce::var>> props = {})
{
    auto* o = new juce::DynamicObject();
    for (auto& [k, v] : props)
        o->setProperty(k, v);
    return juce::var(o);
}

inline juce::File fixture(const char* name)
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
    FakeType* fakeType = nullptr;              // owned by `manager`
    std::unique_ptr<amp::Engine> engine;
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
        amp::Engine::Options o;
        o.deferAudioStart = deferAudio;
        o.libraryRoot = lib;
        o.dataDir = data;
        o.startAudio = withFakeDevice;
        o.startControls = false;
        o.pluginFormats.push_back(std::make_unique<FakeFormat>());
        o.addDefaultPluginFormats = false;
        o.trashFile = [](const juce::File& f) { return f.deleteFile(); }; // never the user's Trash
        if (withFakeDevice)
        {
            manager = std::make_unique<juce::AudioDeviceManager>();
            auto type = std::make_unique<FakeType>();
            fakeType = type.get();
            manager->addAudioDeviceType(std::move(type)); // never touches CoreAudio
            o.deviceManager = manager.get();
        }
        engine = std::make_unique<amp::Engine>(std::move(o));
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

    // Sends a command without expecting its reply yet (deferred replies); returns its id.
    int post(const juce::String& cmd, const juce::var& args = obj())
    {
        const int id = nextId++;
        engine->bridge().handle(obj({ { "v", 1 }, { "cmd", cmd }, { "id", id }, { "args", args } }));
        return id;
    }

    // The reply to `id` sent so far, or void.
    juce::var replyTo(int id) const
    {
        for (auto& m : sent)
            if (m.hasProperty("reply") && m["reply"] == juce::var(id))
                return m;
        return {};
    }

    // Pumps the message loop until the reply to `id` arrives (fails after timeoutMs).
    juce::var waitReply(int id, int timeoutMs = 5000)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        while (replyTo(id).isVoid() && juce::Time::getMillisecondCounter() < deadline)
            juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        const auto r = replyTo(id);
        if (r.isVoid())
            FAIL("no reply for id " << id << " within " << timeoutMs << " ms");
        return r;
    }

    // call() for commands that reply later: sends, then pumps the message loop until the reply arrives.
    juce::var callLater(const juce::String& cmd, const juce::var& args = obj(), int timeoutMs = 5000)
    {
        return waitReply(post(cmd, args), timeoutMs);
    }

    // Pumps the message loop for about `ms` milliseconds.
    static void pump(int ms = 50)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
        while (juce::Time::getMillisecondCounter() < deadline)
            juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
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

    // rescanPlugins answers at once; the scan runs on a thread and ends with a pluginsChanged event, which this
    // waits for (pumping the message loop). Returns that event's data.
    juce::var rescan()
    {
        const auto before = events("pluginsChanged").size();
        REQUIRE(static_cast<bool>(ok("rescanPlugins")["scanning"]));
        const auto deadline = juce::Time::getMillisecondCounter() + 20000;
        while (events("pluginsChanged").size() == before && juce::Time::getMillisecondCounter() < deadline)
            juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
        REQUIRE(events("pluginsChanged").size() > before);
        return lastEvent("pluginsChanged");
    }

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
} // namespace amp_test
