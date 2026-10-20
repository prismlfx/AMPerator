#include "audio/AudioIO.h"

namespace amp
{
// Second, no-op audio callback whose only job is to hear about device (re)starts.
namespace
{
constexpr int silentOutputPair = 1 << 20; // beyond any real device: every output channel is zeroed
}

struct AudioIO::StartWatcher : juce::AudioIODeviceCallback
{
    explicit StartWatcher(AudioIO& o) : owner(o) {}

    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const*, int, int,
                                          const juce::AudioIODeviceCallbackContext&) override
    {
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* d) override
    {
        if (owner.applying)
            return; // apply() reports the final device once
        {
            const juce::ScopedLock sl(owner.wantedNameLock);
            if (owner.wantedName.isNotEmpty() && d->getName() != owner.wantedName)
            {
                // JUCE's own fallback after an unplug (or an outside switch): keep it silent until the message
                // thread closes or adopts it.
                owner.engine.setOutputPair(silentOutputPair);
                return;
            }
        }
        owner.notifyRestarted(d->getCurrentSampleRate(), d->getCurrentBufferSizeSamples());
    }

    void audioDeviceStopped() override {}

    AudioIO& owner;
};

AudioIO::AudioIO(juce::AudioDeviceManager& m, RealtimeEngine& e) : manager(m), engine(e)
{
    watcher = std::make_unique<StartWatcher>(*this);
    manager.addAudioCallback(&engine);
    manager.addAudioCallback(watcher.get());
    manager.addChangeListener(this);
}

AudioIO::~AudioIO()
{
    *alive = false;
    manager.removeChangeListener(this);
    manager.removeAudioCallback(watcher.get());
    manager.removeAudioCallback(&engine);
}

void AudioIO::notifyRestarted(double sampleRate, int bufferSize)
{
    if (! onDeviceRestarted)
        return;
    // Drivers can restart from their own thread; hop to the message thread when there is one to hop to.
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (mm == nullptr || mm->isThisTheMessageThread())
    {
        onDeviceRestarted(sampleRate, bufferSize);
        return;
    }
    juce::MessageManager::callAsync([this, token = alive, sampleRate, bufferSize]
    {
        if (*token && onDeviceRestarted)
            onDeviceRestarted(sampleRate, bufferSize);
    });
}

void AudioIO::setWanted(bool has, const juce::String& name)
{
    hasWanted = has;
    const juce::ScopedLock sl(wantedNameLock);
    wantedName = has ? name : juce::String();
}

bool AudioIO::deviceRunning() const
{
    auto* d = manager.getCurrentAudioDevice();
    return d != nullptr && d->isOpen() && d->isPlaying();
}

void AudioIO::setStatus(DeviceStatus s)
{
    if (s == currentStatus)
        return;
    currentStatus = s;
    if (onStatus)
        onStatus(s);
}

juce::AudioIODeviceType* AudioIO::findType(const Settings& s) const
{
    auto& types = manager.getAvailableDeviceTypes(); // also scans on first use
    auto hasDevices = [](juce::AudioIODeviceType* t) { return ! t->getDeviceNames(false).isEmpty() || ! t->getDeviceNames(true).isEmpty(); };
    for (auto* t : types)
        if (s.audioDeviceType.isNotEmpty() && t->getTypeName() == s.audioDeviceType && hasDevices(t))
            return t;
    if (s.audioDeviceName.isNotEmpty())
        for (auto* t : types)
            if (t->getDeviceNames(false).contains(s.audioDeviceName) || t->getDeviceNames(true).contains(s.audioDeviceName))
                return t;
    if (auto* current = manager.getCurrentDeviceTypeObject())
        if (hasDevices(current))
            return current;
    for (auto* t : types)
        if (hasDevices(t))
            return t;
    return nullptr;
}

juce::Result AudioIO::apply(const Settings& s)
{
    return applyInternal(s, false);
}

