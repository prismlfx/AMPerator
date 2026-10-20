#include <catch2/catch_test_macros.hpp>

#include "control/MappingEngine.h"

namespace
{
using amp::ControlEvent;
using amp::Mapping;

struct Fixture
{
    std::map<juce::String, bool> state { { "amp", false }, { "gate", false } };
    std::vector<std::pair<juce::String, bool>> changes;
    amp::MappingEngine engine { [this](const juce::String& id, std::optional<bool> set) -> std::optional<bool> {
        auto it = state.find(id);
        if (it == state.end())
            return std::nullopt;
        it->second = set.value_or(!it->second);
        return it->second;
    } };

    Fixture()
    {
        engine.setValidNodes({ "amp", "gate" });
        engine.onBypassChanged = [this](const juce::String& n, bool b) { changes.emplace_back(n, b); };
    }
};

Mapping tog(const char* control, const char* node) { return { control, "toggleBypass", node }; }
} // namespace

TEST_CASE("mapping_engine: press toggles mapped node")
{
    Fixture f;
    f.engine.setMappings({ tog("footswitch:L", "amp") }, {});
    f.engine.handle({ "footswitch:L", true });
    REQUIRE(f.changes.size() == 1);
    CHECK(f.changes[0].first == "amp");
    CHECK(f.changes[0].second == true);
    f.engine.handle({ "footswitch:L", true });
    CHECK(f.changes.back().second == false);
}

TEST_CASE("mapping_engine: release does nothing")
{
    Fixture f;
    f.engine.setMappings({ tog("footswitch:L", "amp") }, {});
    f.engine.handle({ "footswitch:L", false });
    CHECK(f.changes.empty());
}

TEST_CASE("mapping_engine: rig mapping overrides global for same control")
{
    Fixture f;
    f.engine.setMappings({ tog("footswitch:L", "gate") }, { tog("footswitch:L", "amp") });
    f.engine.handle({ "footswitch:L", true });
    REQUIRE(f.changes.size() == 1);
    CHECK(f.changes[0].first == "gate");
    auto eff = f.engine.effectiveMappings();
    REQUIRE(eff.size() == 1);
    CHECK(eff[0].node == "gate");
}

TEST_CASE("mapping_engine: global mapping applies when rig has none")
{
    Fixture f;
    f.engine.setMappings({ tog("footswitch:M", "gate") }, { tog("footswitch:L", "amp") });
    f.engine.handle({ "footswitch:L", true });
    REQUIRE(f.changes.size() == 1);
    CHECK(f.changes[0].first == "amp");
    auto eff = f.engine.effectiveMappings();
    REQUIRE(eff.size() == 2);
    CHECK(eff[0].control == "footswitch:M"); // rig first, then global
    CHECK(eff[1].control == "footswitch:L");
}

TEST_CASE("mapping_engine: learn captures next press and does not toggle")
{
    Fixture f;
    std::vector<Mapping> learned;
    f.engine.onLearned = [&](const Mapping& m) { learned.push_back(m); };
    f.engine.beginLearn("amp");
    REQUIRE(f.engine.isLearning());

    f.engine.handle({ "midi:dev:cc7", false }); // release ignored while armed
    CHECK(f.engine.isLearning());
    CHECK(learned.empty());

    f.engine.handle({ "midi:dev:cc7", true });
    REQUIRE(learned.size() == 1);
    CHECK(learned[0].control == "midi:dev:cc7");
    CHECK(learned[0].node == "amp");
    CHECK(learned[0].action == "toggleBypass");
    CHECK(f.changes.empty());
    CHECK(f.state["amp"] == false);
    CHECK_FALSE(f.engine.isLearning());

    f.engine.handle({ "midi:dev:cc7", true }); // very next press toggles
    REQUIRE(f.changes.size() == 1);
    CHECK(f.changes[0].first == "amp");
}

TEST_CASE("mapping_engine: learning replaces an existing mapping for that control")
{
    Fixture f;
    f.engine.setMappings({ tog("footswitch:L", "amp"), tog("footswitch:M", "gate") }, {});
    f.engine.beginLearn("gate");
    f.engine.handle({ "footswitch:L", true });
    auto eff = f.engine.effectiveMappings();
    REQUIRE(eff.size() == 2);
    int forL = 0;
    for (auto& m : eff)
        if (m.control == "footswitch:L")
        {
            ++forL;
            CHECK(m.node == "gate");
        }
    CHECK(forL == 1);
    f.engine.handle({ "footswitch:L", true });
    REQUIRE(f.changes.size() == 1);
    CHECK(f.changes[0].first == "gate");
}

TEST_CASE("mapping_engine: learn overrides a global mapping and cancel disarms")
{
    Fixture f;
    f.engine.setMappings({}, { tog("footswitch:L", "amp") });
    f.engine.beginLearn("gate");
    f.engine.cancelLearn();
    CHECK_FALSE(f.engine.isLearning());
    f.engine.beginLearn("gate");
    f.engine.handle({ "footswitch:L", true });
    f.engine.handle({ "footswitch:L", true });
    REQUIRE(f.changes.size() == 1);
    CHECK(f.changes[0].first == "gate");
    CHECK(f.engine.effectiveMappings().size() == 1);
}

TEST_CASE("mapping_engine: beginLearn for unknown node is ignored")
{
    Fixture f;
    f.engine.beginLearn("ghost");
    CHECK_FALSE(f.engine.isLearning());
}

TEST_CASE("mapping_engine: stale mapping is ignored and pruned")
{
    Fixture f;
    f.engine.setMappings({ tog("footswitch:L", "deleted"), tog("footswitch:M", "amp") }, { tog("footswitch:R", "deleted") });
    f.engine.handle({ "footswitch:L", true });
    f.engine.handle({ "footswitch:R", true });
    CHECK(f.changes.empty());
    auto eff = f.engine.effectiveMappings();
    REQUIRE(eff.size() == 1);
    CHECK(eff[0].node == "amp");
}

TEST_CASE("mapping_engine: unmapped press is a no-op")
{
    Fixture f;
    f.engine.setMappings({ tog("footswitch:L", "amp") }, {});
    f.engine.handle({ "footswitch:R", true });
    CHECK(f.changes.empty());
}

TEST_CASE("mapping_engine: unknown action is kept but not executed; failed bypass fires no callback")
{
    Fixture f;
    f.engine.setMappings({ Mapping { "footswitch:L", "nextRig", "amp" }, tog("footswitch:M", "gate") }, {});
    f.engine.handle({ "footswitch:L", true });
    CHECK(f.changes.empty());
    CHECK(f.engine.effectiveMappings().size() == 2);

    f.state.erase("gate"); // engine says node not found
    f.engine.handle({ "footswitch:M", true });
    CHECK(f.changes.empty());
}
