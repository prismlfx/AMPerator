#include "FakeAudioDevice.h"

namespace amp_test
{
FakeDevice::FakeDevice(const juce::String& name, FakeType& owner) : juce::AudioIODevice(name, "Fake"), type(owner) {}

FakeDevice::~FakeDevice()
{
    stop();
}

juce::String FakeDevice::open(const juce::BigInteger& inputs, const juce::BigInteger& outputs, double sampleRate,
                              int bufferSize)
{
    type.openCalls.add(getName());
    if (type.failOpen || type.refuseOpen.contains(getName()))
        return lastError = "Fake device refused to open";
    if (! getAvailableSampleRates().contains(sampleRate) || ! getAvailableBufferSizes().contains(bufferSize))
        return lastError = "Unsupported rate or buffer size";
    activeIn.clear();
    activeOut.clear();
    activeIn.setBit(0, inputs[0]); // only channels that exist stay active
    for (int i = 0; i < 2; ++i)
        activeOut.setBit(i, outputs[i]);
    rate = sampleRate;
    buffer = bufferSize;
    opened = true;
    xruns = 0;
    lastError.clear();
    return {};
}

void FakeDevice::close()
{
    stop();
    if (opened)
        type.closeCalls.add(getName());
    opened = false;
}

void FakeDevice::start(juce::AudioIODeviceCallback* cb)
{
    if (! opened || cb == nullptr)
        return;
    callback = cb;
    cb->audioDeviceAboutToStart(this);
}

void FakeDevice::stop()
{
    if (auto* cb = std::exchange(callback, nullptr))
        cb->audioDeviceStopped();
}

int FakeDevice::getOutputLatencyInSamples() { return type.outputLatency; }
int FakeDevice::getInputLatencyInSamples() { return type.inputLatency; }

FakeType::FakeType() : juce::AudioIODeviceType("Fake"), names { "Fake Solo", "Fake Other" } {}

void FakeType::addDevice(const juce::String& name)
{
    names.addIfNotAlreadyThere(name);
}

void FakeType::removeDevice(const juce::String& name)
{
    names.removeString(name);
}

int FakeType::getDefaultDeviceIndex(bool) const
{
    return juce::jmax(0, names.indexOf(defaultName));
}

int FakeType::getIndexOfDevice(juce::AudioIODevice* d, bool) const
{
    return d != nullptr ? names.indexOf(d->getName()) : -1;
}

juce::AudioIODevice* FakeType::createDevice(const juce::String& outputName, const juce::String& inputName)
{
    const auto& name = outputName.isNotEmpty() ? outputName : inputName;
    return names.contains(name) ? new FakeDevice(name, *this) : nullptr;
}
} // namespace amp_test
