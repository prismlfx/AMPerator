#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "control/ControlEvent.h"

struct hid_device_; // hidapi

namespace amp
{
/** Turns successive "keys currently down" snapshots (HID boot-keyboard reports) into press/release edges. Pure logic. */
class KeyEdgeDetector
{
public:
    /** Returns releases first, then presses, each in keycode order. Unassigned keys emit nothing. */
    std::vector<ControlEvent> feed(const std::set<uint8_t>& keysDownInReport);
    void assign(uint8_t keycode, const juce::String& control); // e.g. 0x05 -> "footswitch:L"
    void clearAssignments();
    /** The most recent unassigned key that went down, for setup learning. */
    std::optional<uint8_t> lastUnassignedKey() const;

private:
    std::map<uint8_t, juce::String> assigned;
    std::set<uint8_t> held;
    std::optional<uint8_t> lastUnassigned;
};

/** Keys down in an input report: 8-byte boot keyboard report (byte 0 modifiers, byte 1 reserved, bytes 2..7 keycodes),
    or the same prefixed by a small report ID (9 bytes). Modifiers are ignored. nullopt = not a usable report
    (too short, or the "phantom" rollover-error state). */
std::optional<std::set<uint8_t>> parseKeyboardReport(const uint8_t* data, size_t length);

/** True only for the keyboard collection (usage page 0x01, usage 0x06) of a device on the known list or whose product
    string names a footswitch ("FootSwitch", "Foot Switch", "PCsensor", any case). Never matches an ordinary keyboard.
    (A user-editable allow-list in settings would be the way to add other models; out of scope for v1.) */
bool isEligibleFootswitch(uint16_t vendorId, uint16_t productId, uint16_t usagePage, uint16_t usage,
                          const juce::String& product);

/** Reads a USB footswitch (a HID keyboard) directly via hidapi, opened exclusively on macOS so presses don't type into
    other apps and keep working in the background. Reading happens on a background thread; it rescans every 2 s while no
    device is open (hot-plug). Events arrive on the message thread. */
class HidFootswitch : public ControlSource, private juce::Thread
{
public:
    enum class Status { Connected, NotFound, PermissionDenied };

    HidFootswitch();
    ~HidFootswitch() override;

    void start();
    void stop(); // message thread; no event is delivered after it returns
    Status status() const { return currentStatus.load(); }

    /** Keycode -> control ID, typically Settings::footswitchKeys. Callable at any time. */
    void setKeyAssignments(const std::map<int, juce::String>& keys);
    std::optional<uint8_t> lastUnassignedKey() const;

    /** Best-effort VID/PID list (unverified guesses); discovery also matches product strings, see isEligibleFootswitch. */
    static std::vector<std::pair<uint16_t, uint16_t>> knownDevices();

private:
    void run() override;
    hid_device_* tryOpen(); // nullptr with currentStatus updated when nothing could be opened
    void handleReport(const uint8_t* data, size_t length);
    void releaseHeldKeys();

    std::atomic<Status> currentStatus { Status::NotFound };
    mutable std::mutex detectorLock;
    KeyEdgeDetector detector;
};
} // namespace amp
