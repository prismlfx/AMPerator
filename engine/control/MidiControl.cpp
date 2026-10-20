#include "control/MidiControl.h"

namespace amp
{
namespace
{
constexpr int rescanIntervalMs = 2000;
}

MidiControl::~MidiControl() { stop(); }

void MidiControl::start()
{
    refreshDevices();
    startTimer(rescanIntervalMs);
}

void MidiControl::stop()
{
    stopTimer();
    for (auto& [id, input] : inputs)
        input->stop();
    inputs.clear(); // closing the ports waits out any callback in flight
    {
        std::lock_guard lock(ccLock);
        ccDown.clear();
    }
    cancelPending();
}

std::optional<ControlEvent> MidiControl::translate(const juce::MidiMessage& m, const juce::String& device)
{
    auto prefix = "midi:" + device + ":";

    if (m.isNoteOn()) // velocity 0 is reported as note-off by JUCE
        return ControlEvent { prefix + "note" + juce::String(m.getNoteNumber()), true };
    if (m.isNoteOff())
        return ControlEvent { prefix + "note" + juce::String(m.getNoteNumber()), false };

    if (m.isController())
    {
        auto id = prefix + "cc" + juce::String(m.getControllerNumber());
        bool down = m.getControllerValue() >= 64;
        std::lock_guard lock(ccLock);
        auto [it, inserted] = ccDown.try_emplace(id, false); // unknown state counts as released
        if (it->second == down)
            return std::nullopt;
        it->second = down;
        return ControlEvent { id, down };
    }
    return std::nullopt;
}

void MidiControl::handleMidi(const juce::MidiMessage& m, const juce::String& device)
{
    if (auto e = translate(m, device))
        post(*e);
}

void MidiControl::handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& m)
{
    handleMidi(m, source->getName());
}

void MidiControl::timerCallback() { refreshDevices(); }

void MidiControl::refreshDevices()
{
    auto available = juce::MidiInput::getAvailableDevices();

    // Drop ports whose device went away, so a re-plugged device is opened afresh.
    for (auto it = inputs.begin(); it != inputs.end();)
    {
        bool present = std::any_of(available.begin(), available.end(),
                                   [&](const juce::MidiDeviceInfo& d) { return d.identifier == it->first; });
        if (present)
        {
            ++it;
            continue;
        }
        it->second->stop();
        it = inputs.erase(it);
    }

    for (auto& d : available)
    {
        if (inputs.count(d.identifier) != 0)
            continue;
        if (auto input = juce::MidiInput::openDevice(d.identifier, this))
        {
            input->start();
            inputs.emplace(d.identifier, std::move(input));
        }
    }
}
} // namespace amp
