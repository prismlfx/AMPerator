#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <optional>

#include <juce_events/juce_events.h>

#include "dsp/CabIRNode.h"
#include "dsp/NamAmpNode.h"
#include "model/ChainGraph.h"
#include "storage/RigLoader.h"

#include "support/FakePlugins.h"

namespace
{
const amp::ProcessSpec spec { 48000.0, 128 };

juce::File fixture(const char* name)
{
    return juce::File(AMP_FIXTURES_DIR).getChildFile("assets").getChildFile(name);
}

struct Fixture
{
    const juce::ScopedJuceInitialiser_GUI juce; // this thread becomes the message thread
    juce::File dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                         .getChildFile("amp_rig_loader_" + juce::Uuid().toString());
    juce::File assetRoot = dir.getChildFile("assets");
    amp::AssetIndex index;
    std::unique_ptr<amp::PluginHost> host;
    std::unique_ptr<amp::RigLoader> loader;

    Fixture()
    {
        assetRoot.createDirectory();
        dir.getChildFile("plugins").createDirectory();
        host = amp_test::makeFakePluginHost(dir.getChildFile("plugins"));
        host->scan([](float) {});
        loader = std::make_unique<amp::RigLoader>(index, *host);
    }
    ~Fixture() { dir.deleteRecursively(); }

    // Copies a fixture asset into the index root under `asName` and rescans.
    juce::File addAsset(const char* fixtureName, const juce::String& asName)
    {
        const auto f = assetRoot.getChildFile(asName);
        REQUIRE(fixture(fixtureName).copyFileTo(f));
        index.setRoots({ assetRoot });
        index.rescan();
        return f;
    }

    amp::PluginRef fakeGainRef() const
    {
        for (const auto& d : host->available())
            if (d.name == "FakeGain")
                return amp::PluginHost::refFor(d);
        FAIL("FakeGain not found");
        return {};
    }
};

amp::AssetRef assetRef(const juce::String& kind, const juce::String& file, const juce::String& sha)
{
    amp::AssetRef r;
    r.kind = kind;
    r.file = file;
    r.sha256 = sha;
    return r;
}

amp::NodeDoc nodeOf(amp::NodeType type, const juce::String& id)
{
    amp::NodeDoc n;
    n.id = id;
    n.type = type;
    return n;
}

// in -> middle... -> out
amp::RigDocument chainOf(std::vector<amp::NodeDoc> middle)
{
    amp::RigDocument doc;
    doc.id = juce::Uuid().toString();
    doc.name = "Test";
    doc.band = "My Tones";
    doc.nodes.push_back(nodeOf(amp::NodeType::Input, "in"));
    for (auto& n : middle)
        doc.nodes.push_back(std::move(n));
    doc.nodes.push_back(nodeOf(amp::NodeType::Output, "out"));
    for (size_t i = 0; i + 1 < doc.nodes.size(); ++i)
        doc.connections.emplace_back(doc.nodes[i].id, doc.nodes[i + 1].id);
    return doc;
}

amp::NodeDoc* findDoc(amp::RigDocument& doc, const juce::String& id)
{
    for (auto& n : doc.nodes)
        if (n.id == id)
            return &n;
    return nullptr;
}

const amp::NodeDoc* findDoc(const amp::RigDocument& doc, const juce::String& id)
{
    return findDoc(const_cast<amp::RigDocument&>(doc), id);
}

amp::NodeDoc* firstOfType(amp::RigDocument& doc, amp::NodeType type)
{
    for (auto& n : doc.nodes)
        if (n.type == type)
            return &n;
    return nullptr;
}

float paramOf(const juce::NamedValueSet& p, const char* name)
{
    return static_cast<float>(static_cast<double>(p[name]));
}

// Runs a 220 Hz sine (amplitude 0.1) through `node` for `blocks` blocks; returns the largest |out - in|.
float maxDeviation(amp::ProcessorNode& node, int blocks)
{
    juce::AudioBuffer<float> io(2, spec.maxBlock);
    float dev = 0.0f;
    int t = 0;
    for (int b = 0; b < blocks; ++b)
    {
        io.clear();
        for (int i = 0; i < spec.maxBlock; ++i)
            io.setSample(0, i, 0.1f * std::sin(2.0f * juce::MathConstants<float>::pi * 220.0f * (float) (t + i) / 48000.0f));
        juce::AudioBuffer<float> in;
        in.makeCopyOf(io);
        node.process(io, spec.maxBlock);
        for (int i = 0; i < spec.maxBlock; ++i)
            dev = juce::jmax(dev, std::abs(io.getSample(0, i) - in.getSample(0, i)));
        t += spec.maxBlock;
    }
    return dev;
}
} // namespace

