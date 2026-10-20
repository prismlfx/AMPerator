#include "control/HidFootswitch.h"

#include <algorithm>

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
        {
            if (suppressed.erase(k) != 0) // its press belonged to an older map and was already released
                continue;
            if (auto it = assigned.find(k); it != assigned.end())
                out.push_back({ it->second, false });
        }

    for (auto k : keysDownInReport)
        if (held.count(k) == 0)
        {
            lastDown = k;
            ++downCount;
            if (auto it = assigned.find(k); it != assigned.end())
                out.push_back({ it->second, true });
            else
                lastUnassigned = k;
        }

    held = keysDownInReport;
    return out;
}

void KeyEdgeDetector::assign(uint8_t keycode, const juce::String& control) { assigned[keycode] = control; }

std::vector<ControlEvent> KeyEdgeDetector::clearAssignments()
{
    std::vector<ControlEvent> released;
    for (auto k : held)
        if (auto it = assigned.find(k); it != assigned.end())
            released.push_back({ it->second, false });
    assigned.clear();
    suppressed.insert(held.begin(), held.end());
    return released;
}

std::optional<uint8_t> KeyEdgeDetector::lastUnassignedKey() const { return lastUnassigned; }
std::optional<uint8_t> KeyEdgeDetector::lastKeyDown() const { return lastDown; }
uint64_t KeyEdgeDetector::keyDownCount() const { return downCount; }

// --- Report parsing / device eligibility -------------------------------------------------------

std::optional<std::set<uint8_t>> parseKeyboardReport(const uint8_t* data, size_t length)
{
    constexpr size_t bootLength = 8;
    if (data == nullptr)
        return std::nullopt;

    // Some PCsensor units prefix the boot report with report ID 1 (hidapi passes it through).
    if (length == bootLength + 1 && data[0] == 0x01)
        ++data;
    else if (length != bootLength)
        return std::nullopt;

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

/** A hint only: IOHIDCheckAccess is the primary signal. kIOReturnNotPermitted = 0xE00002E2. */
bool looksLikePermissionError()
{
    auto* err = hid_error(nullptr);
    if (err == nullptr)
        return false;
    juce::String e(err);
    return e.containsIgnoreCase("E00002E2") || e.containsIgnoreCase("not permitted");
}

hid_device* openPath(const char* path, bool exclusive)
{
#if defined(__APPLE__)
    hid_darwin_set_open_exclusive(exclusive ? 1 : 0); // global hidapi option, only touched on this thread
#else
    juce::ignoreUnused(exclusive);
#endif
    return hid_open_path(path);
}

bool isOpenExclusive(hid_device* device)
{
#if defined(__APPLE__)
    return hid_darwin_is_device_open_exclusive(device) == 1;
#else
    juce::ignoreUnused(device);
    return false;
#endif
}

enum class Access { Granted, Denied, Undecided };

#if defined(__APPLE__)
constexpr bool hasAccessApi = true;
#else
constexpr bool hasAccessApi = false;
#endif

Access inputMonitoringAccess()
{
#if defined(__APPLE__)
    auto access = IOHIDCheckAccess(kIOHIDRequestTypeListenEvent);
    if (access == kIOHIDAccessTypeGranted)
        return Access::Granted;
    return access == kIOHIDAccessTypeDenied ? Access::Denied : Access::Undecided;
#else
    return Access::Granted;
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
    exclusiveOpen = false;
    {
        std::lock_guard lock(detectorLock);
        detector.feed({}); // forget held keys without emitting
    }
    cancelPending();
}

void HidFootswitch::setKeyAssignments(const std::map<int, juce::String>& keys)
{
    std::vector<ControlEvent> released;
    {
        std::lock_guard lock(detectorLock);
        released = detector.clearAssignments(); // held pedals are released under the old map first
        for (auto& [code, control] : keys)
            if (code >= 0 && code <= 0xff)
                detector.assign(static_cast<uint8_t>(code), control);
    }
    for (auto& e : released)
        post(e);
}

std::optional<uint8_t> HidFootswitch::lastUnassignedKey() const
{
    std::lock_guard lock(detectorLock);
    return detector.lastUnassignedKey();
}

hid_device* HidFootswitch::tryOpen()
{
    bool eligibleSeen = false, permissionHint = false;
    hid_device* opened = nullptr;

    auto* list = hid_enumerate(0, 0);
    for (auto* d = list; d != nullptr && opened == nullptr; d = d->next)
    {
        juce::String product = d->product_string != nullptr ? juce::String(d->product_string) : juce::String();
        if (!isEligibleFootswitch(d->vendor_id, d->product_id, d->usage_page, d->usage, product))
            continue;
        eligibleSeen = true;

        if ((opened = openPath(d->path, true)) != nullptr) // seize: presses no longer reach other apps
            break;
        permissionHint = permissionHint || looksLikePermissionError();

        auto access = inputMonitoringAccess();
        if (access != Access::Granted)
        {
            if (access == Access::Undecided)
                requestInputMonitoringOnce();
            hid_free_enumeration(list);
            currentStatus = Status::PermissionDenied;
            return nullptr;
        }

        // Access is granted, yet seizing failed (not privileged, or another app holds it): share instead.
        if ((opened = openPath(d->path, false)) == nullptr)
            permissionHint = permissionHint || looksLikePermissionError();
    }
    hid_free_enumeration(list);
#if defined(__APPLE__)
    hid_darwin_set_open_exclusive(1); // restore hidapi's default
#endif

    if (opened != nullptr)
    {
        exclusiveOpen = isOpenExclusive(opened);
        currentStatus = Status::Connected;
        return opened;
    }

    // On macOS IOHIDCheckAccess already said "granted" by now, so a failure is Busy; the error text is only a
    // hint where no access API exists.
    exclusiveOpen = false;
    currentStatus = !eligibleSeen ? Status::NotFound
                  : (permissionHint && !hasAccessApi) ? Status::PermissionDenied
                                                       : Status::Busy;
    return nullptr;
}

std::vector<ControlEvent> HidFootswitch::translate(const std::set<uint8_t>& keysDown)
{
    std::vector<ControlEvent> events;
    std::optional<uint8_t> newKey;
    {
        std::lock_guard lock(detectorLock);
        const auto before = detector.keyDownCount();
        events = detector.feed(keysDown);
        if (detector.keyDownCount() != before)
            newKey = detector.lastKeyDown();
    }
    if (newKey)
    {
        if (capturing.exchange(false))
            events.erase(std::remove_if(events.begin(), events.end(), [](const ControlEvent& e) { return e.pressed; }),
                         events.end());
        events.insert(events.begin(), { "hidkey:" + juce::String(static_cast<int>(*newKey)), true });
    }
    return events;
}

void HidFootswitch::handleReport(const uint8_t* data, size_t length)
{
    auto keys = parseKeyboardReport(data, length);
    if (!keys)
        return;
    for (auto& e : translate(*keys))
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
            exclusiveOpen = false;
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
