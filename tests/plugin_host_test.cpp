#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cmath>
#include <cstring>
#include <optional>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include "engine/RtSafety.h"
#include "plugins/PluginHost.h"

namespace
{
// Normalised value v in [0, 1] maps to linear gain 2v (default 0.5 = unity).
class FakeGainParam final : public juce::HostedAudioProcessorParameter
{
public:
    float getValue() const override { return value.load(); }
    void setValue(float v) override { value.store(v); }
    float getDefaultValue() const override { return 0.5f; }
    juce::String getName(int maxLen) const override { return juce::String("gain").substring(0, maxLen); }
    juce::String getLabel() const override { return {}; }
    float getValueForText(const juce::String& text) const override { return text.getFloatValue(); }
    juce::String getParameterID() const override { return "gain"; }

    std::atomic<float> value { 0.5f };
};

class FakeGain final : public juce::AudioPluginInstance
{
public:
    explicit FakeGain(juce::PluginDescription d)
        : AudioPluginInstance(BusesProperties()
                                  .withInput("In", juce::AudioChannelSet::stereo())
                                  .withOutput("Out", juce::AudioChannelSet::stereo())),
          desc(std::move(d))
    {
        auto p = std::make_unique<FakeGainParam>();
        gain = p.get();
        addHostedParameter(std::move(p));
    }

    void fillInPluginDescription(juce::PluginDescription& d) const override { d = desc; }
    const juce::String getName() const override { return desc.name; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        buffer.applyGain(2.0f * gain->getValue());
    }
    bool isBusesLayoutSupported(const BusesLayout& l) const override
    {
        return l.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
               && l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& dest) override
    {
        const float v = gain->getValue();
        dest.replaceAll(&v, sizeof(v));
    }
    void setStateInformation(const void* data, int size) override
    {
        if (size == (int) sizeof(float))
        {
            float v;
            std::memcpy(&v, data, sizeof(v));
            gain->setValue(v);
        }
    }

    FakeGainParam* gain = nullptr;

private:
    juce::PluginDescription desc;
};

class FakeFormat final : public juce::AudioPluginFormat
{
public:
    juce::String getName() const override { return "Fake"; }

    void findAllTypesForFile(juce::OwnedArray<juce::PluginDescription>& results, const juce::String& id) override
    {
        if (fileMightContainThisPluginType(id))
            results.add(new juce::PluginDescription(describe(id)));
    }
    bool fileMightContainThisPluginType(const juce::String& id) override { return id.startsWith("fake://"); }
    juce::String getNameOfPluginFromIdentifier(const juce::String& id) override { return nameFor(id); }
    bool pluginNeedsRescanning(const juce::PluginDescription&) override { return false; }
    bool doesPluginStillExist(const juce::PluginDescription&) override { return true; }
    bool canScanForPlugins() const override { return true; }
    bool isTrivialToScan() const override { return true; }
    juce::StringArray searchPathsForPlugins(const juce::FileSearchPath&, bool, bool) override
    {
        return { "fake://gain", "fake://boost" };
    }
    juce::FileSearchPath getDefaultLocationsToSearch() override { return {}; }
    bool requiresUnblockedMessageThreadDuringCreation(const juce::PluginDescription&) const override { return false; }

protected:
    void createPluginInstance(const juce::PluginDescription& d, double, int, PluginCreationCallback cb) override
    {
        cb(std::make_unique<FakeGain>(d), {});
    }

private:
    static juce::String nameFor(const juce::String& id) { return id == "fake://gain" ? "FakeGain" : "FakeBoost"; }

    static juce::PluginDescription describe(const juce::String& id)
    {
        juce::PluginDescription d;
        d.name = d.descriptiveName = nameFor(id);
        d.pluginFormatName = "Fake";
        d.manufacturerName = "Test Vendor";
        d.category = "Effect";
        d.version = "1.0";
        d.fileOrIdentifier = id;
        d.uniqueId = id.hashCode();
        d.numInputChannels = 2;
        d.numOutputChannels = 2;
        return d;
    }
};

struct Fixture
{
    const juce::ScopedJuceInitialiser_GUI juce; // this thread becomes the message thread
    juce::TemporaryFile tmp;
    juce::File dir = tmp.getFile();