TEST_CASE("rig_loader: builds default rig with fixture assets")
{
    Fixture f;
    f.addAsset("tiny.nam", "tiny.nam");
    f.addAsset("ir_48k.wav", "ir_48k.wav");

    auto doc = amp::makeDefaultRig("Default", "My Tones");
    auto* gateDoc = firstOfType(doc, amp::NodeType::Gate);
    auto* ampDoc = firstOfType(doc, amp::NodeType::NamAmp);
    auto* cabDoc = firstOfType(doc, amp::NodeType::CabIR);
    ampDoc->asset = assetRef("nam", "Models/tiny.nam", amp::AssetIndex::sha256Of(fixture("tiny.nam")));
    cabDoc->asset = assetRef("ir", "IRs/ir_48k.wav", amp::AssetIndex::sha256Of(fixture("ir_48k.wav")));

    juce::String error;
    auto live = f.loader->build(doc, spec, error);
    REQUIRE(live.has_value());
    REQUIRE(error.isEmpty());
    REQUIRE(live->report.missingAssets.isEmpty());
    REQUIRE(live->report.missingPlugins.isEmpty());
    REQUIRE(live->report.placeholderNodeIds.isEmpty());
    REQUIRE(live->report.resolvedByFilename.isEmpty());

    // Input/Output are not processor nodes: the plan is Gate -> NamAmp -> CabIR in chain order.
    const auto& nodes = live->plan->nodes();
    REQUIRE(nodes.size() == 3);
    REQUIRE(nodes[0]->id == gateDoc->id);
    REQUIRE(nodes[1]->id == ampDoc->id);
    REQUIRE(nodes[2]->id == cabDoc->id);
    for (const auto& pn : nodes)
        REQUIRE_FALSE(pn->bypass.load());
    REQUIRE(live->plan->spec().sampleRate == spec.sampleRate);
    REQUIRE(live->plan->spec().maxBlock == spec.maxBlock);

    // The model is loaded and the node prepared: it changes the signal.
    auto* namNode = dynamic_cast<amp::NamAmpNode*>(nodes[1]->node.get());
    REQUIRE(namNode != nullptr);
    REQUIRE(maxDeviation(*namNode, 8) > 1e-3f);

    // The IR is loaded and swaps in during processing.
    auto* cab = dynamic_cast<amp::CabIRNode*>(nodes[2]->node.get());
    REQUIRE(cab != nullptr);
    for (int i = 0; i < 400 && ! cab->irActive(); ++i)
    {
        maxDeviation(*cab, 1);
        juce::Thread::sleep(1);
    }
    REQUIRE(cab->irActive());
}

TEST_CASE("rig_loader: default rig without assets runs empty amp and cab")
{
    Fixture f;
    const auto doc = amp::makeDefaultRig("Default", "My Tones");
    juce::String error;
    auto live = f.loader->build(doc, spec, error);
    REQUIRE(live.has_value());
    REQUIRE(live->report.missingAssets.isEmpty());
    REQUIRE(live->report.placeholderNodeIds.isEmpty());
    const auto& nodes = live->plan->nodes();
    REQUIRE(nodes.size() == 3);
    REQUIRE(dynamic_cast<amp::NamAmpNode*>(nodes[1]->node.get()) != nullptr);
    REQUIRE(dynamic_cast<amp::CabIRNode*>(nodes[2]->node.get()) != nullptr);
    for (const auto& pn : nodes)
        REQUIRE_FALSE(pn->bypass.load());
}

