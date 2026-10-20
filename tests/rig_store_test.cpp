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
    REQUIRE(store.backups(r.id).size() == 2); // the moved backup + the version saved before the move
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
    store.setTrashFunction([](const juce::File& f) { return f.deleteFile(); }); // not the user's Trash
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

TEST_CASE("rig_store: bands come from rig JSON and emptied bands vanish")
{
    TempDir t;
    amp::RigStore store(t.dir);
    auto r = rig("x", "AC/DC");
    REQUIRE(store.save(r).wasOk());
    auto b = store.bands();
    REQUIRE(b.contains("AC/DC"));
    REQUIRE_FALSE(b.contains("AC_DC"));

    t.dir.getChildFile("Rigs/Empty Band").createDirectory(); // user-created empty band
    REQUIRE(store.bands().contains("Empty Band"));

    r.band = "Other";
    REQUIRE(store.save(r).wasOk());
    REQUIRE_FALSE(t.dir.getChildFile("Rigs/AC_DC").exists());
    REQUIRE_FALSE(store.bands().contains("AC/DC"));
    REQUIRE(store.bands().contains("Other"));
}

TEST_CASE("rig_store: corrupt then renamed keeps the corrupt bytes")
{
    TempDir t;
    amp::RigStore store(t.dir);
    auto r = rig("Tone");
    REQUIRE(store.save(r).wasOk());
    auto file = t.dir.getChildFile("Rigs/My Tones/Tone.amprig");
    file.replaceWithText("{ corrupt");
    r.name = "Renamed";
    REQUIRE(store.save(r).wasOk());
    REQUIRE_FALSE(file.exists());
    auto parked = t.dir.getChildFile("Rigs/My Tones/Tone.amprig.corrupt");
    REQUIRE(parked.existsAsFile());
    REQUIRE(parked.loadFileAsString() == "{ corrupt");
    REQUIRE(store.load(r.id).rig->name == "Renamed");
}

TEST_CASE("rig_store: remove refuses when cached path now holds a different rig")
{
    TempDir t;
    amp::RigStore store(t.dir);
    auto a = rig("Tone"), b = rig("Other");
    REQUIRE(store.save(a).wasOk());
    auto file = t.dir.getChildFile("Rigs/My Tones/Tone.amprig");
    file.replaceWithText("{ corrupt"); // list() can no longer see `a`; cache remembers the path
    file.replaceWithText(amp::serializeRig(b)); // an external tool drops another rig there
    REQUIRE(store.remove(a.id).failed());
    REQUIRE(file.existsAsFile());
    REQUIRE(amp::parseRig(file.loadFileAsString()).rig->id == b.id);
    // saving `a` must not overwrite or delete the foreign file either
    REQUIRE(store.save(a).wasOk());
    REQUIRE(amp::parseRig(file.loadFileAsString()).rig->id == b.id);
}

#if !JUCE_WINDOWS
TEST_CASE("rig_store: failed write leaves backups and rig untouched")
{
    TempDir t;
    amp::RigStore store(t.dir);
    auto r = rig("Tone");
    for (int i = 0; i < 3; ++i)
    {
        r.tags = { "s" + juce::String(i) };
        REQUIRE(store.save(r).wasOk());
    }
    auto dir = t.dir.getChildFile("Rigs/My Tones");
    auto before = store.backups(r.id);
    REQUIRE(before.size() == 2);
    auto bak1 = before[0].loadFileAsString();
    auto current = dir.getChildFile("Tone.amprig").loadFileAsString();

    REQUIRE(dir.setReadOnly(true, false));
    auto probe = dir.getChildFile("probe");
    if (probe.create().wasOk()) // e.g. running as root: cannot simulate
    {
        probe.deleteFile();
        dir.setReadOnly(false, false);
        SKIP("directory stays writable");
    }
    for (int i = 0; i < 6; ++i)
    {
        r.tags = { "fail" };
        REQUIRE(store.save(r).failed());
    }
    dir.setReadOnly(false, false);

    REQUIRE(store.backups(r.id).size() == 2);
    REQUIRE(store.backups(r.id)[0].loadFileAsString() == bak1);
    REQUIRE(dir.getChildFile("Tone.amprig").loadFileAsString() == current);
}
#endif

TEST_CASE("rig_store: fresh store finds a corrupt rig and its backups through the backups")
{
    TempDir t;
    auto r = rig("Fragile");
    {
        amp::RigStore store(t.dir);
        REQUIRE(store.save(r).wasOk());
        REQUIRE(store.save(r).wasOk()); // .bak.1 now holds a parseable copy
    }
    const auto file = t.dir.getChildFile("Rigs/My Tones/Fragile.amprig");
    REQUIRE(file.replaceWithText("{ broken"));

    amp::RigStore fresh(t.dir); // knows no paths from earlier scans
    CHECK(fresh.list().empty());
    auto loaded = fresh.load(r.id);
    CHECK_FALSE(loaded.rig.has_value());
    CHECK_FALSE(loaded.error.startsWith("Rig not found")); // a parse error, not "not found"
    auto backups = fresh.backups(r.id);
    REQUIRE(backups.size() == 1);
    CHECK(backups[0] == file.getSiblingFile("Fragile.amprig.bak.1"));
    CHECK(file.loadFileAsString() == "{ broken");
}

