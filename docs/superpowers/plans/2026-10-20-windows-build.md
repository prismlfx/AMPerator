# AMPerator Windows Build Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build, test and package AMPerator for Windows x64 (ASIO + WASAPI audio, WebView2 UI, Raw Input footswitch) with GitHub Actions as the first proof and the macOS build kept green in the same workflow.

**Architecture:** The engine, app and UI stay single-source; Windows differences live in CMake (MSVC flags, JUCE options, WebView2 package fetch), one new platform file for the footswitch reader (`HidFootswitchRawInput.cpp`, behind the unchanged `HidFootswitch` interface), a WebView2 runtime check in the app, and small additive bridge/UI changes for choosing the audio driver. Every piece of logic that can be tested without Windows (reserved file names, scan-code tables, device-type preference) is pure code with Catch2/vitest tests that run on the Mac; the Windows CI job proves the rest.

**Tech Stack:** C++20, CMake ≥ 3.24 (Visual Studio generator chosen by CMake, `-A x64`), MSVC (VS 2022 or 2026), JUCE 8.0.15 with `JUCE_ASIO` (bundled Steinberg headers, GPLv3 option) and `JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING`, Microsoft.Web.WebView2 1.0.3485.44 (NuGet archive via FetchContent), hidapi 0.15.0 (`hidapi_winapi`, enumeration only on Windows), Win32 Raw Input, Catch2 v3.8.1, Vite/TypeScript/vitest, GitHub Actions (`macos-15`, `windows-latest`).

**Spec:** `docs/superpowers/specs/2026-10-20-windows-build-design.md`

## Global Constraints

- Developer machine is a Mac: every task's verification names what runs locally and what is proven only by the `windows` CI job. Push after each task and read the CI result before the next task (`gh run watch` or the Actions tab).
- Commit messages end with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.
- No change to the rig file format, bridge protocol version (stays 1) or DSP. Bridge additions are additive and documented in `docs/bridge-protocol.md`.
- Public interface of `amp::HidFootswitch` and the free functions in `control/HidFootswitch.h` are unchanged (tests, `Engine`, UI depend on them).
- macOS behaviour and Settings view are unchanged: new UI elements render only when `availableDeviceTypes.length > 1` / `hasControlPanel`.
- `AMP_ASIO` CMake option (Windows only) default ON; `OFF` must build and run a WASAPI-only app.
- Static CRT on MSVC (`CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"`), `/utf-8`, `/RTC1` removed from Debug flags, `/bigobj` on `nam_core` and `amp_engine`, `nam_core` `/O2` in Debug.
- WebView2 package: URL `https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.3485.44`, SHA-256 `bc09150b179246ac90189649b13be8e6b11b3ac200e817e18df106e1f3cf489e`, extracted to `${CMAKE_BINARY_DIR}/_deps/Microsoft.Web.WebView2.1.0.3485.44`, `JUCE_WEBVIEW2_PACKAGE_LOCATION=${CMAKE_BINARY_DIR}/_deps`.
- WebView2 user-data folder: `%LOCALAPPDATA%\AMPerator\WebView2` (`juce::File::windowsLocalAppData`); background colour `0xff121316`.
- Device-type preference on Windows, in order: `ASIO`, `Windows Audio (Low Latency Mode)`, `Windows Audio (Exclusive Mode)`, `Windows Audio`; empty list on other platforms. `JUCE_DIRECTSOUND=0`.
- Reserved base names (case-insensitive, part before the first `.`): `CON PRN AUX NUL COM0..COM9 LPT0..LPT9` → append `_` to that part. Applied on all platforms.
- Zip: `AMPerator-${PROJECT_VERSION}-windows-x64.zip` with `AMPerator.exe`, `LICENSE`, `THIRD_PARTY.md`, `README-windows.txt`. x64 only. No installer, no signing.
- Out of scope: ARM64, Linux, exe icon, exclusive footswitch grab on Windows, Windows MIDI Services.

## Review Focus

1. **A rig or band named `con `, `Con.Tone`, `com1` (any case, with extension or trailing space)** must save and reload on both platforms; the display name stays exact. → Task 3 tests `reserved Windows names get an underscore` and `save with reserved band and rig names`.
2. **A held pedal on Windows** (Raw Input repeats the make code every ~30 ms) must toggle once, not stream. → Task 4 test `RawKeyState: repeated make is not a new press`.
3. **An ordinary keyboard plugged in next to the pedal** must never toggle anything, and the pedal must keep working after the keyboard is unplugged. → Task 4 test `eligibleFootswitchIds ignores keyboards that are not footswitches`, Task 4 test `RawKeyState: removing a device releases only its keys`.
4. **A `settings.json` copied from the Mac** (`audioDeviceType: "CoreAudio"`, `audioDeviceName: "Scarlett Solo USB"`) must open some device on Windows without crashing and let Settings switch drivers. → Task 6 test `unknown saved type falls back to the preferred type`.
5. **The saved ASIO interface is unplugged at launch** → WASAPI's default device must play, and the interface must take over when plugged back. → Task 6 test `missing preferred device opens the first preferred type with devices`, plus the existing reconnect tests.

---

## File Structure

```
CMakeLists.txt                              MSVC flags, static CRT, AMP_ASIO option            (Task 1)
cmake/Dependencies.cmake                    + WebView2 archive (WIN32)                          (Task 7)
cmake/AmpJuce.cmake                         per-platform JUCE options, WebView2 define/link     (Tasks 1, 7)
engine/CMakeLists.txt                       MSVC /bigobj, nam_core Debug /O2, new sources       (Tasks 1, 4, 5)
engine/storage/RigStore.cpp                 reserved names in sanitizeFileName                  (Task 3)
engine/control/RawInputKeys.{h,cpp}         NEW pure tables: scan code/VK -> HID usage, device path ids, RawKeyState, eligibleFootswitchIds (Task 4)
engine/control/HidFootswitch.h              Reader pimpl instead of juce::Thread                 (Task 5)
engine/control/HidFootswitch.cpp            common logic only                                   (Task 5)
engine/control/HidFootswitchHidapi.cpp      NEW (non-Windows): today's reader thread, moved     (Task 5)
engine/control/HidFootswitchRawInput.cpp    NEW (Windows): Raw Input reader                      (Task 5)
engine/audio/AudioIO.{h,cpp}                preferred types, deviceTypes(), control panel        (Task 6)
engine/Engine.{h,cpp}, engine/bridge/*.cpp  openAudioControlPanel, getSettings fields, mic hint  (Tasks 6, 7)
ui/src/state.ts, ui/src/views/settings.ts   Driver select, Control panel… button                 (Task 6)
app/CMakeLists.txt                          npm.cmd, PATH wrapper non-Windows, NEEDS_WEBVIEW2, amp_package (Tasks 7, 8)
app/MainWindow.cpp                          WebView2 backend options                             (Task 7)
app/WebView2Runtime.{h,cpp}                 NEW runtime version check                            (Task 7)
app/Main.cpp                                WebView2 missing dialog                              (Task 7)
app/MicrophonePermission.{h,cpp}            comment only                                         (Task 7)
app/README-windows.txt                      NEW zip readme                                       (Task 8)
.github/workflows/ci.yml                    NEW macOS + Windows jobs                             (Tasks 2, 7, 8, 9)
tests/CMakeLists.txt, tests/*_test.cpp      new tests                                            (Tasks 3, 4, 6)
tests/support/FakeAudioDevice.{h,cpp}       FakeType(typeName)                                   (Task 6)
docs/bridge-protocol.md, README.md, THIRD_PARTY.md, docs/manual-test-checklist.md               (Tasks 1, 6, 7, 8)
```