TEST_CASE("rig_loader: missing NAM asset becomes bypassed placeholder and is reported")
{
    Fixture f;
    auto namDoc = nodeOf(amp::NodeType::NamAmp, "amp");
    namDoc.asset = assetRef("nam", "Models/gone.nam", "deadbeef");
    namDoc.params.set("inputGain", 3.0);
    auto cab = nodeOf(amp::NodeType::CabIR, "cab");
    cab.asset = assetRef("ir", "IRs/gone.wav", "feedface");
    const auto doc = chainOf({ nodeOf(amp::NodeType::Gate, "gate"), namDoc, cab });

    juce::String error;
    auto live = f.loader->build(doc, spec, error);
    REQUIRE(live.has_value());
    REQUIRE(live->report.missingAssets.contains("Models/gone.nam"));
    REQUIRE(live->report.missingAssets.contains("IRs/gone.wav"));
    REQUIRE(live->report.placeholderNodeIds == juce::StringArray { "amp", "cab" });

    auto* ampPlan = live->plan->find("amp");
    REQUIRE(ampPlan != nullptr);
    REQUIRE(ampPlan->bypass.load());
    REQUIRE(dynamic_cast<amp::NamAmpNode*>(ampPlan->node.get()) == nullptr);
    REQUIRE(live->plan->find("cab")->bypass.load());
    REQUIRE_FALSE(live->plan->find("gate")->bypass.load());

    // The original NodeDoc is kept untouched, and capture returns it unchanged.
    auto* kept = findDoc(live->doc, "amp");
    REQUIRE(kept->type == amp::NodeType::NamAmp);
    REQUIRE_FALSE(kept->bypass);
    REQUIRE(kept->asset->sha256 == "deadbeef");
    REQUIRE(paramOf(kept->params, "inputGain") == 3.0f);
    auto captured = f.loader->capture(*live);
    auto* capturedAmp = findDoc(captured, "amp");
    REQUIRE_FALSE(capturedAmp->bypass);
    REQUIRE(capturedAmp->asset->file == "Models/gone.nam");
    REQUIRE(capturedAmp->asset->sha256 == "deadbeef");
    REQUIRE(capturedAmp->params.size() == 1);
}

TEST_CASE("rig_loader: unknown node type becomes bypassed placeholder")
{
    Fixture f;
    const auto doc = chainOf({ nodeOf(amp::NodeType::Placeholder, "future") });
    juce::String error;
    auto live = f.loader->build(doc, spec, error);
    REQUIRE(live.has_value());
    REQUIRE(live->report.placeholderNodeIds == juce::StringArray { "future" });
    REQUIRE(live->plan->find("future")->bypass.load());
    REQUIRE_FALSE(findDoc(f.loader->capture(*live), "future")->bypass);
}

TEST_CASE("rig_loader: missing plugin keeps its state through capture")
{
    Fixture f;
    auto plug = nodeOf(amp::NodeType::Plugin, "plug");
    plug.plugin = amp::PluginRef { "VST3", "VST3-Gone-1234", "Gone Verb", "Nobody" };
    const char bytes[] = { 0x00, 0x01, char(0xff), 0x7f, 0x42, 0x00, char(0x80) };
    plug.state.replaceAll(bytes, sizeof(bytes));
    plug.bypass = false;
    const auto doc = chainOf({ plug });

    juce::String error;
    auto live = f.loader->build(doc, spec, error);
    REQUIRE(live.has_value());
    REQUIRE(live->report.missingPlugins == juce::StringArray { "Gone Verb" });
    REQUIRE(live->report.placeholderNodeIds == juce::StringArray { "plug" });
    REQUIRE(live->plan->find("plug")->bypass.load());

    const auto captured = f.loader->capture(*live);
    const auto reparsed = amp::parseRig(amp::serializeRig(captured));
    REQUIRE(reparsed.rig.has_value());
    const auto& n = reparsed.rig->nodes[1];
    REQUIRE(n.id == "plug");
    REQUIRE(n.type == amp::NodeType::Plugin);
    REQUIRE_FALSE(n.bypass);
    REQUIRE(n.plugin->uid == "VST3-Gone-1234");
    REQUIRE(n.state == juce::MemoryBlock(bytes, sizeof(bytes)));
}

TEST_CASE("rig_loader: asset resolved by hash after rename")
{
    Fixture f;
    f.addAsset("tiny.nam", "renamed_capture.nam");
    auto namDoc = nodeOf(amp::NodeType::NamAmp, "amp");
    const auto sha = amp::AssetIndex::sha256Of(fixture("tiny.nam"));
    namDoc.asset = assetRef("nam", "Models/tiny.nam", sha);

    juce::String error;
    auto live = f.loader->build(chainOf({ namDoc }), spec, error);
    REQUIRE(live.has_value());
    REQUIRE(live->report.missingAssets.isEmpty());
    REQUIRE(live->report.placeholderNodeIds.isEmpty());
    REQUIRE(live->report.resolvedByFilename.isEmpty());
    auto* node = dynamic_cast<amp::NamAmpNode*>(live->plan->find("amp")->node.get());
    REQUIRE(node != nullptr);
    REQUIRE(maxDeviation(*node, 8) > 1e-3f);
    REQUIRE(findDoc(f.loader->capture(*live), "amp")->asset->sha256 == sha);
}