TEST_CASE("rig_store: saving under a new name keeps the previous version as a backup")
{
    TempDir t;
    amp::RigStore store(t.dir);
    auto r = rig("A", "Band");
    r.tags.add("tone1");
    REQUIRE(store.save(r).wasOk());
    const auto oldFile = t.dir.getChildFile("Rigs/Band/A.amprig");
    const auto oldText = oldFile.loadFileAsString();

    r.name = "B";
    r.band = "Other";
    r.tags = { "tone2" };
    REQUIRE(store.save(r).wasOk());
    const auto newFile = t.dir.getChildFile("Rigs/Other/B.amprig");
    REQUIRE(newFile.existsAsFile());
    REQUIRE_FALSE(oldFile.existsAsFile());
    REQUIRE(amp::parseRig(newFile.loadFileAsString()).rig->tags == juce::StringArray { "tone2" });
    const auto bak1 = newFile.getSiblingFile("B.amprig.bak.1");
    REQUIRE(bak1.existsAsFile());
    REQUIRE(bak1.loadFileAsString() == oldText);
    auto backups = store.backups(r.id);
    REQUIRE(backups.size() == 1);
    REQUIRE(backups.front() == bak1);
}

TEST_CASE("rig_store: a rename keeps at most five backups, the previous version newest")
{
    TempDir t;
    amp::RigStore store(t.dir);
    auto r = rig("A");
    for (int i = 0; i < 7; ++i) // 6 backups' worth: the store keeps 5
    {
        r.tags = { "v" + juce::String(i) };
        REQUIRE(store.save(r).wasOk());
    }
    r.name = "Renamed";
    r.tags = { "v7" };
    REQUIRE(store.save(r).wasOk());
    auto backups = store.backups(r.id);
    REQUIRE(backups.size() == 5);
    REQUIRE(amp::parseRig(backups.front().loadFileAsString()).rig->tags == juce::StringArray { "v6" });
    REQUIRE(amp::parseRig(backups.back().loadFileAsString()).rig->tags == juce::StringArray { "v2" });
    REQUIRE_FALSE(t.dir.getChildFile("Rigs/My Tones/Renamed.amprig.bak.6").exists());
}

TEST_CASE("rig_store: a Finder copy with the same id gets a fresh id and both rigs list")
{
    TempDir t;
    amp::RigStore store(t.dir);
    auto r = rig("Lead", "Band");
    REQUIRE(store.save(r).wasOk());
    const auto original = t.dir.getChildFile("Rigs/Band/Lead.amprig");
    const auto copy = t.dir.getChildFile("Rigs/Band/Lead copy.amprig");
    REQUIRE(original.copyFileTo(copy));

    amp::RigStore fresh(t.dir); // a new launch: nothing cached
    auto entries = fresh.list();
    REQUIRE(entries.size() == 2);
    REQUIRE(entries[0].id != entries[1].id);
    auto parsedOriginal = amp::parseRig(original.loadFileAsString()).rig;
    auto parsedCopy = amp::parseRig(copy.loadFileAsString()).rig;
    REQUIRE(parsedOriginal->id == r.id); // the older file keeps its id
    REQUIRE(parsedCopy->id != r.id);     // the copy was rewritten with a fresh one
    REQUIRE(parsedCopy->name == "Lead");
    REQUIRE(fresh.load(parsedCopy->id).rig.has_value());
    REQUIRE(fresh.load(r.id).rig->id == r.id);
    REQUIRE(fresh.list().size() == 2); // stable on the next scan
}

TEST_CASE("rig_store: dotfiles neither keep a band folder alive nor become bands")
{
    TempDir t;
    amp::RigStore store(t.dir);
    auto r = rig("Solo", "Old Band");
    REQUIRE(store.save(r).wasOk());
    REQUIRE(t.dir.getChildFile("Rigs/Old Band/.DS_Store").replaceWithText("x"));
    REQUIRE(t.dir.getChildFile("Rigs/.hidden").createDirectory());

    r.band = "New Band";
    REQUIRE(store.save(r).wasOk());
    REQUIRE_FALSE(t.dir.getChildFile("Rigs/Old Band").exists());
    REQUIRE_FALSE(store.bands().contains(".hidden"));
    REQUIRE_FALSE(store.bands().contains("Old Band"));
}

TEST_CASE("rig_store: remove hands the rig file and its backups to the trash function")
{
    TempDir t;
    amp::RigStore store(t.dir);
    const auto bin = t.dir.getChildFile("bin");
    REQUIRE(bin.createDirectory());
    store.setTrashFunction([&](const juce::File& f) { return f.moveFileTo(bin.getChildFile(f.getFileName())); });
    auto r = rig("Gone");
    REQUIRE(store.save(r).wasOk());
    REQUIRE(store.save(r).wasOk()); // one backup
    REQUIRE(store.remove(r.id).wasOk());
    REQUIRE_FALSE(t.dir.getChildFile("Rigs/My Tones/Gone.amprig").exists());
    REQUIRE(bin.getChildFile("Gone.amprig").existsAsFile());
    REQUIRE(bin.getChildFile("Gone.amprig.bak.1").existsAsFile());
}
