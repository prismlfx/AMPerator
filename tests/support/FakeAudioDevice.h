#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

namespace amp_test
{
class FakeType;

// Fake device: 1 input ("In 1"), 2 outputs ("Out 1", "Out 2"), rates {44100, 48000}, buffers {64, 128, 256}.
// There is no audio thread: start() just stores the callback and announces the device to it.
class FakeDevice : public juce::AudioIODevice
{
public:
    FakeDevice(const juce::String& name, FakeType& owner);
    ~FakeDevice() override;

    juce::StringArray getOutputChannelNames() override { return { "Out 1", "Out 2" }; }
    juce::StringArray getInputChannelNames() override { return { "In 1" }; }
    juce::Array<double> getAvailableSampleRates() override { return { 44100.0, 48000.0 }; }
    juce::Array<int> getAvailableBufferSizes() override { return { 64, 128, 256 }; }
    int getDefaultBufferSize() override { return 128; }

    juce::String open(const juce::BigInteger& inputs, const juce::BigInteger& outputs, double sampleRate,
                      int bufferSize) override;
    void close() override;
    bool isOpen() override { return opened; }
    void start(juce::AudioIODeviceCallback*) override;
    void stop() override;
    bool isPlaying() override { return callback != nullptr; }
    juce::String getLastError() override { return lastError; }

    int getCurrentBufferSizeSamples() override { return buffer; }
    double getCurrentSampleRate() override { return rate; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return activeOut; }
    juce::BigInteger getActiveInputChannels() const override { return activeIn; }
    int getOutputLatencyInSamples() override;
    int getInputLatencyInSamples() override;
    int getXRunCount() const noexcept override { return xruns; }

    int xruns = 0; // tests poke this directly

private:
    FakeType& type;
    juce::AudioIODeviceCallback* callback = nullptr;
    bool opened = false;
    double rate = 0;
    int buffer = 0;
    juce::BigInteger activeIn, activeOut;
    juce::String lastError;
};

// Device type "Fake" whose device list can change at runtime. Starts with "Fake Solo" and "Fake Other"
// (default device: "Fake Other").
class FakeType : public juce::AudioIODeviceType
{
public:
    FakeType();

    void addDevice(const juce::String& name);
    void removeDevice(const juce::String& name);
    /** Announce a list change to the AudioDeviceManager, as the OS would after a hot-plug. */
    void notifyDeviceListChanged() { callDeviceChangeListeners(); }

    void scanForDevices() override {}
    juce::StringArray getDeviceNames(bool) const override { return names; }
    int getDefaultDeviceIndex(bool) const override;
    int getIndexOfDevice(juce::AudioIODevice*, bool) const override;
    bool hasSeparateInputsAndOutputs() const override { return false; }
    juce::AudioIODevice* createDevice(const juce::String& outputName, const juce::String& inputName) override;

    int inputLatency = 0, outputLatency = 0; // applied to devices when queried
    bool failOpen = false;                   // open() of any device reports an error
    juce::StringArray refuseOpen;            // devices whose open() reports an error (listed but busy / hogged)
    juce::StringArray openCalls;             // every open() attempt, by device name
    juce::StringArray closeCalls;            // every close() of an open device, by device name

    juce::StringArray names;
    juce::String defaultName = "Fake Other";
};
} // namespace amp_test