TEST_CASE("rig_loader: asset matched by filename is reported and gets the file's hash on capture")
{
    Fixture f;
    const auto file = f.addAsset("tiny.nam", "tiny.nam");
    auto namDoc = nodeOf(amp::NodeType::NamAmp, "amp");
    namDoc.asset = assetRef("nam", "Models/tiny.nam", "0000stale");

    juce::String error;
    auto live = f.loader->build(chainOf({ namDoc }), spec, error);
    REQUIRE(live.has_value());
    REQUIRE(live->report.missingAssets.isEmpty());
    REQUIRE(live->report.placeholderNodeIds.isEmpty());
    REQUIRE(live->report.resolvedByFilename == juce::StringArray { file.getFullPathName() });
    REQUIRE(dynamic_cast<amp::NamAmpNode*>(live->plan->find("amp")->node.get()) != nullptr);
    REQUIRE(findDoc(live->doc, "amp")->asset->sha256 == "0000stale"); // doc untouched by build

    const auto captured = f.loader->capture(*live);
    const auto* capturedAmp = &captured.nodes[1];
    REQUIRE(capturedAmp->asset->sha256 == amp::AssetIndex::sha256Of(file));
    REQUIRE(capturedAmp->asset->file == "Models/tiny.nam");
}

TEST_CASE("rig_loader: capture reflects param and bypass changes")
{
    Fixture f;
    auto gate = nodeOf(amp::NodeType::Gate, "gate");
    gate.params.set("threshold", -40.0);
    gate.params.set("futureKnob", 7.0); // unknown to the node: preserved
    auto eq = nodeOf(amp::NodeType::EQ, "eq");
    eq.bypass = true;
    auto plug = nodeOf(amp::NodeType::Plugin, "plug");
    plug.plugin = f.fakeGainRef();
    const float initialGain = 0.75f; // FakeGain state = its normalised gain value
    plug.state.replaceAll(&initialGain, sizeof(initialGain));

    juce::String error;
    auto live = f.loader->build(chainOf({ gate, eq, plug }), spec, error);
    REQUIRE(live.has_value());
    REQUIRE(live->report.placeholderNodeIds.isEmpty());

    // Doc values were applied at build.
    auto* gatePlan = live->plan->find("gate");
    REQUIRE(paramOf(gatePlan->node->getParams(), "threshold") == -40.0f);
    REQUIRE(live->plan->find("eq")->bypass.load());
    auto* plugNode = dynamic_cast<amp::PluginNode*>(live->plan->find("plug")->node.get());
    REQUIRE(plugNode != nullptr);
    REQUIRE(plugNode->instance()->getHostedParameter(0)->getValue() == 0.75f);

    // Live changes.
    gatePlan->bypass.store(true);
    gatePlan->node->setParam("threshold", -30.0f);
    live->plan->find("eq")->bypass.store(false);
    live->plan->find("eq")->node->setParam("mid", 2.5f);
    plugNode->instance()->getHostedParameter(0)->setValue(0.125f);

    const auto captured = f.loader->capture(*live);
    REQUIRE(captured.nodes.size() == 5);
    const auto& g = captured.nodes[1];
    REQUIRE(g.bypass);
    REQUIRE(paramOf(g.params, "threshold") == -30.0f);
    REQUIRE(paramOf(g.params, "release") == 100.0f);
    REQUIRE(paramOf(g.params, "futureKnob") == 7.0f);
    const auto& e = captured.nodes[2];
    REQUIRE_FALSE(e.bypass);
    REQUIRE(paramOf(e.params, "mid") == 2.5f);
    const auto& p = captured.nodes[3];
    const float expected = 0.125f;
    REQUIRE(p.state == juce::MemoryBlock(&expected, sizeof(expected)));
    REQUIRE(p.plugin->uid == f.fakeGainRef().uid);
}

TEST_CASE("rig_loader: cyclic doc returns error")
{
    Fixture f;
    auto doc = chainOf({ nodeOf(amp::NodeType::Gate, "a"), nodeOf(amp::NodeType::EQ, "b") });
    doc.connections.emplace_back("b", "a");
    juce::String error;
    REQUIRE_FALSE(f.loader->build(doc, spec, error).has_value());
    REQUIRE(error.contains("cycle"));
}

TEST_CASE("rig_loader: non-linear doc returns error")
{
    Fixture f;
    auto doc = chainOf({ nodeOf(amp::NodeType::Gate, "a"), nodeOf(amp::NodeType::EQ, "b") });
    doc.connections = { { "in", "a" }, { "in", "b" }, { "a", "out" }, { "b", "out" } }; // parallel
    REQUIRE(amp::graph::topoOrder(doc).has_value());
    juce::String error;
    REQUIRE_FALSE(f.loader->build(doc, spec, error).has_value());
    REQUIRE(error.contains("chain"));
}
