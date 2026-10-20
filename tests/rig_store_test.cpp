#include <catch2/catch_test_macros.hpp>

#include "storage/RigStore.h"

namespace
{
struct TempDir
{
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("amp_rig_store_" + juce::Uuid().toString());
    TempDir() { dir.createDirectory(); }
    ~TempDir() { dir.deleteRecursively(); }
};

amp::RigDocument rig(const juce::String& name, const juce::String& band = "My Tones")
{
    return amp::makeDefaultRig(name, band);
}
} // namespace

TEST_CASE("rig_store: save then load round trips")
{
    TempDir t;
    amp::RigStore store(t.dir);
    for (auto* d : { "Models", "IRs", "Rigs", "Art" })
        REQUIRE(t.dir.getChildFile(d).isDirectory());

    auto r = rig("Clean", "Band A");
    REQUIRE(store.save(r).wasOk());
    REQUIRE(t.dir.getChildFile("Rigs/Band A/Clean.amprig").existsAsFile());
    auto loaded = store.load(r.id);
    REQUIRE(loaded.rig.has_value());
    REQUIRE(amp::serializeRig(*loaded.rig) == amp::serializeRig(r));

    auto entries = store.list();
    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].id == r.id);
    REQUIRE(entries[0].name == "Clean");
    REQUIRE(entries[0].band == "Band A");
    REQUIRE(store.load("nope").error.isNotEmpty());
}

TEST_CASE("rig_store: illegal characters sanitized")
{
    TempDir t;
    amp::RigStore store(t.dir);
    auto r = rig("Guns N' Roses: Live?", "AC/DC");
    REQUIRE(store.save(r).wasOk());
    REQUIRE(t.dir.getChildFile("Rigs/AC_DC/Guns N' Roses_ Live_.amprig").existsAsFile());
    REQUIRE_FALSE(t.dir.getChildFile("Rigs/AC").exists());
    auto loaded = store.load(r.id);
    REQUIRE(loaded.rig.has_value());
    REQUIRE(loaded.rig->band == "AC/DC");
    REQUIRE(loaded.rig->name == "Guns N' Roses: Live?");
}

TEST_CASE("rig_store: sanitizeFileName rules")
{
    using amp::RigStore;
    REQUIRE(RigStore::sanitizeFileName("a/b\\c:d*e?f\"g<h>i|j") == "a_b_c_d_e_f_g_h_i_j");
    REQUIRE(RigStore::sanitizeFileName("a\tb") == "a_b");
    REQUIRE(RigStore::sanitizeFileName("  ..name.. ") == "name");
    REQUIRE(RigStore::sanitizeFileName("") == "Untitled");
    REQUIRE(RigStore::sanitizeFileName(" . ") == "Untitled");
    REQUIRE(RigStore::sanitizeFileName(juce::String::repeatedString("x", 300)).length() == 120);
}

TEST_CASE("rig_store: filename collision gets suffix")
{
    TempDir t;
    amp::RigStore store(t.dir);
    auto a = rig("A/B"), b = rig("A:B"), c = rig("A?B");
    REQUIRE(store.save(a).wasOk());
    REQUIRE(store.save(b).wasOk());
    REQUIRE(store.save(c).wasOk());
    auto band = t.dir.getChildFile("Rigs/My Tones");
    REQUIRE(band.getChildFile("A_B.amprig").existsAsFile());
    REQUIRE(band.getChildFile("A_B (2).amprig").existsAsFile());
    REQUIRE(band.getChildFile("A_B (3).amprig").existsAsFile());
    // Re-saving an existing rig keeps its own path.
    REQUIRE(store.save(b).wasOk());
    REQUIRE(band.getChildFile("A_B (2).amprig").existsAsFile());
    REQUIRE_FALSE(band.getChildFile("A_B (4).amprig").exists());
    REQUIRE(store.load(a.id).rig->name == "A/B");
    REQUIRE(store.load(b.id).rig->name == "A:B");
}

TEST_CASE("rig_store: sixth save keeps five backups")
{
    TempDir t;
    amp::RigStore store(t.dir);
    auto r = rig("v1");
    r.name = "Tone";
    for (int i = 1; i <= 6; ++i)
    {
        r.tags = { "save" + juce::String(i) };
        REQUIRE(store.save(r).wasOk());
    }
    auto file = t.dir.getChildFile("Rigs/My Tones/Tone.amprig");
    auto bak = store.backups(r.id);
    REQUIRE(bak.size() == 5);
    REQUIRE(bak[0].getFileName() == "Tone.amprig.bak.1");
    REQUIRE(bak[4].getFileName() == "Tone.amprig.bak.5");
    REQUIRE_FALSE(file.getSiblingFile("Tone.amprig.bak.6").exists());
    REQUIRE(amp::parseRig(bak[0].loadFileAsString()).rig->tags[0] == "save5");
    REQUIRE(amp::parseRig(bak[4].loadFileAsString()).rig->tags[0] == "save1");
    REQUIRE(amp::parseRig(file.loadFileAsString()).rig->tags[0] == "save6");
    REQUIRE(store.list().size() == 1); // backups and temp files are not rigs
}

