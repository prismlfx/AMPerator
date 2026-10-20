#include "control/HidFootswitch.h"

#include <hidapi.h>

#if defined(__APPLE__)
 #include <IOKit/hidsystem/IOHIDLib.h> // IOHIDCheckAccess / IOHIDRequestAccess
 #include <hidapi_darwin.h>
#endif

namespace amp
{
// --- KeyEdgeDetector ---------------------------------------------------------------------------

std::vector<ControlEvent> KeyEdgeDetector::feed(const std::set<uint8_t>& keysDownInReport)
{
    std::vector<ControlEvent> out;
    for (auto k : held)
        if (keysDownInReport.count(k) == 0)
            if (auto it = assigned.find(k); it != assigned.end())
                out.push_back({ it->second, false });

    for (auto k : keysDownInReport)
        if (held.count(k) == 0)
        {
            if (auto it = assigned.find(k); it != assigned.end())
                out.push_back({ it->second, true });
            else
                lastUnassigned = k;
        }

    held = keysDownInReport;
    return out;
}

void KeyEdgeDetector::assign(uint8_t keycode, const juce::String& control) { assigned[keycode] = control; }

void KeyEdgeDetector::clearAssignments() { assigned.clear(); }

std::optional<uint8_t> KeyEdgeDetector::lastUnassignedKey() const { return lastUnassigned; }

// --- Report parsing / device eligibility -------------------------------------------------------

std::optional<std::set<uint8_t>> parseKeyboardReport(const uint8_t* data, size_t length)
{
    constexpr size_t bootLength = 8;
    if (data == nullptr || length < bootLength)
        return std::nullopt;

    // Some PCsensor units prefix the boot report with a report ID (hidapi passes it through).
    if (length == bootLength + 1 && data[0] >= 1 && data[0] <= 0x0f)
        ++data;

    std::set<uint8_t> keys;
    bool rollover = true;
    for (size_t i = 2; i < bootLength; ++i)
    {
        auto k = data[i];
        rollover = rollover && k == 0x01; // ErrorRollOver in every slot: key state unknown
        if (k >= 0x04)                    // 0 = none, 1..3 = error codes
            keys.insert(k);
    }
    if (rollover)
        return std::nullopt;
    return keys;
}

bool isEligibleFootswitch(uint16_t vendorId, uint16_t productId, uint16_t usagePage, uint16_t usage,
                          const juce::String& product)
{
    if (usagePage != 0x01 || usage != 0x06) // Generic Desktop / Keyboard collection only
        return false;

    for (auto [vid, pid] : HidFootswitch::knownDevices())
        if (vid == vendorId && pid == productId)
            return true;

    for (auto* name : { "footswitch", "foot switch", "pcsensor" })
        if (product.containsIgnoreCase(name))
            return true;
    return false;
}

// --- HidFootswitch -----------------------------------------------------------------------------

namespace
{
constexpr int rescanIntervalMs = 2000;
constexpr int readTimeoutMs = 100;

bool isPermissionError(const juce::String& error)
{
    // IOHIDDeviceOpen -> kIOReturnNotPermitted (0xE00002E2) when Input Monitoring is denied.
    return error.containsIgnoreCase("E00002E2") || error.containsIgnoreCase("not permitted");
}

bool inputMonitoringDenied()
{
#if defined(__APPLE__)
    return IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) == kIOHIDAccessTypeDenied;
#else
    return false;
#endif
}

void requestInputMonitoringOnce()
{
#if defined(__APPLE__)
    static std::once_flag once;
    std::call_once(once, [] { IOHIDRequestAccess(kIOHIDRequestTypeListenEvent); }); // shows the system prompt
#endif
}
} // namespace

HidFootswitch::HidFootswitch() : juce::Thread("AMPerator HID footswitch") {}

HidFootswitch::~HidFootswitch() { stop(); }

std::vector<std::pair<uint16_t, uint16_t>> HidFootswitch::knownDevices()
{
    return { { 0x0c45, 0x7403 }, { 0x0c45, 0x7404 }, { 0x3553, 0xb001 }, { 0x1a86, 0xe026 } };
}

void HidFootswitch::start()
{
    if (!isThreadRunning())
        startThread();
}

void HidFootswitch::stop()
{
    stopThread(4 * readTimeoutMs + rescanIntervalMs);
    currentStatus = Status::NotFound;
    {
        std::lock_guard lock(detectorLock);
        detector.feed({}); // forget held keys without emitting
    }
    cancelPending();
}

void HidFootswitch::setKeyAssignments(const std::map<int, juce::String>& keys)
{
    std::lock_guard lock(detectorLock);
    detector.clearAssignments();
    for (auto& [code, control] : keys)
        if (code >= 0 && code <= 0xff)
            detector.assign(static_cast<uint8_t>(code), control);
}

std::optional<uint8_t> HidFootswitch::lastUnassignedKey() const
{
    std::lock_guard lock(detectorLock);
    return detector.lastUnassignedKey();
}

hid_device* HidFootswitch::tryOpen()
{
    bool eligibleSeen = false, permissionError = false;
    hid_device* opened = nullptr;

#if defined(__APPLE__)
    hid_darwin_set_open_exclusive(1); // seize: presses no longer reach other apps
#endif

    auto* list = hid_enumerate(0, 0);
    for (auto* d = list; d != nullptr && opened == nullptr; d = d->next)
    {
        juce::String product = d->product_string != nullptr ? juce::String(d->product_string) : juce::String();
        if (!isEligibleFootswitch(d->vendor_id, d->product_id, d->usage_page, d->usage, product))
            continue;

        eligibleSeen = true;
        opened = hid_open_path(d->path);
        if (opened == nullptr)
        {
            auto* err = hid_error(nullptr);
            permissionError = permissionError || (err != nullptr && isPermissionError(juce::String(err)));
        }
    }
    hid_free_enumeration(list);

    if (opened != nullptr)
    {
        currentStatus = Status::Connected;
        return opened;
    }

    if (eligibleSeen && (permissionError || inputMonitoringDenied()))
    {
        currentStatus = Status::PermissionDenied;
        requestInputMonitoringOnce();
    }
    else
    {
        currentStatus = Status::NotFound;
    }
    return nullptr;
}

void HidFootswitch::handleReport(const uint8_t* data, size_t length)
{
    auto keys = parseKeyboardReport(data, length);
    if (!keys)
        return;

    std::vector<ControlEvent> events;
    {
        std::lock_guard lock(detectorLock);
        events = detector.feed(*keys);
    }
    for (auto& e : events)
        post(e);
}

void HidFootswitch::releaseHeldKeys()
{
    std::vector<ControlEvent> events;
    {
        std::lock_guard lock(detectorLock);
        events = detector.feed({});
    }
    for (auto& e : events)
        post(e);
}

void HidFootswitch::run()
{
    hid_init();
    hid_device* device = nullptr;
    uint8_t buffer[64];

    while (!threadShouldExit())
    {
        if (device == nullptr)
        {
            device = tryOpen();
            if (device == nullptr)
            {
                wait(rescanIntervalMs); // hot-plug rescan; stopThread() wakes us
                continue;
            }
        }

        auto n = hid_read_timeout(device, buffer, sizeof(buffer), readTimeoutMs);
        if (n < 0) // unplugged
        {
            hid_close(device);
            device = nullptr;
            currentStatus = Status::NotFound;
            releaseHeldKeys();
            continue;
        }
        if (n > 0)
            handleReport(buffer, static_cast<size_t>(n));
    }

    if (device != nullptr)
        hid_close(device);
}
} // namespace amp
