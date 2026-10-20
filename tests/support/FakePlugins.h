#pragma once

// Fake in-memory plugins for PluginHost tests: a "Fake" AudioPluginFormat whose scan finds
// fake://gain (FakeGain), fake://boost (FakeBoost, also a FakeGain), fake://mono, fake://monostereo
// and fake://sidechain, so no system plugins are touched.

#include <atomic>
#include <cstring>
#include <memory>

#include <juce_audio_processors/juce_audio_processors.h>

#include "plugins/PluginHost.h"

namespace amp_test
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

// Set by tests that check the crash marker guards plugin teardown; FakeBase records whether the marker
// file existed while releaseResources() and the destructor ran.
inline juce::File gCrashMarker;
inline bool gMarkerDuringRelease = false;
inline bool gMarkerDuringDestruction = false;

// Boilerplate shared by the fake plugins.
class FakeBase : public juce::AudioPluginInstance
{
public:
    FakeBase(const BusesProperties& buses, juce::PluginDescription d) : AudioPluginInstance(buses), desc(std::move(d)) {}
    ~FakeBase() override { gMarkerDuringDestruction = gCrashMarker != juce::File() && gCrashMarker.exists(); }

    void fillInPluginDescription(juce::PluginDescription& d) const override { d = desc; }
    const juce::String getName() const override { return desc.name; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override { gMarkerDuringRelease = gCrashMarker != juce::File() && gCrashMarker.exists(); }
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
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

private:
    juce::PluginDescription desc;
};

class FakeGain final : public FakeBase
{
public:
    explicit FakeGain(juce::PluginDescription d)
        : FakeBase(BusesProperties()
                       .withInput("In", juce::AudioChannelSet::stereo())
                       .withOutput("Out", juce::AudioChannelSet::stereo()),
                   std::move(d))
    {
        auto p = std::make_unique<FakeGainParam>();
        gain = p.get();
        addHostedParameter(std::move(p));
    }

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        buffer.applyGain(2.0f * gain->getValue());
    }
    bool isBusesLayoutSupported(const BusesLayout& l) const override
    {
        return l.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
               && l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }
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
};

// Mono in/mono out only. Output = 0.5 * input.
class FakeMono final : public FakeBase
{
public:
    explicit FakeMono(juce::PluginDescription d)
        : FakeBase(BusesProperties()
                       .withInput("In", juce::AudioChannelSet::mono())
                       .withOutput("Out", juce::AudioChannelSet::mono()),
                   std::move(d)) {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        channelsSeen = buffer.getNumChannels();
        buffer.applyGain(0.5f);
    }
    bool isBusesLayoutSupported(const BusesLayout& l) const override
    {
        return l.getMainInputChannelSet() == juce::AudioChannelSet::mono()
               && l.getMainOutputChannelSet() == juce::AudioChannelSet::mono();
    }

    int channelsSeen = -1;
};

// Defaults to mono/mono but also supports mono in/stereo out, which the host should negotiate.
// Output: L = 0.5 * in, R = 0.25 * in.
class FakeMonoToStereo final : public FakeBase
{
public:
    explicit FakeMonoToStereo(juce::PluginDescription d)
        : FakeBase(BusesProperties()
                       .withInput("In", juce::AudioChannelSet::mono())
                       .withOutput("Out", juce::AudioChannelSet::mono()),
                   std::move(d)) {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        channelsSeen = buffer.getNumChannels();
        if (buffer.getNumChannels() < 2)
            return;
        buffer.copyFrom(1, 0, buffer, 0, 0, buffer.getNumSamples());
        buffer.applyGain(0, 0, buffer.getNumSamples(), 0.5f);
        buffer.applyGain(1, 0, buffer.getNumSamples(), 0.25f);
    }
    bool isBusesLayoutSupported(const BusesLayout& l) const override
    {
        return l.getMainInputChannelSet() == juce::AudioChannelSet::mono()
               && (l.getMainOutputChannelSet() == juce::AudioChannelSet::mono()
                   || l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo());
    }

    int channelsSeen = -1;
};

// Stereo main in + stereo sidechain in (4 input channels), stereo out. Output = 0.5 * main input.
// Records the sidechain's peak, then scribbles over it so the next block shows whether the host cleared it.
class FakeSidechain final : public FakeBase
{
public:
    explicit FakeSidechain(juce::PluginDescription d)
        : FakeBase(BusesProperties()
                       .withInput("In", juce::AudioChannelSet::stereo())
                       .withInput("Sidechain", juce::AudioChannelSet::stereo())
                       .withOutput("Out", juce::AudioChannelSet::stereo()),
                   std::move(d)) {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        channelsSeen = buffer.getNumChannels();
        if (buffer.getNumChannels() < 4)
            return;
        const int n = buffer.getNumSamples();
        sidechainPeak = juce::jmax(buffer.getMagnitude(2, 0, n), buffer.getMagnitude(3, 0, n));
        for (int ch = 2; ch < 4; ++ch)
            juce::FloatVectorOperations::fill(buffer.getWritePointer(ch), 9.0f, n);
        buffer.applyGain(0, 0, n, 0.5f);
        buffer.applyGain(1, 0, n, 0.5f);
    }
    bool isBusesLayoutSupported(const BusesLayout& l) const override
    {
        return l.inputBuses.size() == 2 && l.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
               && l.getChannelSet(true, 1) == juce::AudioChannelSet::stereo()
               && l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
    }

    int channelsSeen = -1;
    float sidechainPeak = -1.0f;
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
        return { "fake://gain", "fake://boost", "fake://mono", "fake://monostereo", "fake://sidechain" };
    }
    juce::FileSearchPath getDefaultLocationsToSearch() override { return {}; }
    bool requiresUnblockedMessageThreadDuringCreation(const juce::PluginDescription&) const override { return false; }

protected:
    void createPluginInstance(const juce::PluginDescription& d, double, int, PluginCreationCallback cb) override
    {
        const auto& id = d.fileOrIdentifier;
        if (id == "fake://mono")
            cb(std::make_unique<FakeMono>(d), {});
        else if (id == "fake://monostereo")
            cb(std::make_unique<FakeMonoToStereo>(d), {});
        else if (id == "fake://sidechain")
            cb(std::make_unique<FakeSidechain>(d), {});
        else
            cb(std::make_unique<FakeGain>(d), {});
    }

private:
    static juce::String nameFor(const juce::String& id)
    {
        if (id == "fake://gain") return "FakeGain";
        if (id == "fake://mono") return "FakeMono";
        if (id == "fake://monostereo") return "FakeMonoToStereo";
        if (id == "fake://sidechain") return "FakeSidechain";
        return "FakeBoost";
    }

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

// A PluginHost over FakeFormat only (no default formats), with its data files in dataDir.
std::unique_ptr<amp::PluginHost> makeFakePluginHost(const juce::File& dataDir);
} // namespace amp_test