TEST_CASE("rig_store: corrupt rig file returns error and is not overwritten")
{
    TempDir t;
    amp::RigStore store(t.dir);
    auto r = rig("Tone");
    REQUIRE(store.save(r).wasOk());
    auto file = t.dir.getChildFile("Rigs/My Tones/Tone.amprig");
    file.replaceWithText("{ corrupt");

    auto loaded = store.load(r.id);
    REQUIRE_FALSE(loaded.rig.has_value());
    REQUIRE(loaded.error.isNotEmpty());
    REQUIRE(file.loadFileAsString() == "{ corrupt");
    REQUIRE(store.list().empty());

    // A different rig with the same sanitized name must not claim the corrupt file.
    auto other = rig("Tone");
    REQUIRE(store.save(other).wasOk());
    REQUIRE(file.loadFileAsString() == "{ corrupt");
    REQUIRE(t.dir.getChildFile("Rigs/My Tones/Tone (2).amprig").existsAsFile());
}

TEST_CASE("rig_store: renaming a rig's band moves the file")
{
    TempDir t;
    amp::RigStore store(t.dir);
    auto r = rig("Tone", "Old Band");
    REQUIRE(store.save(r).wasOk());
    r.tags = { "second" };
    REQUIRE(store.save(r).wasOk()); // creates a backup
    auto oldFile = t.dir.getChildFile("Rigs/Old Band/Tone.amprig");
    REQUIRE(oldFile.existsAsFile());

    r.band = "New Band";
    REQUIRE(store.save(r).wasOk());
    REQUIRE_FALSE(oldFile.exists());
    REQUIRE_FALSE(oldFile.getSiblingFile("Tone.amprig.bak.1").exists());
    auto newFile = t.dir.getChildFile("Rigs/New Band/Tone.amprig");
    REQUIRE(newFile.existsAsFile());
    REQUIRE(newFile.getSiblingFile("Tone.amprig.bak.1").existsAsFile());
    REQUIRE(store.list().size() == 1);
    REQUIRE(store.load(r.id).rig->band == "New Band");
    REQUIRE(store.backups(r.id).size() == 1);
}

TEST_CASE("rig_store: renaming a rig moves the file")
{
    TempDir t;
    amp::RigStore store(t.dir);
    auto r = rig("First");
    REQUIRE(store.save(r).wasOk());
    r.name = "Second";
    REQUIRE(store.save(r).wasOk());
    REQUIRE_FALSE(t.dir.getChildFile("Rigs/My Tones/First.amprig").exists());
    REQUIRE(t.dir.getChildFile("Rigs/My Tones/Second.amprig").existsAsFile());
}

TEST_CASE("rig_store: remove deletes file and backups")
{
    TempDir t;
    amp::RigStore store(t.dir);
    auto r = rig("Tone");
    REQUIRE(store.save(r).wasOk());
    REQUIRE(store.save(r).wasOk());
    REQUIRE(store.backups(r.id).size() == 1);
    REQUIRE(store.remove(r.id).wasOk());
    REQUIRE(store.list().empty());
    REQUIRE(t.dir.getChildFile("Rigs/My Tones").findChildFiles(juce::File::findFiles, false).isEmpty());
    REQUIRE(store.remove(r.id).failed());
}

TEST_CASE("rig_store: list sorted by band then name")
{
    TempDir t;
    amp::RigStore store(t.dir);
    REQUIRE(store.save(rig("b", "Z")).wasOk());
    REQUIRE(store.save(rig("b", "A")).wasOk());
    REQUIRE(store.save(rig("a", "A")).wasOk());
    auto l = store.list();
    REQUIRE(l.size() == 3);
    REQUIRE((l[0].band == "A" && l[0].name == "a"));
    REQUIRE((l[1].band == "A" && l[1].name == "b"));
    REQUIRE(l[2].band == "Z");
}

TEST_CASE("rig_store: bands includes My Tones on empty library")
{
    TempDir t;
    amp::RigStore store(t.dir);
    REQUIRE(store.bands().contains("My Tones"));
    REQUIRE(store.save(rig("x", "Other")).wasOk());
    auto b = store.bands();
    REQUIRE(b.contains("My Tones"));
    REQUIRE(b.contains("Other"));
}
