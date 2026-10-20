#include <catch2/catch_test_macros.hpp>

#include "storage/Settings.h"

namespace
{
struct TempDir
{
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("amp_settings_" + juce::Uuid().toString());
    TempDir() { dir.createDirectory(); }
    ~TempDir() { dir.deleteRecursively(); }
};
} // namespace

TEST_CASE("settings: defaults when file missing")
{
    TempDir t;
    auto s = amp::Settings::load(t.dir.getChildFile("settings.json"));
    REQUIRE(s.audioDeviceName.isEmpty());
    REQUIRE(s.inputChannel == 0);
    REQUIRE(s.outputFirstChannel == 0);
    REQUIRE(s.sampleRate == 48000.0);
    REQUIRE(s.bufferSize == 128);
    REQUIRE(s.recentRigIds.isEmpty());
    REQUIRE(s.globalMappings.empty());
}

TEST_CASE("settings: corrupt file yields defaults")
{
    TempDir t;
    auto f = t.dir.getChildFile("settings.json");
    f.replaceWithText("{ not json at all");
    REQUIRE(amp::Settings::load(f).bufferSize == 128);
    f.replaceWithText("[1,2,3]");
    REQUIRE(amp::Settings::load(f).sampleRate == 48000.0);
}

TEST_CASE("settings: wrong-typed values fall back per field")
{
    TempDir t;
    auto f = t.dir.getChildFile("settings.json");
    f.replaceWithText(R"({"bufferSize":"big","sampleRate":44100,"audioDeviceName":5,"lastRigId":"r1"})");
    auto s = amp::Settings::load(f);
    REQUIRE(s.bufferSize == 128);
    REQUIRE(s.sampleRate == 44100.0);
    REQUIRE(s.audioDeviceName.isEmpty());
    REQUIRE(s.lastRigId == "r1");
}

TEST_CASE("settings: save then load round trips")
{
    TempDir t;
    auto f = t.dir.getChildFile("settings.json");
    amp::Settings s;
    s.audioDeviceType = "CoreAudio";
    s.audioDeviceName = "Scarlett Solo";
    s.inputChannel = 1;
    s.outputFirstChannel = 2;
    s.sampleRate = 44100;
    s.bufferSize = 64;
    s.watchedFolders = { "/a", "/b" };
    s.globalMappings = { { "footswitch:L", "toggleBypass", "n1" } };
    s.lastRigId = "r9";
    s.recentRigIds = { "r9", "r1" };
    s.favoriteRigIds = { "r1" };
    REQUIRE(s.save(f));
    REQUIRE_FALSE(f.getSiblingFile("settings.json.tmp").exists());

    auto l = amp::Settings::load(f);
    REQUIRE(l.audioDeviceName == "Scarlett Solo");
    REQUIRE(l.inputChannel == 1);
    REQUIRE(l.outputFirstChannel == 2);
    REQUIRE(l.sampleRate == 44100.0);
    REQUIRE(l.bufferSize == 64);
    REQUIRE(l.watchedFolders == s.watchedFolders);
    REQUIRE(l.globalMappings.size() == 1);
    REQUIRE(l.globalMappings[0].control == "footswitch:L");
    REQUIRE(l.globalMappings[0].node == "n1");
    REQUIRE(l.recentRigIds == s.recentRigIds);
    REQUIRE(l.favoriteRigIds == s.favoriteRigIds);
}

TEST_CASE("settings: recent list capped at 10")
{
    amp::Settings s;
    for (int i = 0; i < 15; ++i)
        s.noteRecent("r" + juce::String(i));
    REQUIRE(s.recentRigIds.size() == 10);
    REQUIRE(s.recentRigIds[0] == "r14");
    REQUIRE(s.recentRigIds[9] == "r5");

    s.noteRecent("r7"); // dedupes and moves to front
    REQUIRE(s.recentRigIds.size() == 10);
    REQUIRE(s.recentRigIds[0] == "r7");
    REQUIRE(s.recentRigIds.indexOf("r7", false, 1) == -1);

    TempDir t;
    auto f = t.dir.getChildFile("settings.json");
    juce::String json = R"({"recentRigIds":[)";
    for (int i = 0; i < 14; ++i)
        json += (i ? "," : "") + juce::String("\"x") + juce::String(i) + "\"";
    f.replaceWithText(json + "]}");
    REQUIRE(amp::Settings::load(f).recentRigIds.size() == 10);
}

TEST_CASE("settings: out-of-range numbers and malformed mappings are rejected")
{
    TempDir t;
    auto f = t.dir.getChildFile("settings.json");
    f.replaceWithText(R"({"bufferSize":1e300,"sampleRate":-5,"inputChannel":1e30,"outputFirstChannel":2,
        "globalMappings":[{"control":"footswitch:L","action":"toggleBypass"},{"control":5},{"action":"x"},"str",{}]})");
    auto s = amp::Settings::load(f);
    REQUIRE(s.bufferSize == 128);
    REQUIRE(s.sampleRate == 48000.0);
    REQUIRE(s.inputChannel == 0);
    REQUIRE(s.outputFirstChannel == 2);
    REQUIRE(s.globalMappings.size() == 1);
    REQUIRE(s.globalMappings[0].control == "footswitch:L");
}

TEST_CASE("settings: a corrupt file is kept as settings.json.corrupt")
{
    TempDir t;
    auto f = t.dir.getChildFile("settings.json");
    f.replaceWithText("{ not json at all");
    REQUIRE(amp::Settings::load(f).bufferSize == 128);
    const auto kept = t.dir.getChildFile("settings.json.corrupt");
    REQUIRE(kept.existsAsFile());
    REQUIRE(kept.loadFileAsString() == "{ not json at all");

    // A second corrupt file does not overwrite the first copy.
    f.replaceWithText("[1,2,3]");
    amp::Settings::load(f);
    REQUIRE(kept.loadFileAsString() == "{ not json at all");
    REQUIRE(t.dir.getChildFile("settings.json.corrupt.2").loadFileAsString() == "[1,2,3]");
}

TEST_CASE("settings: the manually chosen footswitch device round trips")
{
    TempDir t;
    auto f = t.dir.getChildFile("settings.json");
    amp::Settings s;
    REQUIRE_FALSE(s.footswitchDevice.has_value());
    s.footswitchDevice = std::make_pair<uint16_t, uint16_t>(0x1234, 0xabcd);
    REQUIRE(s.save(f));
    auto loaded = amp::Settings::load(f);
    REQUIRE(loaded.footswitchDevice.has_value());
    REQUIRE(loaded.footswitchDevice->first == 0x1234);
    REQUIRE(loaded.footswitchDevice->second == 0xabcd);

    f.replaceWithText(R"({"footswitchDevice": {"vid": "x", "pid": 3}})");
    REQUIRE_FALSE(amp::Settings::load(f).footswitchDevice.has_value());
}
