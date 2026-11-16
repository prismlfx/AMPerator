#include <catch2/catch_test_macros.hpp>

#include "assets/AssetIndex.h"
#include "online/Placement.h"

using namespace amp::online;

namespace
{
struct TempDir
{
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("amp_pl_" + juce::String(juce::Random::getSystemRandom().nextInt64()));
    TempDir() { dir.createDirectory(); }
    ~TempDir() { dir.deleteRecursively(); }
};

juce::File fixtureAsset(const char* name)
{
    return juce::File(AMP_FIXTURES_DIR).getChildFile("assets").getChildFile(name);
}

Provenance prov(juce::String creator, juce::String title, juce::String model, juce::String format = "nam")
{
    Provenance p;
    p.creator.username = creator;
    p.title = title;
    p.modelName = model;
    p.format = format;
    return p;
}
} // namespace

TEST_CASE("online_placement: placement sanitizes creator and title", "[online]")
{
    TempDir t;
    const auto longTitle = juce::String::repeatedString("A", 190) + juce::String(juce::CharPointer_UTF8("\xf0\x9f\x8e\xb8")) +
                           juce::String::repeatedString("B", 9);
    REQUIRE(longTitle.length() == 200);
    const auto f = placementFor(t.dir, prov("AC/DC: live?", longTitle, "Standard"));

    CHECK(f.getParentDirectory().getParentDirectory() == t.dir.getChildFile("Models/TONE3000"));
    CHECK(f.getParentDirectory().getFileName() == "AC_DC_ live_");
    CHECK(f.getFileExtension() == ".nam");
    // 80 (title) + " — " + 8 (model) + ".nam"
    CHECK(f.getFileName().length() <= 95);
    CHECK(f.getFileName().contains(juce::String(juce::CharPointer_UTF8(" \xe2\x80\x94 Standard"))));
    CHECK(! f.getFileName().containsAnyOf("/\\:*?\"<>|"));
}

TEST_CASE("online_placement: byte-heavy titles stay inside file name limits", "[online]")
{
    TempDir t;
    const auto emoji = juce::String(juce::CharPointer_UTF8("\xf0\x9f\x8e\xb8"));
    const auto f = placementFor(t.dir, prov("x", juce::String::repeatedString(emoji, 120),
                                            juce::String::repeatedString(emoji, 120)));
    CHECK(f.getFileName().getNumBytesAsUTF8() <= 255);
    CHECK(f.getFileName().endsWith(".nam"));
}

TEST_CASE("online_placement: folders, extensions and empty creator", "[online]")
{
    TempDir t;
    const auto ir = placementFor(t.dir, prov("", "Cab", "Close", "ir"));
    CHECK(ir == t.dir.getChildFile("IRs/TONE3000/unknown")
                    .getChildFile(juce::String(juce::CharPointer_UTF8("Cab \xe2\x80\x94 Close.wav"))));
    const auto nam = placementFor(t.dir, prov("jane", "Amp", "Std"));
    CHECK(nam.getFullPathName().contains("Models"));
    CHECK(nam.getFileExtension() == ".nam");

    // Trailing dots/spaces after truncation are trimmed.
    const auto cut = placementFor(t.dir, prov("j", juce::String::repeatedString("a", 79) + "." + "tail", "m"));
    CHECK(cut.getFileName().startsWith(juce::String::repeatedString("a", 79) + " "));
}

TEST_CASE("online_placement: placeFile reuses an identical file and numbers a different one", "[online]")
{
    TempDir t;
    const auto target = t.dir.getChildFile("Models/TONE3000/jane/Amp.nam");

    auto make = [&](const char* name, const juce::String& text)
    {
        auto f = t.dir.getChildFile(name);
        f.replaceWithText(text);
        return f;
    };

    juce::String err;
    auto a = make("p1", "alpha");
    const auto shaA = amp::AssetIndex::sha256Of(a);
    const auto placed = placeFile(a, target, shaA, err);
    CHECK(err.isEmpty());
    CHECK(placed == target);
    CHECK(target.loadFileAsString() == "alpha");
    CHECK(! a.exists());

    // Same content again: reused, partial removed, nothing numbered.
    auto a2 = make("p2", "alpha");
    CHECK(placeFile(a2, target, shaA, err) == target);
    CHECK(! a2.exists());
    CHECK(! target.getSiblingFile("Amp (2).nam").exists());

    // Different content: " (2)", then " (3)"; the original is untouched.
    auto b = make("p3", "beta");
    const auto second = placeFile(b, target, amp::AssetIndex::sha256Of(b), err);
    CHECK(second == target.getSiblingFile("Amp (2).nam"));
    CHECK(second.loadFileAsString() == "beta");
    CHECK(target.loadFileAsString() == "alpha");
    CHECK(! b.exists());

    auto c = make("p4", "gamma");
    CHECK(placeFile(c, target, amp::AssetIndex::sha256Of(c), err) == target.getSiblingFile("Amp (3).nam"));

    // The same content as the numbered one is found past the original.
    auto b2 = make("p5", "beta");
    CHECK(placeFile(b2, target, amp::AssetIndex::sha256Of(b2), err) == second);
    CHECK(! b2.exists());
    CHECK(err.isEmpty());
}

TEST_CASE("online_placement: placeFile reports failure and keeps the partial", "[online]")
{
    TempDir t;
    auto partial = t.dir.getChildFile("p");
    partial.replaceWithText("x");
    // A regular file where the directory must go.
    t.dir.getChildFile("blocked").replaceWithText("file");
    juce::String err;
    const auto r = placeFile(partial, t.dir.getChildFile("blocked/sub/Amp.nam"), amp::AssetIndex::sha256Of(partial), err);
    CHECK(r == juce::File());
    CHECK(err.isNotEmpty());
    CHECK(partial.existsAsFile());

    // A missing partial is an error too.
    err = {};
    CHECK(placeFile(t.dir.getChildFile("nope"), t.dir.getChildFile("out/A.nam"), "00", err) == juce::File());
    CHECK(err.isNotEmpty());
}

TEST_CASE("online_placement: validateAssetFile accepts real assets and rejects mismatches", "[online]")
{
    TempDir t;
    CHECK(validateAssetFile(fixtureAsset("tiny.nam"), "nam").isEmpty());
    CHECK(validateAssetFile(fixtureAsset("ir_48k.wav"), "ir").isEmpty());

    auto html = t.dir.getChildFile("page.nam");
    html.replaceWithText("<html><body>Not found</body></html>");
    CHECK(validateAssetFile(html, "nam") == "not a valid nam file");

    CHECK(validateAssetFile(fixtureAsset("tiny.nam"), "ir") == "not a valid ir file");
    CHECK(validateAssetFile(fixtureAsset("ir_48k.wav"), "nam") == "not a valid nam file");
    CHECK(validateAssetFile(t.dir.getChildFile("missing.nam"), "nam") == "not a valid nam file");

    auto noVersion = t.dir.getChildFile("nv.nam");
    noVersion.replaceWithText("{\"architecture\":\"WaveNet\"}");
    CHECK(validateAssetFile(noVersion, "nam") == "not a valid nam file");
    auto numVersion = t.dir.getChildFile("nv2.nam");
    numVersion.replaceWithText("{\"version\":5}");
    CHECK(validateAssetFile(numVersion, "nam") == "not a valid nam file");

    auto empty = t.dir.getChildFile("empty.wav");
    empty.replaceWithText("");
    CHECK(validateAssetFile(empty, "ir") == "not a valid ir file");
}