juce::Result AudioIO::applyInternal(const Settings& s, bool reconnecting)
{
    struct Guard
    {
        explicit Guard(std::atomic<bool>& f) : flag(f) { flag = true; }
        ~Guard() { flag = false; }
        std::atomic<bool>& flag;
    } guard(applying);

    auto fail = [this, reconnecting](const juce::String& message)
    {
        if (! reconnecting) // a failed reconnect stays Disconnected and keeps the wanted settings
        {
            setWanted(false, {});
            setStatus(DeviceStatus::NoDevice);
        }
        return juce::Result::fail(message);
    };

    auto* type = findType(s);
    if (type == nullptr)
        return fail("No audio device found");

    const auto outputNames = type->getDeviceNames(false);
    const auto inputNames = type->getDeviceNames(true);

    if (outputNames.isEmpty() && inputNames.isEmpty())
        return fail("No audio device found");

    juce::String name = s.audioDeviceName;
    bool usedFallback = false;
    if (name.isEmpty() || ! (outputNames.contains(name) || inputNames.contains(name)))
    {
        usedFallback = true;
        const auto& names = outputNames.isEmpty() ? inputNames : outputNames;
        name = names[juce::jlimit(0, names.size() - 1, type->getDefaultDeviceIndex(outputNames.isEmpty()))];
    }

    // Probe the device (unopened) for its channel counts so exactly the wanted channels can be enabled.
    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.outputDeviceName = outputNames.contains(name) ? name : juce::String();
    setup.inputDeviceName = inputNames.contains(name) ? name : juce::String();
    setup.sampleRate = s.sampleRate;
    setup.bufferSize = s.bufferSize;
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = false;

    int numIn = 0, numOut = 0;
    {
        std::unique_ptr<juce::AudioIODevice> probe(type->createDevice(setup.outputDeviceName, setup.inputDeviceName));
        if (probe == nullptr)
            return fail("Can't open the audio device: " + name);
        numIn = probe->getInputChannelNames().size();
        numOut = probe->getOutputChannelNames().size();
    }

    const int inCh = (usedFallback || s.inputChannel < 0 || s.inputChannel >= numIn) ? 0 : s.inputChannel;
    const int outFirst = (usedFallback || s.outputFirstChannel < 0 || s.outputFirstChannel >= numOut) ? 0 : s.outputFirstChannel;
    if (numIn > 0)
        setup.inputChannels.setBit(inCh);
    for (int c = outFirst; c < juce::jmin(numOut, outFirst + 2); ++c)
        setup.outputChannels.setBit(c);

    if (manager.getCurrentAudioDeviceType() != type->getTypeName())
        manager.setCurrentAudioDeviceType(type->getTypeName(), false);

    // A device the driver stopped is still "current"; setAudioDeviceSetup would no-op on an identical setup.
    if (manager.getCurrentAudioDevice() != nullptr && ! deviceRunning())
        manager.closeAudioDevice();

    const auto error = manager.setAudioDeviceSetup(setup, true);
    if (error.isNotEmpty())
        return fail(error);

    if (! deviceRunning())
    {
        manager.closeAudioDevice();
        return fail("The audio device did not start");
    }

    setWanted(true, name);
    wanted = s;
    wanted.audioDeviceType = type->getTypeName();
    wanted.audioDeviceName = name;
    wantedInput = inCh;
    wantedOutputFirst = outFirst;

    routeEngineChannels();
    setStatus(DeviceStatus::Running);
    auto* dev = manager.getCurrentAudioDevice();
    notifyRestarted(dev->getCurrentSampleRate(), dev->getCurrentBufferSizeSamples());
    return juce::Result::ok();
}

void AudioIO::routeEngineChannels()
{
    auto* d = manager.getCurrentAudioDevice();
    if (d == nullptr)
        return;
    // JUCE hands the callback only the active channels, packed; translate device channels to packed indices.
    auto packedIndex = [](const juce::BigInteger& active, int channel)
    {
        if (! active[channel])
            return -1;
        int rank = 0;
        for (int i = 0; i < channel; ++i)
            rank += active[i] ? 1 : 0;
        return rank;
    };
    const auto in = d->getActiveInputChannels();
    const auto out = d->getActiveOutputChannels();
    const int inIdx = packedIndex(in, wantedInput);
    const int outIdx = packedIndex(out, wantedOutputFirst);
    engine.setInputChannel(inIdx >= 0 ? inIdx : in.countNumberOfSetBits()); // missing input = silence
    engine.setOutputPair(outIdx >= 0 ? outIdx : 0);
}