    Fixture() { dir.createDirectory(); }
    ~Fixture() { dir.deleteRecursively(); }

    std::unique_ptr<amp::PluginHost> makeHost() const
    {
        std::vector<std::unique_ptr<juce::AudioPluginFormat>> formats;
        formats.push_back(std::make_unique<FakeFormat>());
        return std::make_unique<amp::PluginHost>(dir, std::move(formats), false);
    }
};

std::optional<juce::PluginDescription> findByName(const juce::Array<juce::PluginDescription>& list,
                                                  const juce::String& name)
{
    for (const auto& d : list)
        if (d.name == name)
            return d;
    return std::nullopt;
}

const amp::ProcessSpec spec { 48000.0, 256 };
} // namespace

TEST_CASE("plugin_host: create known plugin returns node")
{
    Fixture f;
    auto host = f.makeHost();
    host->scan([](float) {});
    const auto desc = findByName(host->available(), "FakeGain");
    REQUIRE(desc.has_value());

    const auto ref = amp::PluginHost::refFor(*desc);
    REQUIRE(ref.format == "Fake");
    REQUIRE(ref.uid == desc->createIdentifierString());
    REQUIRE(ref.name == "FakeGain");
    REQUIRE(ref.vendor == "Test Vendor");

    juce::String error;
    auto node = host->create(ref, spec, error);
    REQUIRE(node != nullptr);
    REQUIRE(error.isEmpty());
    REQUIRE(node->isStereo());
    REQUIRE_FALSE(f.dir.getChildFile("crash-marker").exists()); // cleared after creation

    auto* pluginNode = dynamic_cast<amp::PluginNode*>(node.get());
    REQUIRE(pluginNode != nullptr);
    REQUIRE(pluginNode->instance() != nullptr);
    pluginNode->instance()->getHostedParameter(0)->setValue(0.25f); // linear gain 0.5

    node->prepare(spec);
    REQUIRE_FALSE(f.dir.getChildFile("crash-marker").exists()); // cleared after first prepare
    juce::AudioBuffer<float> io(2, spec.maxBlock);
    for (int ch = 0; ch < 2; ++ch)
        juce::FloatVectorOperations::fill(io.getWritePointer(ch), 0.8f, spec.maxBlock);
    amp::RtSafety::reset();
    {
        const amp::RtSafety::ScopedNoAlloc noAlloc;
        node->process(io, 128);
    }
    REQUIRE(amp::RtSafety::violations() == 0);
    REQUIRE(std::abs(io.getSample(0, 0) - 0.4f) < 1e-6f);
    REQUIRE(std::abs(io.getSample(1, 127) - 0.4f) < 1e-6f);
    REQUIRE(std::abs(io.getSample(0, 128) - 0.8f) < 1e-6f); // beyond numSamples: untouched
}

TEST_CASE("plugin_host: format and name fall back when uid is unknown")
{
    Fixture f;
    auto host = f.makeHost();
    host->scan([](float) {});
    juce::String error;
    auto node = host->create({ "Fake", "Fake-FakeGain-stale-uid", "FakeGain", "" }, spec, error);
    REQUIRE(node != nullptr);
}

TEST_CASE("plugin_host: unknown uid returns null with error")
{
    Fixture f;
    auto host = f.makeHost();
    host->scan([](float) {});
    juce::String error;
    auto node = host->create({ "Fake", "Fake-Nope-123", "Nope", "Nobody" }, spec, error);
    REQUIRE(node == nullptr);
    REQUIRE(error.isNotEmpty());
}

TEST_CASE("plugin_host: state round trips")
{
    Fixture f;
    auto host = f.makeHost();
    host->scan([](float) {});
    const auto ref = amp::PluginHost::refFor(*findByName(host->available(), "FakeGain"));
    juce::String error;
    auto a = host->create(ref, spec, error);
    auto b = host->create(ref, spec, error);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);

    dynamic_cast<amp::PluginNode&>(*a).instance()->getHostedParameter(0)->setValue(0.125f);
    const auto state = a->getState();
    REQUIRE(state.getSize() > 0);
    b->setState(state);
    REQUIRE(dynamic_cast<amp::PluginNode&>(*b).instance()->getHostedParameter(0)->getValue() == 0.125f);
}

