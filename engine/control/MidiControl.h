#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include <map>
#include <memory>
#include <mutex>
#include <optional>

#include "control/ControlEvent.h"

namespace amp
{
/** Listens to every MIDI input (re-checking for new/removed devices every 2 s). Notes and CCs become
    "midi:<device>:note<n>" / "midi:<device>:cc<n>" (device name verbatim). CC value >= 64 = pressed, < 64 = released,
    edge-detected per device and controller. Events arrive on the message thread. */
class MidiControl : public ControlSource, private juce::MidiInputCallback, private juce::Timer
{
public:
    ~MidiControl() override;

    void start(); // message thread
    void stop();  // message thread; no event is delivered after it returns

    /** Pure translation (plus CC edge state); thread-safe. Used by handleMidi and by tests. */
    std::optional<ControlEvent> translate(const juce::MidiMessage&, const juce::String& device);
    /** translate() + asynchronous delivery to onEvent on the message thread. */
    void handleMidi(const juce::MidiMessage&, const juce::String& device);

private:
    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage&) override;
    void timerCallback() override; // device rescan
    void refreshDevices();

    std::map<juce::String, std::unique_ptr<juce::MidiInput>> inputs; // by device identifier
    std::mutex ccLock;
    std::map<juce::String, bool> ccDown; // control ID -> last state
};
} // namespace amp
