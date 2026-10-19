#include <catch2/catch_test_macros.hpp>

#include "model/RigDocument.h"

#include <set>

namespace
{
juce::String fixtureText()
{
    return juce::File(AMP_FIXTURES_DIR).getChildFile("rigs/interstate.amprig").loadFileAsString();
}

amp::RigDocument loadFixture()
{
    auto r = amp::parseRig(fixtureText());
    REQUIRE(r.rig.has_value());
    return *r.rig;
}

void requireEqual(const amp::RigDocument& a, const amp::RigDocument& b)
{
    REQUIRE(a.id == b.id);
    REQUIRE(a.name == b.name);
    REQUIRE(a.band == b.band);
    REQUIRE(a.tags == b.tags);
    REQUIRE(a.connections == b.connections);
    REQUIRE(a.mappings.size() == b.mappings.size());
    for (size_t i = 0; i < a.mappings.size(); ++i)
    {
        REQUIRE(a.mappings[i].control == b.mappings[i].control);
        REQUIRE(a.mappings[i].action == b.mappings[i].action);
        REQUIRE(a.mappings[i].node == b.mappings[i].node);
    }
    REQUIRE(a.nodes.size() == b.nodes.size());
    for (size_t i = 0; i < a.nodes.size(); ++i)
    {
        const auto& x = a.nodes[i];
        const auto& y = b.nodes[i];
        REQUIRE(x.id == y.id);
        REQUIRE(x.type == y.type);
        REQUIRE(x.bypass == y.bypass);
        REQUIRE(x.artArchetype == y.artArchetype);
        REQUIRE(x.state == y.state);
        REQUIRE(x.params.size() == y.params.size());
        for (int p = 0; p < x.params.size(); ++p)
        {
            REQUIRE(x.params.getName(p) == y.params.getName(p));
            REQUIRE(x.params.getValueAt(p) == y.params.getValueAt(p));
        }
        REQUIRE(x.asset.has_value() == y.asset.has_value());
        if (x.asset)
        {
            REQUIRE(x.asset->kind == y.asset->kind);
            REQUIRE(x.asset->file == y.asset->file);
            REQUIRE(x.asset->sha256 == y.asset->sha256);
            REQUIRE(x.asset->source.has_value() == y.asset->source.has_value());
            if (x.asset->source)
            {
                REQUIRE(x.asset->source->provider == y.asset->source->provider);
                REQUIRE(x.asset->source->id == y.asset->source->id);
            }
        }
        REQUIRE(x.plugin.has_value() == y.plugin.has_value());
        if (x.plugin)
        {
            REQUIRE(x.plugin->format == y.plugin->format);
            REQUIRE(x.plugin->uid == y.plugin->uid);
            REQUIRE(x.plugin->name == y.plugin->name);
            REQUIRE(x.plugin->vendor == y.plugin->vendor);
        }
    }
}
} // namespace

TEST_CASE("round trip preserves all fields")
{
    auto a = loadFixture();

    REQUIRE(a.name == "Interstate Love Song");
    REQUIRE(a.band == "STP");
    REQUIRE(a.tags.size() == 2);
    REQUIRE(a.nodes.size() == 7);
    REQUIRE(a.nodes[3].type == amp::NodeType::NamAmp);
    REQUIRE(a.nodes[3].asset->source->provider == "tone3000");
    REQUIRE(a.nodes[3].artArchetype == "high-gain-rectifier");
    REQUIRE(static_cast<bool>(a.nodes[3].params["normalize"]) == true);
    REQUIRE(a.nodes[5].state.getSize() == 15);
    REQUIRE(a.nodes[2].state.getSize() == 8);
    REQUIRE(a.connections.size() == 6);
    REQUIRE(a.mappings.size() == 1);

    auto text = amp::serializeRig(a);
    auto r = amp::parseRig(text);
    REQUIRE(r.rig.has_value());
    requireEqual(a, *r.rig);
}

TEST_CASE("rejects wrong format")
{
    auto text = fixtureText().replace("amperator.rig", "other");
    auto r = amp::parseRig(text);
    REQUIRE_FALSE(r.rig.has_value());
    REQUIRE(r.error.contains("format"));
}

TEST_CASE("rejects newer version")
{
    auto text = fixtureText().replace("\"version\": 1", "\"version\": 2");
    auto r = amp::parseRig(text);
    REQUIRE_FALSE(r.rig.has_value());
    REQUIRE(r.error.contains("version"));
}

TEST_CASE("malformed JSON returns error, no throw")
{
    for (const char* bad : { "{ not json", "", "[1,2,3]", "42" })
    {
        amp::ParseResult r;
        REQUIRE_NOTHROW(r = amp::parseRig(bad));
        REQUIRE_FALSE(r.rig.has_value());
        REQUIRE(r.error.isNotEmpty());
    }
}

TEST_CASE("unknown keys survive round trip")
{
    auto text = fixtureText().replace("\"id\": \"n1\", \"type\": \"Gate\"",
                                      "\"id\": \"n1\", \"futureKey\": 1, \"type\": \"Gate\"");
    auto r = amp::parseRig(text);
    REQUIRE(r.rig.has_value());
    auto out = amp::serializeRig(*r.rig);
    auto r2 = amp::parseRig(out);
    REQUIRE(r2.rig.has_value());
    REQUIRE(r2.rig->nodes[1].extra.getProperty("futureKey", {}) == juce::var(1));
    REQUIRE(out.contains("futureKey"));
}

TEST_CASE("unknown node type becomes placeholder and round trips unchanged")
{
    auto text = fixtureText().replace("\"type\": \"Gate\"", "\"type\": \"Reverb9000\"");
    auto r = amp::parseRig(text);
    REQUIRE(r.rig.has_value());
    REQUIRE(r.rig->nodes[1].type == amp::NodeType::Placeholder);

    auto out = amp::serializeRig(*r.rig);
    REQUIRE(out.contains("Reverb9000"));
    REQUIRE_FALSE(out.contains("Placeholder"));
    auto r2 = amp::parseRig(out);
    REQUIRE(r2.rig.has_value());
    REQUIRE(r2.rig->nodes[1].type == amp::NodeType::Placeholder);
    REQUIRE(r2.rig->nodes[1].params.size() == 1);
}

TEST_CASE("default rig shape")
{
    auto rig = amp::makeDefaultRig("Test", "My Tones");
    REQUIRE(rig.name == "Test");
    REQUIRE(rig.band == "My Tones");
    REQUIRE(rig.id.isNotEmpty());
    REQUIRE(rig.nodes.size() == 5);
    const amp::NodeType expected[] = { amp::NodeType::Input, amp::NodeType::Gate, amp::NodeType::NamAmp,
                                       amp::NodeType::CabIR, amp::NodeType::Output };
    std::set<juce::String> ids;
    for (size_t i = 0; i < 5; ++i)
    {
        REQUIRE(rig.nodes[i].type == expected[i]);
        ids.insert(rig.nodes[i].id);
    }
    REQUIRE(ids.size() == 5);
    REQUIRE(rig.connections.size() == 4);
    for (size_t i = 0; i < 4; ++i)
        REQUIRE(rig.connections[i] == std::make_pair(rig.nodes[i].id, rig.nodes[i + 1].id));

    auto r = amp::parseRig(amp::serializeRig(rig));
    REQUIRE(r.rig.has_value());
    requireEqual(rig, *r.rig);
}
