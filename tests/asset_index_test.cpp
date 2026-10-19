#include <catch2/catch_test_macros.hpp>

#include "assets/AssetIndex.h"

namespace
{
struct TempRoot
{
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("amp_asset_index_" + juce::Uuid().toString());
    TempRoot() { dir.createDirectory(); }
    ~TempRoot() { dir.deleteRecursively(); }
};

juce::File fixture(const char* name)
{
    return juce::File(AMP_FIXTURES_DIR).getChildFile("assets").getChildFile(name);
}

amp::AssetIndex indexOf(const juce::File& root)
{
    amp::AssetIndex idx;
    idx.setRoots({ root });
    idx.rescan();
    return idx;
}

amp::AssetRef refTo(const juce::String& kind, const juce::String& file, const juce::String& sha)
{
    amp::AssetRef r;
    r.kind = kind;
    r.file = file;
    r.sha256 = sha;
    return r;
}
} // namespace

TEST_CASE("asset_index: resolves by hash after rename")
{
    TempRoot t;
    const auto moved = t.dir.getChildFile("sub/renamed_amp.nam");
    moved.getParentDirectory().createDirectory();
    REQUIRE(fixture("tiny.nam").copyFileTo(moved));

    auto idx = indexOf(t.dir);
    const auto r = idx.resolve(refTo("nam", "Models/tiny.nam", amp::AssetIndex::sha256Of(fixture("tiny.nam"))));
    REQUIRE(r.how == amp::ResolveKind::ByHash);
    REQUIRE(r.file == moved);
}

TEST_CASE("asset_index: falls back to filename when hash differs")
{
    TempRoot t;
    const auto f = t.dir.getChildFile("Tiny.NAM");
    REQUIRE(fixture("tiny.nam").copyFileTo(f));

    auto idx = indexOf(t.dir);
    const auto r = idx.resolve(refTo("nam", "Models/old/tiny.nam", "deadbeef"));
    REQUIRE(r.how == amp::ResolveKind::ByFilename);
    REQUIRE(r.file == f);

    // Filename fallback is restricted to the same kind.
    REQUIRE(idx.resolve(refTo("ir", "tiny.nam", "deadbeef")).how == amp::ResolveKind::Missing);
}

TEST_CASE("asset_index: missing when neither")
{
    TempRoot t;
    REQUIRE(fixture("tiny.nam").copyFileTo(t.dir.getChildFile("a.nam")));

    auto idx = indexOf(t.dir);
    const auto r = idx.resolve(refTo("nam", "nothere.nam", "deadbeef"));
    REQUIRE(r.how == amp::ResolveKind::Missing);
    REQUIRE(r.file == juce::File());
}

TEST_CASE("asset_index: list filters by kind and skips hidden and other files")
{
    TempRoot t;
    REQUIRE(fixture("tiny.nam").copyFileTo(t.dir.getChildFile("a.nam")));
    REQUIRE(fixture("ir_48k.wav").copyFileTo(t.dir.getChildFile("b.wav")));
    REQUIRE(fixture("ir_48k.wav").copyFileTo(t.dir.getChildFile("c.WAV")));
    REQUIRE(t.dir.getChildFile("notes.txt").replaceWithText("x"));
    REQUIRE(fixture("tiny.nam").copyFileTo(t.dir.getChildFile(".hidden.nam")));
    REQUIRE(t.dir.getChildFile(".secret").createDirectory());
    REQUIRE(fixture("tiny.nam").copyFileTo(t.dir.getChildFile(".secret/in_hidden.nam")));

    auto idx = indexOf(t.dir);
    REQUIRE(idx.list("nam").size() == 1);
    REQUIRE(idx.list("nam")[0].file.getFileName() == "a.nam");
    REQUIRE(idx.list("ir").size() == 2);
    REQUIRE(idx.list("other").empty());
}

TEST_CASE("asset_index: rescan picks up new file")
{
    TempRoot t;
    amp::AssetIndex idx;
    idx.setRoots({ t.dir });
    idx.rescan();
    REQUIRE(idx.list("nam").empty());

    REQUIRE(fixture("tiny.nam").copyFileTo(t.dir.getChildFile("new.nam")));
    idx.rescan();
    REQUIRE(idx.list("nam").size() == 1);
}

TEST_CASE("asset_index: unchanged files are not rehashed")
{
    TempRoot t;
    REQUIRE(fixture("tiny.nam").copyFileTo(t.dir.getChildFile("a.nam")));
    REQUIRE(fixture("ir_48k.wav").copyFileTo(t.dir.getChildFile("b.wav")));

    amp::AssetIndex idx;
    idx.setRoots({ t.dir });
    idx.rescan();
    REQUIRE(idx.hashCount() == 2);

    idx.rescan();
    REQUIRE(idx.hashCount() == 2);
    REQUIRE(idx.list("nam").size() == 1);
}

TEST_CASE("asset_index: sha256Of is lowercase hex")
{
    const auto h = amp::AssetIndex::sha256Of(fixture("ir_48k.wav"));
    REQUIRE(h.length() == 64);
    REQUIRE(h == h.toLowerCase());
}
