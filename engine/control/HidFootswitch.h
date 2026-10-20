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
    /** Drops every assignment. Held assigned keys are released under the old map (returned); they stay held, so they
        produce nothing more until physically released and pressed again. */
    std::vector<ControlEvent> clearAssignments();
    /** The most recent unassigned key that went down, for setup learning. */
    std::optional<uint8_t> lastUnassignedKey() const;
    /** The most recent key that went down, assigned or not. */
    std::optional<uint8_t> lastKeyDown() const;
    /** Increments on every key-down edge. */
    uint64_t keyDownCount() const;

private:
    uint64_t downCount = 0;
    std::optional<uint8_t> lastDown;
    std::map<uint8_t, juce::String> assigned;
    std::set<uint8_t> held;
    std::set<uint8_t> suppressed; // held across a reassignment: ignored until released
    std::optional<uint8_t> lastUnassigned;
};

/** Keys down in an input report: 8-byte boot keyboard report (byte 0 modifiers, byte 1 reserved, bytes 2..7 keycodes),
    or the same prefixed by report ID 1 (9 bytes). Modifiers are ignored. nullopt = not a usable report
    (any other length or report ID, or the "phantom" rollover-error state). */
std::optional<std::set<uint8_t>> parseKeyboardReport(const uint8_t* data, size_t length);

using UsbIds = std::pair<uint16_t, uint16_t>; // vendor id, product id

/** True only for the keyboard collection (usage page 0x01, usage 0x06) of a device on the known list, whose product
    string names a footswitch ("FootSwitch", "Foot Switch", "PCsensor", any case), or that the user picked
    (`userDevice`, Settings::footswitchDevice). Never matches an ordinary keyboard the user did not pick. */
bool isEligibleFootswitch(uint16_t vendorId, uint16_t productId, uint16_t usagePage, uint16_t usage,
                          const juce::String& product, const std::optional<UsbIds>& userDevice = std::nullopt);

/** One HID collection as hidapi enumerates it. */
struct HidKeyboardInfo
{
    uint16_t vendorId = 0, productId = 0;
    juce::String manufacturer, product;
    uint16_t usagePage = 0, usage = 0;
};

/** The keyboard collections (usage page 0x01, usage 0x06) in `all`, one entry per vendor/product id, in order. */
std::vector<HidKeyboardInfo> keyboardsOnly(const std::vector<HidKeyboardInfo>& all);

/** Reads a USB footswitch (a HID keyboard) directly via hidapi, so presses keep working while the app is in the
    background. Reading happens on a background thread; it rescans every 2 s while no device is open (hot-plug).
    Events arrive on the message thread.

    Open strategy (macOS): try an exclusive (seizing) open first, so presses don't type into other apps. If that fails,
    Input Monitoring access (IOHIDCheckAccess) decides: denied -> PermissionDenied; not yet decided -> the system prompt
    is requested and the status is PermissionDenied until a later rescan succeeds. With access granted the same device is
    opened shared instead (exclusive() == false). In shared mode every press ALSO types into the front app; pedals
    reprogrammed to F13/F14/F15 (in the default key map) avoid visible typing. */
class HidFootswitch : public ControlSource, private juce::Thread
{
public:
    enum class Status
    {
        Connected,        // opened, exclusive or shared (see exclusive())
        NotFound,         // no eligible device present
        PermissionDenied, // Input Monitoring denied or not yet granted
        Busy              // eligible device present but could not be opened at all (e.g. held by another app)
    };

    HidFootswitch();
    ~HidFootswitch() override;

    void start();
    void stop(); // message thread; no event is delivered after it returns
    Status status() const { return currentStatus.load(); }
    /** True when the open device is seized (presses don't reach other apps). False when shared or not connected. */
    bool exclusive() const { return exclusiveOpen.load(); }

    /** A keyboard the user picked as the footswitch (Settings::footswitchDevice), eligible in addition to the automatic
        rules; nullopt = automatic rules only. Callable at any time; used by the next scan for a device. */
    void setUserDevice(std::optional<UsbIds>);
    /** Every connected HID collection (enumerates on the calling thread; serialised with the reader's scans). */
    static std::vector<HidKeyboardInfo> enumerateDevices();

    /** Keycode -> control ID, typically Settings::footswitchKeys. Callable at any time. */
    void setKeyAssignments(const std::map<int, juce::String>& keys);
    std::optional<uint8_t> lastUnassignedKey() const;
    /** While capturing, the next key-down is swallowed (no control press is posted for it) and capturing turns off.
        Every key-down also posts a ControlEvent { "hidkey:<keycode>", true } so the owner learns of the raw key. */
    void setCapturing(bool on) { capturing.store(on); }

    /** Turns the keys currently down into the events to post: a "hidkey:<code>" press first for a new key-down, then
        the control edges (the control press of that key is dropped when capturing). Public for tests. */
    std::vector<ControlEvent> translate(const std::set<uint8_t>& keysDown);

    /** Best-effort VID/PID list (unverified guesses); discovery also matches product strings, see isEligibleFootswitch. */
    static std::vector<std::pair<uint16_t, uint16_t>> knownDevices();

private:
    void run() override;
    hid_device_* tryOpen(); // nullptr with currentStatus updated when nothing could be opened
    void handleReport(const uint8_t* data, size_t length);
    void releaseHeldKeys();

    std::atomic<Status> currentStatus { Status::NotFound };
    std::atomic<bool> exclusiveOpen { false };
    std::atomic<bool> capturing { false };
    mutable std::mutex userDeviceLock;
    std::optional<UsbIds> userDevice;
    mutable std::mutex detectorLock;
    KeyEdgeDetector detector;
};
} // namespace amp