void AudioIO::writeBack(Settings& s) const
{
    auto* d = manager.getCurrentAudioDevice();
    if (d == nullptr)
        return;
    s.audioDeviceType = d->getTypeName();
    s.audioDeviceName = d->getName();
    s.sampleRate = d->getCurrentSampleRate();
    s.bufferSize = d->getCurrentBufferSizeSamples();
    const auto in = d->getActiveInputChannels();
    const auto out = d->getActiveOutputChannels();
    s.inputChannel = in.isZero() ? 0 : in.findNextSetBit(0);
    s.outputFirstChannel = out.isZero() ? 0 : out.findNextSetBit(0);
}

int AudioIO::xrunCount() const
{
    return deviceRunning() ? manager.getCurrentAudioDevice()->getXRunCount() : -1;
}

double AudioIO::roundTripLatencyMs() const
{
    if (! deviceRunning())
        return 0.0;
    auto* d = manager.getCurrentAudioDevice();
    const double sr = d->getCurrentSampleRate();
    if (sr <= 0.0)
        return 0.0;
    return (d->getInputLatencyInSamples() + d->getOutputLatencyInSamples() + 2.0 * d->getCurrentBufferSizeSamples()) / sr * 1000.0;
}

juce::StringArray AudioIO::availableDevices() const
{
    juce::StringArray names;
    if (auto* t = manager.getCurrentDeviceTypeObject())
    {
        names.addArray(t->getDeviceNames(false));
        for (auto& n : t->getDeviceNames(true))
            names.addIfNotAlreadyThere(n);
    }
    return names;
}

juce::Array<double> AudioIO::sampleRates() const
{
    auto* d = manager.getCurrentAudioDevice();
    return d != nullptr ? d->getAvailableSampleRates() : juce::Array<double>();
}

juce::Array<int> AudioIO::bufferSizes() const
{
    auto* d = manager.getCurrentAudioDevice();
    return d != nullptr ? d->getAvailableBufferSizes() : juce::Array<int>();
}

void AudioIO::changeListenerCallback(juce::ChangeBroadcaster*)
{
    if (applying || ! hasWanted)
        return;

    auto* d = manager.getCurrentAudioDevice();
    const bool running = deviceRunning();
    if (running && d->getName() == wanted.audioDeviceName)
    {
        setStatus(DeviceStatus::Running); // e.g. the manager reopened it by itself
        return;
    }

    bool wantedListed = false;
    for (auto* t : manager.getAvailableDeviceTypes())
        if (t->getTypeName() == wanted.audioDeviceType)
            wantedListed = t->getDeviceNames(false).contains(wanted.audioDeviceName)
                        || t->getDeviceNames(true).contains(wanted.audioDeviceName);

    if (running && wantedListed)
    {
        // Someone switched devices outside apply(): adopt what is running instead of fighting it.
        writeBack(wanted);
        setWanted(true, wanted.audioDeviceName);
        wantedInput = wanted.inputChannel;
        wantedOutputFirst = wanted.outputFirstChannel;
        routeEngineChannels();
        setStatus(DeviceStatus::Running);
        notifyRestarted(d->getCurrentSampleRate(), d->getCurrentBufferSizeSamples());
        return;
    }

    // The wanted device is gone or stopped. The manager may have switched to some other device on its own
    // (it falls back to a default when the current one vanishes); this app wants silence instead.
    if (d != nullptr)
        manager.closeAudioDevice();
    setStatus(DeviceStatus::Disconnected);

    if (wantedListed)
        applyInternal(Settings(wanted), true); // back again: same settings
}
} // namespace amp