---

### Task 1: MSVC and per-platform CMake groundwork

**Files:**
- Modify: `CMakeLists.txt` (after `project()`, before `include(cmake/Dependencies.cmake)`), `cmake/AmpJuce.cmake`, `engine/CMakeLists.txt`, `THIRD_PARTY.md`

**Interfaces:**
- Produces: CMake cache option `AMP_ASIO` (BOOL, WIN32 only, default ON); compile definitions on `amp_juce` (re-exported): Windows `JUCE_ASIO=$<BOOL:${AMP_ASIO}> JUCE_WASAPI=1 JUCE_DIRECTSOUND=0`; Apple `JUCE_PLUGINHOST_AU=1` (moved out of the common list). Nothing else changes for consumers.

- [ ] **Step 1: Root `CMakeLists.txt`** — add the `if(MSVC)` block from spec §4.1 verbatim (static CRT via `CMAKE_MSVC_RUNTIME_LIBRARY`, `add_compile_options(/utf-8)`, `string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG …)`) and `if(WIN32) option(AMP_ASIO "…" ON) endif()`. Comment each line with its reason (the spec's).
- [ ] **Step 2: `cmake/AmpJuce.cmake`** — remove `JUCE_PLUGINHOST_AU=1` from the common `target_compile_definitions`; add `if(APPLE)` → `JUCE_PLUGINHOST_AU=1`; `if(WIN32)` → `JUCE_ASIO=$<BOOL:${AMP_ASIO}> JUCE_WASAPI=1 JUCE_DIRECTSOUND=0`. (WebView2 comes in Task 7.)
- [ ] **Step 3: `engine/CMakeLists.txt`** — `if(MSVC) target_compile_options(nam_core PRIVATE /bigobj $<$<CONFIG:Debug>:/O2>) target_compile_options(amp_engine PRIVATE /bigobj) endif()`, with a comment that D9025 ("overriding /Od with /O2") is expected and mirrors the clang `-O2`.
- [ ] **Step 4: `THIRD_PARTY.md`** — add a row: ASIO SDK headers (`asio.h`, `asiosys.h`, `iasiodrv.h`, bundled in JUCE 8.0.15 under `modules/juce_audio_devices/native/asio/`) | Steinberg ASIO License **or GPLv3** (used under GPLv3) | ASIO device support on Windows (`AMP_ASIO`).
- [ ] **Step 5: Verify on the Mac that nothing changed** — Run: `cmake -B build -G Xcode && cmake --build build --target amp_tests AMPerator --config Debug && ctest --test-dir build -C Debug`. Expected: configure ok, 257 tests pass, `grep JUCE_PLUGINHOST_AU build/compile_commands.json` or the Xcode build log still shows `JUCE_PLUGINHOST_AU=1` on amp_juce.
- [ ] **Step 6: Commit** — `build: MSVC groundwork (static CRT, /utf-8, nam_core Debug /O2) and per-platform JUCE options with an AMP_ASIO switch`

---

### Task 2: CI workflow and the MSVC compile sweep

**Files:**
- Create: `.github/workflows/ci.yml`
- Modify: whatever `engine/`, `tests/` sources MSVC rejects (fixes stay behaviour-neutral; no `#if _MSC_VER` unless unavoidable)

**Interfaces:**
- Produces: workflow `CI` with jobs `macos` and `windows` (spec §4.6). In this task the Windows job configures with `-DAMP_BUILD_APP=OFF` and builds/tests only `amp_tests` in Debug; Task 7 turns the app on.

- [ ] **Step 1: Write `.github/workflows/ci.yml`** — `on: push: branches: [master]; pull_request; workflow_dispatch`; `concurrency: { group: ci-${{ github.ref }}, cancel-in-progress: true }`. Job `macos` (`runs-on: macos-15`, `timeout-minutes: 60`): `actions/checkout@v4`, `actions/setup-node@v4` with `node-version: 22`, `cmake -B build -G Xcode`, `cmake --build build --config Debug --target amp_tests AMPerator`, `ctest --test-dir build -C Debug --output-on-failure`, `npm ci && npx vitest run` in `ui`. Job `windows` (`runs-on: windows-latest`, `timeout-minutes: 90`): checkout, setup-node 22, `cmake -B build -A x64 -DAMP_BUILD_APP=OFF`, `cmake --build build --config Debug --target amp_tests`, `ctest --test-dir build -C Debug --output-on-failure`.
- [ ] **Step 2: Push and watch** — Run: `git push && gh run watch` (or open Actions). Expected first time: `macos` green; `windows` likely fails at compile. Record every error in the task notes.
- [ ] **Step 3: Fix MSVC errors in order of appearance**, one commit per file group, re-pushing until the Windows configure+build gets to ctest. Known candidates: implicit narrowing in `static_cast`-free arithmetic (`int`↔`size_t`, warnings only unless in a constant expression), `juce::String("…").containsChar(c)` with `juce_wchar` (fine), `std::erase_if` (needs `<map>`/`<vector>`, present), missing `<cstdint>`/`<algorithm>` includes that libc++ pulled in transitively, `M_PI` (needs `_USE_MATH_DEFINES` or `juce::MathConstants<double>::pi`), `ssize_t`, POSIX `unistd.h`. Do not silence warnings globally.
- [ ] **Step 4: Read the ctest result on Windows** — Expected: all tests pass, including `realtime_engine: RtSafety counts allocations inside ScopedNoAlloc` and `realtime_engine: alloc guard covers the audio callback`. If the link step reports `LNK2005` on `operator new`/`operator delete` or the RtSafety tests fail, apply the fallback below before continuing; otherwise skip it.
- [ ] **Step 4b (only if Step 4 showed the RtSafety problem): `_CrtSetAllocHook` fallback** — in `engine/engine/RtSafety.cpp`, under `#if defined(_MSC_VER)` inside the `!NDEBUG` branch, replace the operator definitions with: a hook `int __cdecl allocHook(int allocType, void*, size_t, int, long, const unsigned char*, int) noexcept` that calls `noteAllocation()` for `_HOOK_ALLOC`, `_HOOK_REALLOC`, `_HOOK_FREE` and returns `TRUE`; install it once from a constant-initialised static (`struct Installer { Installer() { _CrtSetAllocHook(allocHook); } } installer;`). Keep `ScopedNoAlloc`, `violations()`, `reset()`, `active()` unchanged. Note in `RtSafety.h` that on MSVC the debug CRT hook counts every `malloc`/`new`/`free` on a guarded thread.
- [ ] **Step 5: Verify** — Both jobs green on the Actions tab for the final commit of this task. Run the Mac suite once more locally: `ctest --test-dir build -C Debug` → all pass.
- [ ] **Step 6: Commit (the last fix)** — `ci: GitHub Actions workflow building and testing on macOS and Windows (engine and tests)`; fix commits named `fix(msvc): …`.

---

### Task 3: Reserved Windows file names in `RigStore::sanitizeFileName`

**Files:**
- Modify: `engine/storage/RigStore.cpp` (`sanitizeFileName`, lines ~85–96), `tests/rig_store_test.cpp`

**Interfaces:**
- Consumes: `static juce::String RigStore::sanitizeFileName(const juce::String&)` (unchanged signature).
- Produces: the reserved-name rule from Global Constraints; a file-local helper `bool isReservedBaseName(const juce::String& base)` in the anonymous namespace of `RigStore.cpp`.

- [ ] **Step 1: Write the failing tests** in `tests/rig_store_test.cpp`:
```cpp
TEST_CASE("rig_store: reserved Windows names get an underscore")
{
    using amp::RigStore;
    CHECK(RigStore::sanitizeFileName("CON") == "CON_");
    CHECK(RigStore::sanitizeFileName("nul") == "nul_");
    CHECK(RigStore::sanitizeFileName("Com1.tone") == "Com1_.tone");
    CHECK(RigStore::sanitizeFileName("con ") == "con_");        // trimmed first, then checked
    CHECK(RigStore::sanitizeFileName("LPT9") == "LPT9_");
    CHECK(RigStore::sanitizeFileName("CONCERT") == "CONCERT");
    CHECK(RigStore::sanitizeFileName("LPT10") == "LPT10");
    CHECK(RigStore::sanitizeFileName("aux.") == "aux_");        // trailing dot already trimmed
}

TEST_CASE("rig_store: save with reserved band and rig names")
{
    TempDir t;
    amp::RigStore store(t.dir);
    auto r = rig("AUX"); r.band = "COM1";
    REQUIRE(store.save(r).wasOk());
    REQUIRE(t.dir.getChildFile("Rigs/COM1_/AUX_.amprig").existsAsFile());
    auto loaded = store.load(r.id);
    REQUIRE(loaded.rig);
    CHECK(loaded.rig->name == "AUX");
    CHECK(loaded.rig->band == "COM1");
}
```
- [ ] **Step 2: Run** — `cmake --build build --target amp_tests --config Debug && ctest --test-dir build -C Debug -R "reserved"` → FAIL (`"CON" == "CON_"` mismatch).
- [ ] **Step 3: Implement** — `isReservedBaseName(base)`: `base.equalsIgnoreCase` against `CON PRN AUX NUL`, or `COM`/`LPT` followed by exactly one digit `0–9`. In `sanitizeFileName`, after the trimming and the 120-char cut and before the `Untitled` fallback: split at the first `.`; if the part before it is reserved, insert `_` after it.
- [ ] **Step 4: Run** — `ctest --test-dir build -C Debug -R "rig_store"` → all pass (including the existing `sanitizeFileName rules`).
- [ ] **Step 5: Commit** — `fix(storage): keep rig and band file names clear of Windows reserved device names`

---

### Task 4: Pure Raw Input helpers (`RawInputKeys`)

**Files:**
- Create: `engine/control/RawInputKeys.h`, `engine/control/RawInputKeys.cpp`, `tests/raw_input_keys_test.cpp`
- Modify: `engine/CMakeLists.txt` (add the two sources), `tests/CMakeLists.txt` (add the test file)

**Interfaces:**
- Consumes: `amp::UsbIds`, `amp::HidKeyboardInfo`, `amp::isEligibleFootswitch(...)` from `control/HidFootswitch.h`.
- Produces (`namespace amp::RawInputKeys`, header includes `<cstdint> <optional> <set> <vector>` and `control/HidFootswitch.h`):
```cpp
std::optional<uint8_t> hidUsageFromScanCode(uint16_t makeCode, bool e0);   // PS/2 scan code set 1 -> HID keyboard usage; nullopt for modifiers/unknown
std::optional<uint8_t> hidUsageFromVirtualKey(uint16_t vk);                 // Win32 VK_* -> HID usage; letters are layout-dependent (fallback only)
std::optional<UsbIds>  usbIdsFromDevicePath(const juce::String& path);      // "...VID_0C45&PID_7403..." any case -> {0x0c45, 0x7403}
std::set<UsbIds> eligibleFootswitchIds(const std::vector<HidKeyboardInfo>& all, const std::optional<UsbIds>& userDevice);
class RawKeyState                                                           // keys held, per device handle; returns the union after each change
{
public:
    using DeviceId = std::uintptr_t;
    std::set<uint8_t> apply(DeviceId device, uint8_t usage, bool pressed);
    std::set<uint8_t> removeDevice(DeviceId device);
    std::set<uint8_t> keysDown() const;
};
```

- [ ] **Step 1: Write the failing tests** in `tests/raw_input_keys_test.cpp` (`using namespace amp::RawInputKeys;`):
```cpp
TEST_CASE("raw_input: scan codes map to HID usages")
{
    CHECK(hidUsageFromScanCode(0x1E, false) == 0x04); // a
    CHECK(hidUsageFromScanCode(0x30, false) == 0x05); // b
    CHECK(hidUsageFromScanCode(0x2E, false) == 0x06); // c
    CHECK(hidUsageFromScanCode(0x64, false) == 0x68); // F13
    CHECK(hidUsageFromScanCode(0x65, false) == 0x69); // F14
    CHECK(hidUsageFromScanCode(0x66, false) == 0x6A); // F15
    CHECK(hidUsageFromScanCode(0x76, false) == 0x73); // F24
    CHECK(hidUsageFromScanCode(0x3B, false) == 0x3A); // F1
    CHECK(hidUsageFromScanCode(0x57, false) == 0x44); // F11
    CHECK(hidUsageFromScanCode(0x1C, false) == 0x28); // Enter
    CHECK(hidUsageFromScanCode(0x1C, true)  == 0x58); // keypad Enter
    CHECK(hidUsageFromScanCode(0x48, true)  == 0x52); // Up arrow
    CHECK(hidUsageFromScanCode(0x48, false) == 0x60); // keypad 8
    CHECK_FALSE(hidUsageFromScanCode(0x2A, false).has_value()); // Left Shift: modifiers are ignored, as in parseKeyboardReport
    CHECK_FALSE(hidUsageFromScanCode(0x00, false).has_value());
    CHECK_FALSE(hidUsageFromScanCode(0x7F, false).has_value());
}

TEST_CASE("raw_input: virtual keys map to HID usages")
{
    CHECK(hidUsageFromVirtualKey(0x41) == 0x04); // VK_A
    CHECK(hidUsageFromVirtualKey(0x30) == 0x27); // VK_0
    CHECK(hidUsageFromVirtualKey(0x31) == 0x1E); // VK_1
    CHECK(hidUsageFromVirtualKey(0x70) == 0x3A); // VK_F1
    CHECK(hidUsageFromVirtualKey(0x7C) == 0x68); // VK_F13
    CHECK(hidUsageFromVirtualKey(0x87) == 0x73); // VK_F24
    CHECK(hidUsageFromVirtualKey(0x0D) == 0x28); // VK_RETURN
    CHECK_FALSE(hidUsageFromVirtualKey(0x10).has_value()); // VK_SHIFT
}

TEST_CASE("raw_input: vendor and product ids come from the device path")
{
    auto ids = usbIdsFromDevicePath(R"(\\?\HID#VID_0C45&PID_7403&MI_00#7&2a5e1b3&0&0000#{884b96c3-56ef-11d1-bc8c-00a0c91405dd})");
    REQUIRE(ids);
    CHECK(ids->first == 0x0c45);
    CHECK(ids->second == 0x7403);
    CHECK(usbIdsFromDevicePath(R"(\\?\hid#vid_3553&pid_b001#...)")->second == 0xb001);
    CHECK_FALSE(usbIdsFromDevicePath(R"(\\?\ACPI#PNP0303#4&1b7a3d6&0#{884b96c3-...})").has_value()); // a PS/2 keyboard
    CHECK_FALSE(usbIdsFromDevicePath("").has_value());
}

TEST_CASE("raw_input: eligibleFootswitchIds ignores keyboards that are not footswitches")
{
    std::vector<amp::HidKeyboardInfo> all = {
        { 0x05ac, 0x0342, "Apple", "Internal Keyboard", 0x01, 0x06 },
        { 0x0c45, 0x7403, "PCsensor", "FootSwitch", 0x01, 0x06 },
        { 0x0c45, 0x7403, "PCsensor", "FootSwitch", 0x01, 0x02 }, // mouse collection of the same device
        { 0x1234, 0x0001, "Acme", "Keyboard", 0x01, 0x06 } };
    auto ids = eligibleFootswitchIds(all, std::nullopt);
    CHECK(ids == std::set<amp::UsbIds> { { 0x0c45, 0x7403 } });
    CHECK(eligibleFootswitchIds(all, amp::UsbIds { 0x1234, 0x0001 }) == std::set<amp::UsbIds> { { 0x0c45, 0x7403 }, { 0x1234, 0x0001 } });
}

TEST_CASE("raw_input: RawKeyState: repeated make is not a new press")
{
    RawKeyState s;
    CHECK(s.apply(1, 0x68, true) == std::set<uint8_t> { 0x68 });
    CHECK(s.apply(1, 0x68, true) == std::set<uint8_t> { 0x68 }); // auto-repeat: same set, so KeyEdgeDetector sees no edge
    CHECK(s.apply(1, 0x68, false).empty());
}

TEST_CASE("raw_input: RawKeyState: removing a device releases only its keys")
{
    RawKeyState s;
    s.apply(1, 0x68, true);
    s.apply(2, 0x04, true);
    CHECK(s.keysDown() == std::set<uint8_t> { 0x04, 0x68 });
    CHECK(s.removeDevice(1) == std::set<uint8_t> { 0x04 });
    CHECK(s.removeDevice(1) == std::set<uint8_t> { 0x04 }); // unknown device: no change
}
```
- [ ] **Step 2: Add the files to CMake and run** — `cmake --build build --target amp_tests --config Debug` → link errors (undefined `hidUsageFromScanCode` …).
- [ ] **Step 3: Implement `RawInputKeys.cpp`** with `constexpr` tables:
  - Scan code set 1 (`e0 == false`): `0x01→0x29` Esc; `0x02..0x0B→0x1E..0x27` (1–9, 0); `0x0C→0x2D 0x0D→0x2E 0x0E→0x2A 0x0F→0x2B`; `0x10..0x19` = q w e r t y u i o p → `0x14 0x1A 0x08 0x15 0x17 0x1C 0x18 0x0C 0x12 0x13`; `0x1A→0x2F 0x1B→0x30 0x1C→0x28`; `0x1E..0x26` = a s d f g h j k l → `0x04 0x16 0x07 0x09 0x0A 0x0B 0x0D 0x0E 0x0F`; `0x27→0x33 0x28→0x34 0x29→0x35 0x2B→0x31`; `0x2C..0x32` = z x c v b n m → `0x1D 0x1B 0x06 0x19 0x05 0x11 0x10`; `0x33→0x36 0x34→0x37 0x35→0x38 0x37→0x55 0x39→0x2C 0x3A→0x39`; `0x3B..0x44→0x3A..0x43` (F1–F10); `0x45→0x53 0x46→0x47`; keypad `0x47→0x5F 0x48→0x60 0x49→0x61 0x4A→0x56 0x4B→0x5C 0x4C→0x5D 0x4D→0x5E 0x4E→0x57 0x4F→0x59 0x50→0x5A 0x51→0x5B 0x52→0x62 0x53→0x63`; `0x57→0x44 0x58→0x45` (F11, F12); `0x64..0x6E→0x68..0x72` (F13–F23); `0x76→0x73` (F24). Modifiers `0x1D 0x2A 0x36 0x38` → nullopt.
  - `e0 == true`: `0x1C→0x58 0x35→0x54 0x47→0x4A 0x48→0x52 0x49→0x4B 0x4B→0x50 0x4D→0x4F 0x4F→0x4D 0x50→0x51 0x51→0x4E 0x52→0x49 0x53→0x4C`; `0x1D 0x38 0x5B 0x5C` (RCtrl, RAlt, Win) → nullopt.
  - VK: `0x41..0x5A→0x04+(vk-0x41)`, `0x31..0x39→0x1E+(vk-0x31)`, `0x30→0x27`, `0x70..0x7B→0x3A+(vk-0x70)`, `0x7C..0x87→0x68+(vk-0x7C)`, `0x0D→0x28 0x1B→0x29 0x20→0x2C 0x09→0x2B 0x08→0x2A 0x25→0x50 0x26→0x52 0x27→0x4F 0x28→0x51`; else nullopt.
  - `usbIdsFromDevicePath`: case-insensitive search for `VID_` and `PID_` followed by exactly four hex digits (`juce::String::getHexValue32` on the 4-char substring); both present or nullopt.
  - `eligibleFootswitchIds`: for each `HidKeyboardInfo`, `isEligibleFootswitch(vid, pid, usagePage, usage, product, userDevice)` → insert.
  - `RawKeyState`: `std::map<DeviceId, std::set<uint8_t>>`; union computed on every call.
  Reference the table's source in a comment: Microsoft, *Keyboard Scan Code Specification* (scan code set 1 ↔ HID usage columns).
- [ ] **Step 4: Run** — `ctest --test-dir build -C Debug -R raw_input` → all pass.
- [ ] **Step 5: Commit** — `feat(control): pure Raw Input helpers (scan code and VK tables, device ids, per-device key state)`

---

### Task 5: Footswitch reader split: hidapi thread (macOS) and Raw Input (Windows)

**Files:**
- Modify: `engine/control/HidFootswitch.h` (private section), `engine/control/HidFootswitch.cpp` (becomes common-only), `engine/CMakeLists.txt`
- Create: `engine/control/HidFootswitchHidapi.cpp`, `engine/control/HidFootswitchRawInput.cpp`
- Test: existing `tests/control_input_test.cpp` (unchanged) must pass; Windows compile proven by CI

**Interfaces:**
- Consumes: Task 4's `RawInputKeys` (Windows reader), hidapi (`hid_enumerate` on both; `hid_open_path`/`hid_read_timeout`/`hidapi_darwin.h` only in the hidapi reader).
- Produces (private to the class; public API unchanged):
```cpp
// HidFootswitch.h — replaces ": private juce::Thread", run(), tryOpen()
struct Reader { virtual ~Reader() = default; virtual void start() = 0; virtual void stop() = 0; };
static std::unique_ptr<Reader> makeReader(HidFootswitch&);   // defined once per platform file
std::unique_ptr<Reader> reader;                              // created in start(), destroyed in stop()
// helpers the readers call (HidFootswitch.cpp):
void deliverKeys(const std::set<uint8_t>& keysDown);         // translate(keysDown) then post() each event
void releaseHeldKeys();                                      // deliverKeys({}) — existing function, kept
void setStatus(Status s, bool exclusive);                    // writes currentStatus and exclusiveOpen
std::optional<UsbIds> pickedDevice() const;                  // userDevice under its lock
```
  `start()`: `if (!reader) { reader = makeReader(*this); reader->start(); }`. `stop()`: `if (reader) { reader->stop(); reader.reset(); }` then the existing resets (`currentStatus = NotFound`, `exclusiveOpen = false`, `detector.feed({})`, `cancelPending()`).

- [ ] **Step 1: Header and common file** — apply the Interfaces changes to `HidFootswitch.h`; in `HidFootswitch.cpp` keep `KeyEdgeDetector`, `parseKeyboardReport`, `isEligibleFootswitch`, `keyboardsOnly`, `enumerationLock()`, constructor/destructor, `start/stop`, `setKeyAssignments`, `lastUnassignedKey`, `setUserDevice`, `enumerateDevices`, `translate`, `knownDevices`, `releaseHeldKeys`, plus the new `deliverKeys`, `setStatus`, `pickedDevice`. Move everything else (the `openPath`/`isOpenExclusive`/`inputMonitoringAccess`/`requestInputMonitoringOnce`/`looksLikePermissionError` helpers, `tryOpen`, `run`, `handleReport`, the IOKit/`hidapi_darwin.h` includes) into `HidFootswitchHidapi.cpp` as `class HidapiReader final : public HidFootswitch::Reader, private juce::Thread` whose `start()` is `startThread()` and `stop()` is `stopThread(4 * readTimeoutMs + rescanIntervalMs)`. The whole file is wrapped in `#if !JUCE_WINDOWS` (include `<juce_core/juce_core.h>` first for the macro). `enumerationLock()` becomes a function declared in `HidFootswitch.h`'s private section as `static std::mutex& enumerationLock();` so both files share it.
- [ ] **Step 2: Build and run the Mac suite** — `cmake -B build -G Xcode && cmake --build build --target amp_tests --config Debug && ctest --test-dir build -C Debug -R control_input` → all pass. Launch the Debug app with the pedal plugged in: Settings shows Connected and the pedals toggle (behaviour identical to before the split).
- [ ] **Step 3: Commit** — `refactor(control): move the hidapi footswitch reader behind a per-platform Reader interface`
- [ ] **Step 4: Write `HidFootswitchRawInput.cpp`** (`#if JUCE_WINDOWS`; includes `<windows.h>` after `juce_core`, `<hidsdi.h>` not needed): `class RawInputReader final : public HidFootswitch::Reader`:
  - `start()`: register a window class `L"AMPeratorRawInput"` once (`RegisterClassExW`, `lpfnWndProc = &RawInputReader::wndProc`), create `hwnd = CreateWindowExW(0, cls, L"", 0, 0,0,0,0, HWND_MESSAGE, nullptr, GetModuleHandle(nullptr), this)`, store `this` with `SetWindowLongPtrW(hwnd, GWLP_USERDATA, …)`; `RAWINPUTDEVICE rid { 0x01, 0x06, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, hwnd }; RegisterRawInputDevices(&rid, 1, sizeof rid)`; then `rescan()`.
  - `rescan()`: `eligible = RawInputKeys::eligibleFootswitchIds(HidFootswitch::enumerateDevices(), owner.pickedDevice())`; `owner.setStatus(eligible.empty() ? Status::NotFound : Status::Connected, false)`; forget cached device ids so they are resolved again.
  - `wndProc`: `WM_INPUT` → `GetRawInputData(…, RID_INPUT, …)` into a local buffer (size query first; 64 bytes suffices for keyboards); if `header.dwType == RIM_TYPEKEYBOARD`: `idsFor(header.hDevice)` (cache `std::map<HANDLE, std::optional<UsbIds>>`, filled via `GetRawInputDeviceInfoW(h, RIDI_DEVICENAME, …)` → `usbIdsFromDevicePath`); if the ids are in `eligible`: `usage = hidUsageFromScanCode(MakeCode, Flags & RI_KEY_E0)` else `hidUsageFromVirtualKey(VKey)`; if a usage: `owner.deliverKeys(keys.apply((uintptr_t) hDevice, *usage, !(Flags & RI_KEY_BREAK)))`. Always `DefWindowProc` afterwards (required for WM_INPUT cleanup). `WM_INPUT_DEVICE_CHANGE` → `GIDC_REMOVAL`: `owner.deliverKeys(keys.removeDevice((uintptr_t) lParam))`; both arrival and removal → `rescan()`.
  - `stop()`: `RAWINPUTDEVICE rid { 0x01, 0x06, RIDEV_REMOVE, nullptr }; RegisterRawInputDevices(...)`; `DestroyWindow(hwnd)`; `owner.deliverKeys({})`.
  - `makeReader` returns `std::make_unique<RawInputReader>(owner)`.
  In `engine/CMakeLists.txt`: `if(WIN32) list(APPEND … control/HidFootswitchRawInput.cpp) else() … control/HidFootswitchHidapi.cpp endif()` (both files also carry their `#if`, so listing both is harmless; list one to keep the build lean).
- [ ] **Step 5: Verify** — Mac: `cmake --build build --target amp_tests AMPerator --config Debug && ctest --test-dir build -C Debug` → all pass (the Windows file is not compiled). Push; the `windows` CI job must compile `HidFootswitchRawInput.cpp` and pass `control_input` and `raw_input` tests. Fix MSVC complaints in that file until green.
- [ ] **Step 6: Commit** — `feat(control): Raw Input footswitch reader for Windows (background input, hot-plug, shared mode)`

---

### Task 6: Audio driver choice: preferred device types, Driver picker, control panel

**Files:**
- Modify: `engine/audio/AudioIO.h`, `engine/audio/AudioIO.cpp` (`findType`, constructor), `engine/Engine.h`, `engine/Engine.cpp` (constructor: pass `AudioIO::platformPreferredTypes()`), `engine/bridge/CommandsSystem.cpp` (`getSettings`, new command), `engine/bridge/Bridge.cpp` (dispatch table line ~241), `docs/bridge-protocol.md`, `tests/support/FakeAudioDevice.{h,cpp}`, `tests/audio_io_test.cpp`, `tests/bridge_test.cpp`, `ui/src/state.ts`, `ui/src/views/settings.ts`, `ui/src/views/settings.test.ts`

**Interfaces:**
- Consumes: `AudioIO::apply/findType`, `SettingsPatch`, `json::strings`, UI `select(options, value, onChange, emptyLabel)` and `field(label, node)` helpers in `settings.ts`, `call(cmd, args)` in `bridge.ts`.
- Produces:
```cpp
// AudioIO.h
AudioIO(juce::AudioDeviceManager&, RealtimeEngine&, juce::StringArray preferredTypes = platformPreferredTypes());
static juce::StringArray platformPreferredTypes(); // Windows: the four names from Global Constraints; else empty
juce::StringArray deviceTypes() const;             // type names that list at least one device, manager order
bool hasControlPanel() const;                      // open device && device->hasControlPanel()
void showControlPanel();                           // device->showControlPanel(); no-op otherwise
// Engine.h
CommandResult openAudioControlPanel();             // success({}) always; calls audio->showControlPanel() when audio
// FakeAudioDevice.h
explicit FakeType(juce::String typeName = "Fake"); // getTypeName() returns it; devices/default unchanged
```
  Bridge: `getSettings`/`setSettings` replies add `availableDeviceTypes: [string]`, `hasControlPanel: bool`; command `openAudioControlPanel` → `{}`.
  UI (`state.ts` `Settings`): `availableDeviceTypes?: string[]; hasControlPanel?: boolean;`.

- [ ] **Step 1: Write the failing C++ tests** in `tests/audio_io_test.cpp`. Add a second fixture next to `Fixture`:
```cpp
struct TwoTypeFixture // "ASIO" and "Windows Audio (Low Latency Mode)" fakes; the preference list names ASIO first
{
    const juce::ScopedJuceInitialiser_GUI juce;
    juce::AudioDeviceManager manager;
    amp_test::FakeType* asio = nullptr;
    amp_test::FakeType* wasapi = nullptr;
    RealtimeEngine engine;
    std::unique_ptr<AudioIO> io;
    TwoTypeFixture(bool asioHasDevices = true)
    {
        auto a = std::make_unique<amp_test::FakeType>("ASIO");            // FakeType starts with "Fake Solo", "Fake Other"
        auto w = std::make_unique<amp_test::FakeType>("Windows Audio (Low Latency Mode)");
        if (! asioHasDevices) a->names.clear();
        asio = a.get(); wasapi = w.get();
        manager.addAudioDeviceType(std::move(w));                            // WASAPI first, as JUCE registers it
        manager.addAudioDeviceType(std::move(a));
        io = std::make_unique<AudioIO>(manager, engine, juce::StringArray { "ASIO", "Windows Audio (Low Latency Mode)" });
    }
};

TEST_CASE("audio_io: unknown saved type falls back to the preferred type")
{
    TwoTypeFixture f;
    Settings s; s.audioDeviceType = "CoreAudio"; s.audioDeviceName = "Scarlett Solo USB";
    REQUIRE(f.io->apply(s).wasOk());
    CHECK(f.manager.getCurrentAudioDeviceType() == "ASIO");
    CHECK(f.io->deviceTypes() == juce::StringArray { "Windows Audio (Low Latency Mode)", "ASIO" });
}

TEST_CASE("audio_io: missing preferred device opens the first preferred type with devices")
{
    TwoTypeFixture f(false); // ASIO installed but its interface is unplugged
    Settings s; s.audioDeviceType = "ASIO"; s.audioDeviceName = "Scarlett ASIO";
    REQUIRE(f.io->apply(s).wasOk());
    CHECK(f.manager.getCurrentAudioDeviceType() == "Windows Audio (Low Latency Mode)");
    CHECK(f.io->deviceTypes() == juce::StringArray { "Windows Audio (Low Latency Mode)" });
    f.asio->addDevice("Scarlett ASIO"); f.asio->notifyDeviceListChanged(); // plugged back: the existing reconnect path adopts it
    CHECK(f.manager.getCurrentAudioDeviceType() == "ASIO");
    CHECK(f.io->usingPreferred());
}

TEST_CASE("audio_io: empty preference list keeps the manager's current type")
{
    Fixture f; // one type, default-constructed AudioIO: platformPreferredTypes() is empty on macOS
    REQUIRE(f.io->apply(Settings {}).wasOk());
    CHECK(f.manager.getCurrentAudioDeviceType() == "Fake");
}

TEST_CASE("audio_io: fake devices have no control panel")
{
    Fixture f;
    REQUIRE(f.io->apply(Fixture::solo()).wasOk());
    CHECK_FALSE(f.io->hasControlPanel());
    f.io->showControlPanel(); // no-op, no crash
}
```
  and in `tests/bridge_test.cpp`: the `getSettings` reply has `availableDeviceTypes` equal to `["Fake"]` and `hasControlPanel == false`; `openAudioControlPanel` replies `ok: true`.
- [ ] **Step 2: Run** — `ctest --test-dir build -C Debug -R "audio_io|bridge"` → FAIL (no such constructor/fields).
- [ ] **Step 3: Implement** — `FakeType(typeName)` stores the name (`AudioIODeviceType(typeName)` base constructor). `AudioIO::findType`: after the saved-type and saved-name lookups fail, iterate `preferredTypes` and return the first whose type `hasDevices`; then the existing current-type / first-with-devices fallbacks. `deviceTypes()`, `hasControlPanel()`, `showControlPanel()` as in Interfaces. `Engine::openAudioControlPanel`; `Bridge.cpp` entry `{ "openAudioControlPanel", [](Engine& e, const juce::var&) { return e.openAudioControlPanel(); } }`; `getSettings` sets the two properties.
- [ ] **Step 4: Run** — `ctest --test-dir build -C Debug` → all pass.
- [ ] **Step 5: Write the failing UI tests** in `ui/src/views/settings.test.ts`: `renders no Driver select with one device type` (baseSettings + `availableDeviceTypes: ['CoreAudio']` → `document.querySelector('[data-field="driver"]')` is null); `Driver select switches type and clears the device` (`availableDeviceTypes: ['ASIO', 'Windows Audio']`, change the select to `Windows Audio` → `lastCommand()` is `setSettings` with `{ audioDeviceType: 'Windows Audio', audioDeviceName: '' }`); `Control panel button only with hasControlPanel` (absent for `false`, present for `true`, click → `lastCommand().cmd === 'openAudioControlPanel'`).
- [ ] **Step 6: Run** — `cd ui && npx vitest run src/views/settings.test.ts` → FAIL.
- [ ] **Step 7: Implement in `settings.ts`** — a `field('Driver', select(types.map(t => [t, t]), s.audioDeviceType, v => apply({ audioDeviceType: v, audioDeviceName: '' }), '—'))` rendered before the Device field only when `(s.availableDeviceTypes ?? []).length > 1`, with `data-field="driver"` on the field; a button `Control panel…` beside Buffer size when `s.hasControlPanel`, `onclick: () => call('openAudioControlPanel')`. Add the two optional fields to `Settings` in `state.ts`.
- [ ] **Step 8: Run** — `npx vitest run` → all pass (93 + new).
- [ ] **Step 9: Document** in `docs/bridge-protocol.md`: the two new `getSettings` fields and the `openAudioControlPanel` command (same table style as `getSettings`).
- [ ] **Step 10: Verify on the Mac** — Debug app: Settings unchanged (one type, no Driver row, no button). `ctest` + `vitest` green. Push; CI green on both jobs.
- [ ] **Step 11: Commit** — `feat(audio): prefer ASIO then WASAPI low latency on Windows, expose device types and the driver control panel to Settings`

---

### Task 7: WebView2 UI backend, runtime check, app build on Windows

**Files:**
- Modify: `cmake/Dependencies.cmake`, `cmake/AmpJuce.cmake`, `app/CMakeLists.txt`, `app/MainWindow.cpp`, `app/Main.cpp`, `app/MicrophonePermission.h`, `engine/Engine.cpp` (`openAudioDevice`), `THIRD_PARTY.md`, `.github/workflows/ci.yml`
- Create: `app/WebView2Runtime.h`, `app/WebView2Runtime.cpp`

**Interfaces:**
- Consumes: JUCE `FindWebView2.cmake` (`juce::juce_webview2`), `WebBrowserComponent::Options::WinWebView2`, `GetAvailableCoreWebView2BrowserVersionString` (WebView2.h).
- Produces:
```cpp
// app/WebView2Runtime.h
namespace amp {
/** Windows: the installed WebView2 runtime version, or nullopt when none is installed. Other platforms: nullopt. */
std::optional<juce::String> webView2RuntimeVersion();
/** True when this build draws its UI with WebView2 (Windows). */
constexpr bool needsWebView2Runtime();
}
```

- [ ] **Step 1: `cmake/Dependencies.cmake`** — under `if(WIN32)`, the `FetchContent_Declare(WebView2 …)` block from spec §4.1 (URL, `URL_HASH`, `DOWNLOAD_NAME`, `SOURCE_DIR`) and `FetchContent_MakeAvailable(WebView2)`; `set(JUCE_WEBVIEW2_PACKAGE_LOCATION "${CMAKE_BINARY_DIR}/_deps")`. Also silence hidapi's Windows target like the darwin one (`if(TARGET hidapi_winapi AND MSVC) target_compile_options(hidapi_winapi PRIVATE /W0)`).
- [ ] **Step 2: `cmake/AmpJuce.cmake`** — in the `WIN32` block: `JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1`; `list(APPEND CMAKE_MODULE_PATH "${juce_SOURCE_DIR}/extras/Build/CMake")`; `find_package(WebView2 REQUIRED)`; `target_link_libraries(amp_juce PRIVATE juce::juce_webview2)`.
- [ ] **Step 3: `app/CMakeLists.txt`** — `find_program(AMP_NPM_EXECUTABLE NAMES npm.cmd npm)`; `if(WIN32) set(_amp_npm "${AMP_NPM_EXECUTABLE}") else() <existing env wrapper> endif()`; `juce_add_gui_app(... NEEDS_WEBVIEW2 TRUE)`; add `WebView2Runtime.h WebView2Runtime.cpp` to `target_sources`.
- [ ] **Step 4: `app/WebView2Runtime.cpp`** — `#if JUCE_WINDOWS`: `#include <WebView2.h>`, `wil`-free: `LPWSTR v = nullptr; if (SUCCEEDED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &v)) && v) { juce::String s(v); CoTaskMemFree(v); return s; } return std::nullopt;` `#else` return nullopt. `needsWebView2Runtime()` is `JUCE_WINDOWS != 0`.
- [ ] **Step 5: `app/Main.cpp`** — at the top of `initialise`, `if constexpr (amp::needsWebView2Runtime())`: `const auto v = amp::webView2RuntimeVersion(); if (!v) { showWebView2Missing(); return; } juce::Logger::writeToLog("WebView2 runtime " + *v);`. `showWebView2Missing()`: `AlertWindow::showAsync` with `WarningIcon`, title `WebView2 Runtime required`, message `AMPerator's screen is drawn by Microsoft Edge WebView2, which is missing on this PC. Install the Evergreen runtime from Microsoft, then start AMPerator again.`, buttons `Download`, `Quit`; callback: result 1 → `juce::URL("https://developer.microsoft.com/en-us/microsoft-edge/webview2/").launchInDefaultBrowser()`; then `quit()` in both cases.
- [ ] **Step 6: `app/MainWindow.cpp`** — in `Content`'s initialiser, after `withKeepPageLoadedWhenBrowserIsHidden()`, add the `#if JUCE_WINDOWS` options block from spec §4.3 (backend `webview2`, user-data folder `windowsLocalAppData/AMPerator/WebView2`, background `0xff121316`, status bar and built-in error page disabled). Use a small helper `static juce::WebBrowserComponent::Options platformOptions(juce::WebBrowserComponent::Options)` so the chain stays readable.
- [ ] **Step 7: `engine/Engine.cpp` and `app/MicrophonePermission.h`** — in `openAudioDevice`, when `audio->apply(settings)` fails: `#if JUCE_WINDOWS` append `" If Windows blocks microphone access for desktop apps, allow it in Settings → Privacy & security → Microphone."` (UTF-8 via `juce::String::fromUTF8` as the existing text does). Update the header comment of `requestMicrophoneAccess` to say Windows needs no prompt and the privacy switch is reported through the `audioDevice` error.
- [ ] **Step 8: `THIRD_PARTY.md`** — row: Microsoft.Web.WebView2 1.0.3485.44 (NuGet, fetched at configure time on Windows) | Microsoft Software License Terms (`LICENSE.txt` in the package; the loader library may be redistributed within the application) | Web UI host on Windows (`WebView2LoaderStatic.lib`).
- [ ] **Step 9: Workflow** — in `.github/workflows/ci.yml` `windows` job: drop `-DAMP_BUILD_APP=OFF`; after ctest add `cmake --build build --config Release --target AMPerator` and `npm ci && npx vitest run` in `ui`.
- [ ] **Step 10: Verify** — Mac: configure/build/ctest/vitest unchanged and green; the Debug app still shows the UI (no Windows code compiled; `webView2RuntimeVersion()` returns nullopt and is never consulted). Push; `windows` job: configure downloads the WebView2 archive (hash ok), `amp_tests` Debug green, `AMPerator` Release links (`WebView2LoaderStatic.lib` resolved), vitest green. Fix until green.
- [ ] **Step 11: Commit** — `feat(app): WebView2 backend on Windows with a runtime check, WebView2 SDK fetched by CMake, Windows app in CI`

---

### Task 8: Packaging, artifact, documentation

**Files:**
- Create: `app/README-windows.txt`
- Modify: `app/CMakeLists.txt` (`amp_package`), `.github/workflows/ci.yml` (package + upload), `README.md`, `docs/manual-test-checklist.md`

**Interfaces:**
- Produces: CMake target `amp_package` (WIN32) writing `${CMAKE_BINARY_DIR}/AMPerator-${PROJECT_VERSION}-windows-x64.zip`; CI artifact `AMPerator-windows-x64`.

- [ ] **Step 1: `app/README-windows.txt`** — plain text, ~25 lines: what it is (link to the GitHub repo), requirements (Windows 10 1809+/11, x64; Microsoft Edge WebView2 Runtime with `https://developer.microsoft.com/en-us/microsoft-edge/webview2/`; your interface's ASIO driver recommended, WASAPI works without one), first run (SmartScreen "Windows protected your PC" → More info → Run anyway, because the exe is not signed yet), where files live (`Documents\AMPerator\Models`, `IRs`, `Rigs`, `settings.json`), footswitch note (presses also reach the front app; set pedals to F13/F14/F15), license (AGPLv3; see LICENSE and THIRD_PARTY.md).
- [ ] **Step 2: `amp_package` target** in `app/CMakeLists.txt` under `if(WIN32)`: `add_custom_target(amp_package COMMAND ${CMAKE_COMMAND} -E make_directory "${_pkg_dir}" COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:AMPerator>" "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/THIRD_PARTY.md" "${CMAKE_CURRENT_SOURCE_DIR}/README-windows.txt" "${_pkg_dir}" COMMAND ${CMAKE_COMMAND} -E tar cf "${CMAKE_BINARY_DIR}/AMPerator-${PROJECT_VERSION}-windows-x64.zip" --format=zip -- . WORKING_DIRECTORY "${_pkg_dir}" DEPENDS AMPerator)` with `_pkg_dir = ${CMAKE_BINARY_DIR}/package/AMPerator`, so the zip unpacks into one `AMPerator\` folder.
- [ ] **Step 3: Workflow** — replace the Release `AMPerator` build with `cmake --build build --config Release --target amp_package`; add `actions/upload-artifact@v4` with `name: AMPerator-windows-x64`, `path: build/AMPerator-*-windows-x64.zip`, `retention-days: 14`, `if-no-files-found: error`.
- [ ] **Step 4: README.md** — under Build: a "Windows" subsection with the four commands from spec §4.1 and requirements; `-DAMP_ASIO=OFF` explained in one line; "Download" note that CI publishes a zip per push (Actions → artifacts) until releases exist. Under Running: WebView2 runtime line; mark the Input Monitoring and exclusive-mode sentences as macOS; a Windows footswitch sentence (always shared; F13–F15). Project overview line "the macOS app" → "the app (macOS: WKWebView; Windows: WebView2)".
- [ ] **Step 5: `docs/manual-test-checklist.md`** — add a "## Windows" section with the checks from spec §6 (zip runs on a PC without the VC++ redistributable; WebView2 dialog when the runtime is missing — optional; Stage/Settings render; Driver lists ASIO and WASAPI types; Scarlett via ASIO plays, latency at 64/128 noted; Control panel… opens the Focusrite panel and a buffer change rebuilds without restart; WASAPI Low Latency plays; footswitch assign/toggle with Notepad in front; F13–F15 pedals type nothing visible; a Mac-saved rig plays the same; a rig named `CON` saves and reloads; `set AMP_UI_DEV=1` dev server in Debug; `/art/` images show; a VST3 editor opens; clean quit).
- [ ] **Step 6: Verify** — Push; `windows` job uploads `AMPerator-windows-x64` (download it from the run page; the zip holds `AMPerator\AMPerator.exe`, `LICENSE`, `THIRD_PARTY.md`, `README-windows.txt`). `macos` job green.
- [ ] **Step 7: Commit** — `build(windows): zip package target, CI artifact, Windows docs and manual checklist`

---

### Task 9: Dependency cache for CI

**Files:**
- Modify: `.github/workflows/ci.yml`

- [ ] **Step 1: Cache the FetchContent sources** — in both jobs, before configure: `actions/cache@v4` with `path: ${{ github.workspace }}/deps`, `key: deps-${{ runner.os }}-${{ hashFiles('cmake/Dependencies.cmake') }}`; pass `-DFETCHCONTENT_BASE_DIR=${{ github.workspace }}/deps` to `cmake -B`.
- [ ] **Step 2: Verify** — Push twice: the second run's configure step logs no git clones (JUCE, NAM core, hidapi, Catch2) and no WebView2 download; both jobs green; the Windows job time drops by the fetch time only.
- [ ] **Step 3: Commit** — `ci: cache FetchContent sources between runs`

---

### Task 10: Verification on the Windows PC

**Files:**
- Modify: `docs/manual-test-checklist.md` (results), any fix commits named `fix(windows): …`

- [ ] **Step 1:** Download the `AMPerator-windows-x64` artifact from the latest green run onto the PC, unpack, run `AMPerator.exe`, and work through the "## Windows" checklist section, recording latency numbers and the footswitch key codes.
- [ ] **Step 2: WASAPI-only build** — on the PC: `cmake -B build-noasio -A x64 -DAMP_ASIO=OFF && cmake --build build-noasio --config Release --target AMPerator`; the app runs, Settings → Driver lists only the Windows Audio types, the Scarlett plays through WASAPI Low Latency.
- [ ] **Step 3:** For each failed item, open an issue-sized note in the checklist and fix it on the Mac with CI as the gate; the fixes that need the PC to confirm (Raw Input table rows, resource-provider URL shape, ASIO control panel restart) are retested by downloading the next artifact.
- [ ] **Step 4: Commit** — `docs: Windows manual test results (<date>, <Windows version>)`
