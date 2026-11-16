#include <catch2/catch_test_macros.hpp>

#include "online/ProviderIndex.h"

using namespace amp::online;

namespace
{
struct TempDir
{
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("amp_pi_" + juce::String(juce::Random::getSystemRandom().nextInt64()));
    TempDir() { dir.createDirectory(); }
    ~TempDir() { dir.deleteRecursively(); }
};

Provenance sample(const juce::String& modelId = "m_123")
{
    Provenance p;
    p.modelId = modelId;
    p.toneId = "t_456";
    p.title = "Mesa Dual Rec Ch2";
    p.modelName = "Standard";
    p.size = "standard";
    p.architecture = "2";
    p.format = "nam";
    p.gear = "amp";
    p.license = "cc-by";
    p.toneUrl = "https://www.tone3000.com/tones/t_456";
    p.image = "tone3000/t_456.jpg";
    p.downloadedAt = "2026-10-20T18:02:11Z";
    p.creator = { "u_1", "jane", "Jane", "https://a/jane.png", "https://www.tone3000.com/jane" };
    return p;
}
} // namespace

TEST_CASE("online_provider_index: put, bySha and shaForModel", "[online]")
{
    TempDir t;
    ProviderIndex idx(t.dir.getChildFile("tone3000/index.json"));
    CHECK(idx.size() == 0);
    idx.put("ABCDEF", sample());
    CHECK(idx.size() == 1);
    REQUIRE(idx.bySha("abcdef").has_value());
    CHECK(idx.bySha("AbCdEf")->modelId == "m_123");
    CHECK(! idx.bySha("0000").has_value());
    CHECK(idx.shaForModel("m_123") == juce::String("abcdef"));
    CHECK(! idx.shaForModel("m_999").has_value());
    CHECK(! idx.shaForModel("").has_value());
}

TEST_CASE("online_provider_index: load creates nothing, save creates the directory", "[online]")
{
    TempDir t;
    const auto f = t.dir.getChildFile("tone3000/index.json");
    ProviderIndex idx(f);
    idx.load();
    CHECK(! f.getParentDirectory().exists());
    idx.put("aa", sample());
    REQUIRE(idx.save());
    CHECK(f.existsAsFile());
}

TEST_CASE("online_provider_index: save then load round-trips every field", "[online]")
{
    TempDir t;
    const auto f = t.dir.getChildFile("tone3000/index.json");
    {
        ProviderIndex idx(f);
        idx.put("9F2C", sample());
        idx.put("77", sample("m_2"));
        REQUIRE(idx.save());
    }
    const auto root = juce::JSON::parse(f.loadFileAsString());
    const auto rec = root.getProperty("9f2c", {});
    CHECK(rec.getProperty("modelName", {}).toString() == "Standard");
    CHECK(rec.getProperty("creator", {}).getProperty("username", {}).toString() == "jane");

    ProviderIndex idx(f);
    idx.load();
    CHECK(idx.size() == 2);
    const auto p = idx.bySha("9f2c");
    REQUIRE(p.has_value());
    const auto s = sample();
    CHECK(p->provider == "tone3000");
    CHECK(p->modelId == s.modelId);
    CHECK(p->toneId == s.toneId);
    CHECK(p->title == s.title);
    CHECK(p->modelName == s.modelName);
    CHECK(p->size == s.size);
    CHECK(p->architecture == s.architecture);
    CHECK(p->format == s.format);
    CHECK(p->gear == s.gear);
    CHECK(p->license == s.license);
    CHECK(p->toneUrl == s.toneUrl);
    CHECK(p->image == s.image);
    CHECK(p->downloadedAt == s.downloadedAt);
    CHECK(p->creator.id == s.creator.id);
    CHECK(p->creator.username == s.creator.username);
    CHECK(p->creator.displayName == s.creator.displayName);
    CHECK(p->creator.avatarUrl == s.creator.avatarUrl);
    CHECK(p->creator.url == s.creator.url);
    CHECK(idx.shaForModel("m_2") == juce::String("77"));
}

TEST_CASE("online_provider_index: a corrupt file is set aside and the index starts empty", "[online]")
{
    TempDir t;
    const auto f = t.dir.getChildFile("index.json");
    f.replaceWithText("{ not json");
    ProviderIndex idx(f);
    idx.load();
    CHECK(idx.size() == 0);
    CHECK(! f.exists());
    CHECK(t.dir.getChildFile("index.json.corrupt").loadFileAsString() == "{ not json");

    f.replaceWithText("[1,2]");
    idx.load();
    CHECK(idx.size() == 0);
    CHECK(t.dir.getChildFile("index.json.corrupt.2").existsAsFile());
}

TEST_CASE("online_provider_index: toVar builds the bridge source object", "[online]")
{
    const auto v = toVar(sample());
    CHECK(v.getProperty("provider", {}).toString() == "tone3000");
    CHECK(v.getProperty("id", {}).toString() == "m_123");
    CHECK(v.getProperty("toneId", {}).toString() == "t_456");
    CHECK(v.getProperty("title", {}).toString() == "Mesa Dual Rec Ch2");
    CHECK(v.getProperty("creator", {}).getProperty("displayName", {}).toString() == "Jane");
    CHECK(v.getProperty("license", {}).toString() == "cc-by");
    CHECK(v.getProperty("url", {}).toString() == "https://www.tone3000.com/tones/t_456");
    CHECK(v.getProperty("image", {}).toString() == "tone3000/t_456.jpg");
}

TEST_CASE("online_provider_index: shaForModel prefers the newest download", "[online]")
{
    TempDir t;
    ProviderIndex idx(t.dir.getChildFile("index.json"));
    auto older = sample();
    older.downloadedAt = "2026-01-01T00:00:00Z";
    auto newer = sample();
    newer.downloadedAt = "2026-06-01T00:00:00Z";
    idx.put("zz", older);
    idx.put("aa", newer); // sorts before "zz": the winner is not simply the first or last key
    CHECK(idx.shaForModel("m_123") == juce::String("aa"));
}

TEST_CASE("online_provider_index: save never overwrites a corrupt file that could not be set aside", "[online]")
{
    TempDir t;
    const auto f = t.dir.getChildFile("index.json");
    f.replaceWithText("{ broken");
    // Occupy the .corrupt name with a directory: neither move nor copy can use it, but the next suffix is free,
    // so the bytes still get preserved.
    t.dir.getChildFile("index.json.corrupt").createDirectory();
    ProviderIndex idx(f);
    idx.load();
    CHECK(idx.size() == 0);
    CHECK(t.dir.getChildFile("index.json.corrupt.2").loadFileAsString() == "{ broken");
}
