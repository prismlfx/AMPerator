#include <catch2/catch_test_macros.hpp>

#include "model/ChainGraph.h"

using namespace amp;

namespace
{
NodeDoc node(const juce::String& id, NodeType t = NodeType::Gate)
{
    NodeDoc n;
    n.id = id;
    n.type = t;
    return n;
}

std::vector<juce::String> middle(const RigDocument& d)
{
    auto o = graph::linearOrder(d);
    return { o.begin() + 1, o.end() - 1 };
}
} // namespace

TEST_CASE("linearOrder of default rig is in, Gate, NamAmp, CabIR, out")
{
    auto rig = makeDefaultRig("r", "b");
    auto order = graph::linearOrder(rig);
    REQUIRE(order.size() == 5);
    for (size_t i = 0; i < 5; ++i)
        CHECK(order[i] == rig.nodes[i].id);
    CHECK(rig.nodes[0].type == NodeType::Input);
    CHECK(rig.nodes[4].type == NodeType::Output);
    CHECK(graph::isLinearChain(rig));
}

TEST_CASE("insert at 0 goes after input")
{
    auto rig = makeDefaultRig("r", "b");
    auto id = graph::insertNode(rig, node("x"), 0);
    CHECK(id == "x");
    auto order = graph::linearOrder(rig);
    REQUIRE(order.size() == 6);
    CHECK(order[0] == rig.nodes[0].id);
    CHECK(order[1] == "x");
    CHECK(graph::insertNode(rig, node("y"), -5) == "y");
    CHECK(graph::linearOrder(rig)[1] == "y");
}

TEST_CASE("insert past end goes before output")
{
    auto rig = makeDefaultRig("r", "b");
    graph::insertNode(rig, node("x"), 99);
    auto order = graph::linearOrder(rig);
    REQUIRE(order.size() == 6);
    CHECK(order[4] == "x");
    CHECK(order[5] == rig.nodes[4].id);
}

TEST_CASE("insert assigns a fresh id when empty or colliding")
{
    auto rig = makeDefaultRig("r", "b");
    auto a = graph::insertNode(rig, node(""), 0);
    CHECK(a.isNotEmpty());
    auto b = graph::insertNode(rig, node(a), 0);
    CHECK(b.isNotEmpty());
    CHECK(b != a);
    CHECK(rig.nodes.size() == 7);
    CHECK(graph::isLinearChain(rig));
}

TEST_CASE("insert into a non-linear doc is refused")
{
    auto rig = makeDefaultRig("r", "b");
    rig.connections.pop_back();
    auto before = rig.nodes.size();
    CHECK(graph::insertNode(rig, node("x"), 0).isEmpty());
    CHECK(rig.nodes.size() == before);
}

TEST_CASE("move reorders and keeps chain linear")
{
    auto rig = makeDefaultRig("r", "b");
    auto gate = rig.nodes[1].id, amp = rig.nodes[2].id, cab = rig.nodes[3].id;
    CHECK(graph::moveNode(rig, cab, 0));
    CHECK(middle(rig) == std::vector<juce::String>{ cab, gate, amp });
    CHECK(graph::moveNode(rig, cab, 99));
    CHECK(middle(rig) == std::vector<juce::String>{ gate, amp, cab });
    CHECK(graph::isLinearChain(rig));
    CHECK(rig.nodes.size() == 5);
    CHECK(graph::moveNode(rig, gate, 1));   // order is evaluated after removing the node
    CHECK(middle(rig) == std::vector<juce::String>{ amp, gate, cab });
}

TEST_CASE("move and remove on a non-linear doc are refused and leave it unchanged")
{
    auto rig = makeDefaultRig("r", "b");
    rig.connections.pop_back();
    auto conns = rig.connections;
    auto gate = rig.nodes[1].id;
    CHECK_FALSE(graph::moveNode(rig, gate, 0));
    CHECK_FALSE(graph::removeNode(rig, gate));
    CHECK(rig.connections == conns);
    CHECK(rig.nodes.size() == 5);
}

TEST_CASE("insert refuses Input/Output nodes")
{
    auto rig = makeDefaultRig("r", "b");
    auto conns = rig.connections;
    CHECK(graph::insertNode(rig, node("i2", NodeType::Input), 0).isEmpty());
    CHECK(graph::insertNode(rig, node("o2", NodeType::Output), 0).isEmpty());
    CHECK(rig.nodes.size() == 5);
    CHECK(rig.connections == conns);
    CHECK(graph::isLinearChain(rig));
}

TEST_CASE("remove rewires neighbors and prunes mappings")
{
    auto rig = makeDefaultRig("r", "b");
    auto gate = rig.nodes[1].id, amp = rig.nodes[2].id;
    rig.mappings.push_back({ "footswitch:L", "toggleBypass", amp });
    rig.mappings.push_back({ "footswitch:R", "toggleBypass", gate });
    CHECK(graph::removeNode(rig, amp));
    CHECK(graph::isLinearChain(rig));
    CHECK(rig.nodes.size() == 4);
    CHECK(rig.connections.size() == 3);
    CHECK(graph::linearOrder(rig) == std::vector<juce::String>{ rig.nodes[0].id, gate, rig.nodes[2].id, rig.nodes[3].id });
    REQUIRE(rig.mappings.size() == 1);
    CHECK(rig.mappings[0].node == gate);
}

TEST_CASE("input/output cannot be moved or removed; unknown ids fail")
{
    auto rig = makeDefaultRig("r", "b");
    auto in = rig.nodes[0].id, out = rig.nodes[4].id;
    auto before = rig.connections;
    CHECK_FALSE(graph::moveNode(rig, in, 1));
    CHECK_FALSE(graph::moveNode(rig, out, 0));
    CHECK_FALSE(graph::removeNode(rig, in));
    CHECK_FALSE(graph::removeNode(rig, out));
    CHECK_FALSE(graph::removeNode(rig, "nope"));
    CHECK_FALSE(graph::moveNode(rig, "nope", 0));
    CHECK(rig.connections == before);
    CHECK(rig.nodes.size() == 5);
}

TEST_CASE("cycle detected")
{
    RigDocument d;
    d.nodes = { node("a"), node("b") };
    d.connections = { { "a", "b" }, { "b", "a" } };
    CHECK_FALSE(graph::topoOrder(d).has_value());
    CHECK_FALSE(graph::isLinearChain(d));
}

TEST_CASE("topoOrder orders a chain")
{
    auto rig = makeDefaultRig("r", "b");
    auto t = graph::topoOrder(rig);
    REQUIRE(t.has_value());
    CHECK(*t == graph::linearOrder(rig));
}