TEST_CASE("plugin_host: crash marker on startup blacklists that plugin")
{
    Fixture f;
    juce::String uid;
    {
        auto host = f.makeHost();
        host->scan([](float) {});
        uid = amp::PluginHost::refFor(*findByName(host->available(), "FakeGain")).uid;
    }
    // Simulate a crash during creation: the marker survives because the process died.
    REQUIRE(f.dir.getChildFile("crash-marker").replaceWithText(uid));

    auto host = f.makeHost(); // reloads known-plugins.xml, no rescan
    host->checkCrashMarkerOnStartup();
    REQUIRE(host->blacklisted().contains(uid));
    REQUIRE_FALSE(f.dir.getChildFile("crash-marker").exists());
    REQUIRE(f.dir.getChildFile("blacklist.txt").loadFileAsString().contains(uid));

    host->checkCrashMarkerOnStartup(); // no marker: nothing changes, no duplicate entry
    REQUIRE(host->blacklisted().size() == 1);
}

TEST_CASE("plugin_host: blacklisted plugin cannot be created and is hidden from available()")
{
    Fixture f;
    auto host = f.makeHost();
    host->scan([](float) {});
    const auto ref = amp::PluginHost::refFor(*findByName(host->available(), "FakeGain"));
    REQUIRE(f.dir.getChildFile("crash-marker").replaceWithText(ref.uid));
    host->checkCrashMarkerOnStartup();

    REQUIRE_FALSE(findByName(host->available(), "FakeGain").has_value());
    REQUIRE(findByName(host->available(), "FakeBoost").has_value());
    juce::String error;
    REQUIRE(host->create(ref, spec, error) == nullptr);
    REQUIRE(error.isNotEmpty());
}

TEST_CASE("plugin_host: unblacklist restores it")
{
    Fixture f;
    auto host = f.makeHost();
    host->scan([](float) {});
    const auto ref = amp::PluginHost::refFor(*findByName(host->available(), "FakeGain"));
    REQUIRE(f.dir.getChildFile("crash-marker").replaceWithText(ref.uid));
    host->checkCrashMarkerOnStartup();

    host->unblacklist(ref.uid);
    REQUIRE(host->blacklisted().isEmpty());
    REQUIRE(findByName(host->available(), "FakeGain").has_value());
    juce::String error;
    REQUIRE(host->create(ref, spec, error) != nullptr);
    REQUIRE_FALSE(f.dir.getChildFile("blacklist.txt").loadFileAsString().contains(ref.uid));

    auto reloaded = f.makeHost(); // blacklist.txt persists the removal
    REQUIRE(reloaded->blacklisted().isEmpty());
}

TEST_CASE("plugin_host: a plugin file that crashed the scan is skipped on the next scan")
{
    Fixture f;
    SECTION("our crash marker") { REQUIRE(f.dir.getChildFile("crash-marker").replaceWithText("fake://boost")); }
    SECTION("JUCE dead-man's pedal") { REQUIRE(f.dir.getChildFile("scan-in-progress").replaceWithText("fake://boost")); }

    auto host = f.makeHost();
    host->checkCrashMarkerOnStartup();
    REQUIRE(host->blacklisted().contains("fake://boost"));

    float lastProgress = 0.0f;
    host->scan([&](float p) { lastProgress = p; });
    REQUIRE(lastProgress == 1.0f);
    REQUIRE(findByName(host->available(), "FakeGain").has_value());
    REQUIRE_FALSE(findByName(host->available(), "FakeBoost").has_value());
    REQUIRE_FALSE(f.dir.getChildFile("crash-marker").exists());
    REQUIRE_FALSE(f.dir.getChildFile("scan-in-progress").loadFileAsString().contains("fake://"));
}
