#pragma once

#include <atomic>
#include <functional>
#include <memory>

#include <juce_audio_devices/juce_audio_devices.h>

#include "engine/RealtimeEngine.h"
#include "storage/Settings.h"

namespace amp
{
enum class DeviceStatus
{
    Running,      // the requested (or fallback) device is open and playing
    Disconnected, // it was running and went away (unplugged / stopped); output is silent
    NoDevice      // nothing is open (never applied, or opening failed)
};

// Wraps a juce::AudioDeviceManager for the rig player: opens the interface named in Settings, falls back to
// the type's default device, reports latency / xruns, and reopens the same settings when the device comes back.
//
// Threading: everything here is message-thread only (including the ChangeListener notifications).
// The RealtimeEngine is registered as the manager's audio callback for the lifetime of the AudioIO.
// Channel routing: exactly the selected input channel and the selected output pair are enabled on the device,
// so inside the engine they are channel 0 and channels 0/1 (JUCE packs active channels).
class AudioIO : private juce::ChangeListener
{
public:
    AudioIO(juce::AudioDeviceManager&, RealtimeEngine&);
    ~AudioIO() override;

    // Opens the device named by `s` (type, name, rate, buffer, input channel, first output of a stereo pair).
    // An unknown / empty device name opens the device type's default device with input 0 and outputs 0/1 and
    // still returns ok -- call writeBack() to learn what actually opened. Out-of-range channels fall back the
    // same way. Fails (with the JUCE error message, status NoDevice) only if nothing could be opened.
    juce::Result apply(const Settings& s);

    // Copies the device that is actually open (type, name, rate, buffer, channels) into `s`. No device: unchanged.
    void writeBack(Settings& s) const;

    DeviceStatus status() const { return currentStatus; }

    // Xruns since the device opened; -1 when no device is running or the driver cannot count them.
    int xrunCount() const;

    // (inputLatency + outputLatency + 2 * buffer) / sampleRate * 1000; 0 without a running device.
    double roundTripLatencyMs() const;

    // Fired on the message thread when the status changes (not for repeated notifications of the same status).
    std::function<void(DeviceStatus)> onStatus;

    // Fired on the message thread exactly once per successful apply() / reconnect (after the status is Running),
    // and for driver-side restarts of the wanted device, with the rate and buffer size it now runs at. Never
    // fired for throwaway devices (the type's default device opened during a type switch, or JUCE's own fallback
    // after an unplug). The engine facade rebuilds its ProcessingPlan here, because RealtimeEngine refuses plans
    // prepared for a different sample rate. (Without a MessageManager -- headless tests -- it fires inline.)
    std::function<void(double sampleRate, int bufferSize)> onDeviceRestarted;

    juce::StringArray availableDevices() const;  // of the current device type
    juce::Array<double> sampleRates() const;      // of the open device; empty without one
    juce::Array<int> bufferSizes() const;

private:
    struct StartWatcher;

    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    bool deviceRunning() const;
    void setStatus(DeviceStatus);
    void notifyRestarted(double sampleRate, int bufferSize);
    void routeEngineChannels();
    juce::Result applyInternal(const Settings&, bool reconnecting);
    void setWanted(bool has, const juce::String& name);
    juce::AudioIODeviceType* findType(const Settings&) const;

    juce::AudioDeviceManager& manager;
    RealtimeEngine& engine;
    std::unique_ptr<StartWatcher> watcher;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    DeviceStatus currentStatus = DeviceStatus::NoDevice;
    std::atomic<bool> applying { false };
    bool hasWanted = false;
    juce::CriticalSection wantedNameLock; // the watcher reads the wanted name from driver threads
    juce::String wantedName;
    Settings wanted; // what to reapply on reconnect; audioDeviceType/Name are the devices actually chosen
    int wantedInput = 0, wantedOutputFirst = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioIO)
};
} // namespace amp
