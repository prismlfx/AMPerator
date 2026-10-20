# AMPerator — Windows Build (Sub-project 5)

**Date:** 2026-10-20
**Status:** Draft for review
**Builds on:** `2026-10-19-amperator-core-design.md` (sub-project 1, merged). Independent of sub-projects 3 and 4.

## 1. Purpose

AMPerator is written against JUCE 8 and has stayed cross-platform in intent, but it has only ever been compiled on
macOS. This sub-project turns it into a program a Windows guitarist can download and play: an `AMPerator.exe` that
opens the interface through ASIO (with WASAPI as the fallback), draws the web UI in Microsoft Edge WebView2, reads the
USB footswitch in the background, and keeps every rig, model and IR file compatible with the Mac build.

**Who it is for:** the same player as sub-project 1, now on a Windows PC (Scarlett Solo, a 3-pedal PCsensor-style USB
footswitch), plus anyone who finds the repository and only owns a PC. The developer works on a Mac day to day and has
a Windows PC only for testing, so **continuous integration on GitHub is the first proof** that the Windows build works;
the PC is for the things CI cannot see (sound, latency, pedals, the WebView2 window).

**Success criteria**
1. A GitHub Actions workflow builds the engine, tests and app on `windows-latest` with MSVC, runs the Catch2 suite
   (257 tests today, plus this sub-project's) and the vitest suite (93 today) green, and uploads a zip holding `AMPerator.exe`. The macOS build and
   tests run in the same workflow and stay green.
2. On the Windows PC, the zip runs with nothing installed but the WebView2 runtime (present on Windows 11 and on
   up-to-date Windows 10): Settings offers the ASIO driver of the interface, round-trip latency at 64–128 samples is
   in the same range as on the Mac, and the Stage/Edit/Settings views look and behave as on macOS.
3. The footswitch toggles pedals while another window is in front, with the same L/M/R assignment flow as on macOS.
4. A rig saved on the Mac opens on Windows (and back) with the same models, IRs and knob values; rig and band
   names that are illegal on Windows (`CON`, `AUX`, `COM1`, …) save safely on both platforms.
5. Building with ASIO disabled (`-DAMP_ASIO=OFF`) produces a working WASAPI-only app, for anyone who prefers not to
   accept Steinberg's ASIO terms.

## 2. What the survey found (facts that shape the design)

Checked on 2026-10-20 in the fetched dependency sources under `build/_deps` (JUCE 8.0.15, hidapi 0.15.0) and online.

| Fact | Consequence |
|---|---|
| **JUCE 8.0.15 bundles the Steinberg ASIO SDK headers** (`modules/juce_audio_devices/native/asio/{asio.h,asiosys.h,iasiodrv.h}`) and `JUCE_ASIO=1` uses them by default (`JUCE_ASIO_USE_EXTERNAL_SDK=0`). Their `LICENSE.txt` offers them under the proprietary Steinberg ASIO License **or GPLv3**, at the user's choice. GPLv3 code may be combined with AGPLv3 code (AGPLv3 §13). | No download from Steinberg is needed, at configure time or ever: AMPerator already fetches JUCE, and the headers come with it. The brief's "fetch the SDK from Steinberg's URL" is dropped (see §7.1); the documented opt-out (`AMP_ASIO`, default ON) stays, falling back to WASAPI. The notice goes into `THIRD_PARTY.md`. |
| JUCE's Windows `WebBrowserComponent` **defaults to the Internet Explorer control**; WebView2 must be requested with `Options::Backend::webview2`, and when the WebView2 runtime is missing JUCE **silently falls back to IE**, where the Vite/TypeScript UI would not run. WebView2 is compiled in with `JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1` on the module (that define turns on `JUCE_USE_WIN_WEBVIEW2`) and links `WebView2LoaderStatic.lib` from the `Microsoft.Web.WebView2` NuGet package. JUCE's `FindWebView2.cmake` looks for a directory named `*Microsoft.Web.WebView2*` under `JUCE_WEBVIEW2_PACKAGE_LOCATION` (default: the user's NuGet cache) and names version **1.0.3485.44**. | The app asks for the WebView2 backend explicitly, checks the runtime itself before opening the window (friendly error, download link, quit), and the build fetches the NuGet package (a zip, pinned by SHA-256) with FetchContent, so CI and a fresh PC need no NuGet tooling. |
| **hidapi cannot read input reports from keyboard-class devices on Windows**: `windows/hid.c` opens keyboards and mice without read/write access because "the system takes exclusive control over them … to prevent keyloggers". Enumeration (vendor/product ids, product string, usage page/usage) still works. There is no exclusive grab on Windows short of a filter driver. | The current reader thread (hidapi `hid_read_timeout`) stays for macOS; Windows gets a **Raw Input** backend (`WM_INPUT`, registered with `RIDEV_INPUTSINK` so presses arrive while another app is in front) behind the same `HidFootswitch` interface. Mode is always "shared"; the F13–F15 guidance already in the UI applies. |
| `AudioDeviceManager` on Windows registers five device types in this order: Windows Audio (WASAPI shared), Windows Audio (Exclusive Mode), Windows Audio (Low Latency Mode), DirectSound, ASIO. `AudioIO::findType` with no saved type takes the manager's current type, i.e. WASAPI shared (the slowest). The Settings view has no device-type picker because macOS has only CoreAudio. | DirectSound is compiled out (legacy, duplicates WASAPI). With no saved type the engine prefers ASIO, then WASAPI Low Latency, then WASAPI shared. Settings gains a **Driver** picker (shown only when more than one type has devices) and a **Control panel…** button for ASIO drivers whose buffer size is set in the vendor panel (the Focusrite driver is one). |
| `RtSafety.cpp` already carries `_WIN32` branches (`_aligned_malloc`/`_aligned_free`). MSVC supports replacing the global allocation functions from an object that the executable links. | The guard is kept as written; the existing Debug test `realtime_engine: RtSafety counts allocations inside ScopedNoAlloc` run on Windows CI is the proof. `_CrtSetAllocHook` is the documented fallback if MSVC rejects the replacement (§7.5). |
| MSVC details: `engine/engine/RealtimeEngine.h` contains UTF-8 (an em dash) so MSVC needs `/utf-8`; Eigen-heavy translation units can exceed the default object section limit (`/bigobj`); CMake's default Debug flags include `/RTC1`, which is incompatible with the `/O2` that `nam_core` needs in Debug (unoptimised Eigen is too slow for the regression test, as the existing `-O2` on clang notes); the default `/MD` CRT needs the VC++ redistributable installed. `npm` is `npm.cmd` on Windows and the PATH separator in `app/CMakeLists.txt` is `:`. | Root CMake sets `/utf-8`, strips `/RTC1` from the Debug flags, gives `nam_core` `/O2` in Debug, adds `/bigobj`, and links the **static CRT** so the zip runs on a bare PC. `find_program` looks for `npm.cmd` first and the PATH wrapper is skipped on Windows. |
| `windows-latest` moved to Windows Server 2025 with **Visual Studio 2026 (MSBuild 18)** in June 2026; Node is not pinned by the image. | The workflow does not hard-code a generator: `cmake -B build -A x64` lets CMake pick the installed Visual Studio (2022 on the user's PC, 2026 on the runner). Node is pinned with `actions/setup-node` (22). |
| Library root already resolves to `Documents\AMPerator` on Windows (`RigStore::defaultLibraryRoot`), NAM model paths already go through `std::filesystem::path` from wide strings, `juce::File::moveToTrash`, `TemporaryFile` and the atomic rename are implemented by JUCE on Windows, and one test (`rig_store: failed write leaves backups and rig untouched`) is already `#if !JUCE_WINDOWS`. WebView2 by default writes its user-data folder **next to the executable**, which fails under `Program Files`. | Storage needs no change beyond reserved names. The WebView2 user-data folder is set to `%LOCALAPPDATA%\AMPerator\WebView2`. |
| Windows has no microphone permission prompt for desktop apps, but the privacy switch "Let desktop apps access your microphone" makes WASAPI capture fail to open. `juce::RuntimePermissions::request` (already used in `app/MicrophonePermission.cpp` for non-Apple platforms) answers "granted" at once. | `MicrophonePermission.cpp` is unchanged in behaviour (comment updated). The `audioDevice` startup error gets a Windows-only hint pointing at that switch. |

## 3. Scope

**In scope**
- CMake: Windows/MSVC configuration, ASIO option, WebView2 package fetch, static CRT, per-platform JUCE options,
  `npm.cmd`, a `amp_package` target that writes the Windows zip.
- Engine: Raw Input footswitch backend; device-type preference and picker support; reserved file names; the
  Windows audio hint; whatever MSVC needs to compile warning-clean enough to read (no `/WX`).
- App: WebView2 backend options, runtime check, user-data folder.
- UI: Driver picker and Control panel button in Settings (hidden when irrelevant, so macOS is unchanged).
- CI: `.github/workflows/ci.yml` with a macOS job and a Windows job; the Windows job uploads the zip.
- Docs: README "Windows" section, `THIRD_PARTY.md` rows, bridge protocol additions, manual checklist section.

**Out of scope (later)**
- An installer (MSI/Inno Setup), code signing and SmartScreen reputation; the zip will show the "unknown publisher"
  warning on first run, documented in the zip's `README-windows.txt`.
- Windows on ARM64 (x64 only), Linux, MIDI via Windows MIDI Services (JUCE's WinMM default is used).
- Exclusive access to the footswitch on Windows (needs a filter driver; not possible for a user-mode app).
- An app icon for the exe (JUCE's default icon is used; cosmetic).
- Any change to the rig file format, the bridge protocol version, or the DSP.

## 4. Design

### 4.1 Build system

**Root `CMakeLists.txt`** (before `include(cmake/Dependencies.cmake)`, so every target inherits it):
```cmake
if(MSVC)
  # Static CRT: the zipped exe runs without the VC++ redistributable. Catch2/hidapi (FetchContent) follow suit (CMP0091).
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/utf-8)                                   # sources are UTF-8 (RealtimeEngine.h)
  string(REPLACE "/RTC1" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}") # nam_core gets /O2 in Debug (D8016 otherwise)
endif()
if(WIN32)
  option(AMP_ASIO "ASIO devices through the ASIO SDK headers bundled with JUCE (Steinberg license or GPLv3; OFF = WASAPI only)" ON)
endif()
```

**`cmake/Dependencies.cmake`** adds, on `WIN32` only, the WebView2 SDK as a pinned archive (a `.nupkg` is a zip;
`DOWNLOAD_NAME` with a `.zip` suffix makes CMake extract it; the explicit `SOURCE_DIR` name is what JUCE's find
module globs for):
```cmake
FetchContent_Declare(WebView2
  URL https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.3485.44
  URL_HASH SHA256=bc09150b179246ac90189649b13be8e6b11b3ac200e817e18df106e1f3cf489e
  DOWNLOAD_NAME Microsoft.Web.WebView2.1.0.3485.44.zip
  SOURCE_DIR ${CMAKE_BINARY_DIR}/_deps/Microsoft.Web.WebView2.1.0.3485.44)
set(JUCE_WEBVIEW2_PACKAGE_LOCATION ${CMAKE_BINARY_DIR}/_deps)
```
(The hash was computed on 2026-10-20 from the package nuget.org serves; it contains `build/native/include/WebView2.h`,
`WebView2EnvironmentOptions.h`, `build/native/x64/WebView2LoaderStatic.lib` and `LICENSE.txt`.) hidapi's Windows
target is `hidapi_winapi`; the existing `hidapi::hidapi` alias resolves to it, so `amp_engine` is unchanged.

**`cmake/AmpJuce.cmake`** gets per-platform JUCE options next to the common ones:
```cmake
if(APPLE)
  target_compile_definitions(amp_juce PRIVATE JUCE_PLUGINHOST_AU=1)
endif()
if(WIN32)
  target_compile_definitions(amp_juce PRIVATE
    JUCE_ASIO=$<BOOL:${AMP_ASIO}> JUCE_WASAPI=1 JUCE_DIRECTSOUND=0
    JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1)
  list(APPEND CMAKE_MODULE_PATH "${juce_SOURCE_DIR}/extras/Build/CMake")
  find_package(WebView2 REQUIRED)                    # JUCE's module: juce::juce_webview2 (include dir + static loader lib)
  target_link_libraries(amp_juce PRIVATE juce::juce_webview2)
endif()
```
`juce_gui_extra` is compiled inside `amp_juce`, so the define and the include path must be there, not on the app;
the loader library propagates to the app through the static library's link interface.

**`engine/CMakeLists.txt`**: on MSVC, `nam_core` gets `/bigobj` and `$<$<CONFIG:Debug>:/O2>` (the clang branch
already has `-O2`; the D9025 "overriding /Od" warning is expected and commented), `amp_engine` gets `/bigobj`. The
footswitch sources become `control/HidFootswitch.cpp` (common), `control/RawInputKeys.{h,cpp}` (common, pure
tables) and one of `control/HidFootswitchHidapi.cpp` / `control/HidFootswitchRawInput.cpp` chosen by `WIN32`.

**`app/CMakeLists.txt`**: `find_program(AMP_NPM_EXECUTABLE NAMES npm.cmd npm)`; the `cmake -E env PATH=…` wrapper is
used only when `NOT WIN32` (on Windows the build inherits the shell's PATH, where node already is); `juce_add_gui_app`
gets `NEEDS_WEBVIEW2 TRUE`; `app/WebView2Runtime.{h,cpp}` is added on every platform (its body is `#if JUCE_WINDOWS`; elsewhere it returns "not applicable"). A `WIN32`-only custom target
`amp_package` zips `$<TARGET_FILE:AMPerator>`, `LICENSE`, `THIRD_PARTY.md` and `app/README-windows.txt` into
`${CMAKE_BINARY_DIR}/AMPerator-${PROJECT_VERSION}-windows-x64.zip` with `cmake -E tar cf … --format=zip`, so the
same command packages locally and in CI.

**Local build on Windows** (documented in the README): Visual Studio 2022 or 2026 with "Desktop development with
C++", CMake 3.24+, Node 20+, Git; then
```
cmake -B build -A x64
cmake --build build --config Release --target AMPerator          # build\app\AMPerator_artefacts\Release\AMPerator.exe
cmake --build build --config Debug --target amp_tests && ctest --test-dir build -C Debug
cmake --build build --config Release --target amp_package        # build\AMPerator-0.1.0-windows-x64.zip
```

### 4.2 Audio: ASIO first, WASAPI fallback

- With `AMP_ASIO=ON` (default) the device types are **ASIO**, **Windows Audio (Low Latency Mode)**, **Windows Audio
  (Exclusive Mode)** and **Windows Audio**. With `AMP_ASIO=OFF` the three WASAPI types remain. No code in `engine/`
  mentions ASIO; everything goes through `juce::AudioIODeviceType` as today.
- `AudioIO::findType`: when the saved type is empty or lists no devices and no saved device name is found, the type
  is picked from a **preference list** instead of the manager's current type: `{"ASIO", "Windows Audio (Low Latency
  Mode)", "Windows Audio (Exclusive Mode)", "Windows Audio"}` on Windows, empty elsewhere (so macOS behaviour is
  unchanged). Only types that list at least one device qualify; if none of the preferred types does, the first type
  with devices is used as now. The list is a constructor parameter (`AudioIO(manager, engine, preferredTypes =
  AudioIO::platformPreferredTypes())`) so the fake-device tests can drive it.
- New read-only `AudioIO` methods: `juce::StringArray deviceTypes() const` (type names that list a device),
  `bool hasControlPanel() const`, `void showControlPanel()` (ASIO drivers; JUCE stops the device, shows the panel
  and restarts it, which reaches the engine through the existing `onDeviceRestarted` path and rebuilds the plan at
  the new buffer size).
- Bridge (additive, protocol stays 1): `getSettings`/`setSettings` replies gain `availableDeviceTypes: [name]` and
  `hasControlPanel: bool`; new command `openAudioControlPanel` → `{}` (no-op without a panel). `docs/bridge-protocol.md`
  documents both.
- Settings view: a **Driver** select above Device, rendered only when `availableDeviceTypes.length > 1`; choosing one
  sends `setSettings({audioDeviceType, audioDeviceName: ""})` so the new type's default device opens and is written
  back (an empty name is what `AudioIO::apply` treats as "the type's default"). A **Control panel…** button next to
  Buffer size, rendered only when `hasControlPanel`. On macOS neither appears, so the view is pixel-identical there.
- Default settings on a fresh Windows machine: type empty → ASIO if the interface's driver is installed, otherwise
  WASAPI Low Latency; sample rate 48000, buffer 128 as today.

### 4.3 Web UI on WebView2

`app/MainWindow.cpp` adds, under `#if JUCE_WINDOWS`, to the `WebBrowserComponent::Options`:
```cpp
.withBackend(juce::WebBrowserComponent::Options::Backend::webview2)
.withWinWebView2Options(juce::WebBrowserComponent::Options::WinWebView2 {}
    .withUserDataFolder(juce::File::getSpecialLocation(juce::File::windowsLocalAppData)
                            .getChildFile("AMPerator").getChildFile("WebView2"))
    .withBackgroundColour(juce::Colour(0xff121316))   // the window colour: no white flash before the page paints
    .withStatusBarDisabled()
    .withBuiltInErrorPageDisabled())
```
Everything else (native functions `amp`/`chooseFolder`, the resource provider, `getResourceProviderRoot()`, the
`/art/` route, the `AMP_UI_DEV=1` dev server in Debug, the navigation lock) is backend-independent JUCE 8 API and is
expected to work unchanged; §6 lists the on-PC checks for it.

**Runtime check.** `app/WebView2Runtime.{h,cpp}` exposes `std::optional<juce::String> webView2RuntimeVersion()`:
on Windows it calls `GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)` from the statically linked
loader (the app target has `NEEDS_WEBVIEW2`, which gives it the SDK include path); it returns `nullopt` when the
call fails or on other platforms. `AmperatorApp::initialise` calls it first on Windows, before the Engine exists:
when it is empty, a native `AlertWindow` titled "WebView2 Runtime required" explains that AMPerator's screen is drawn
by Microsoft Edge WebView2, which is missing on this PC, with buttons **Download** (opens
`https://developer.microsoft.com/en-us/microsoft-edge/webview2/` in the default browser) and **Quit**; both quit
the app. The found version is logged with `juce::Logger::writeToLog` to help bug reports.

### 4.4 Footswitch on Windows: Raw Input behind the same interface

`HidFootswitch`'s public interface (`Status`, `start/stop`, `status()`, `exclusive()`, `setUserDevice`,
`enumerateDevices`, `setKeyAssignments`, `lastUnassignedKey`, `setCapturing`, `translate`, `knownDevices`) and the
free functions tested today (`KeyEdgeDetector`, `parseKeyboardReport`, `isEligibleFootswitch`, `keyboardsOnly`)
do not change, so `Engine`, the bridge, the UI and `control_input_test.cpp` are untouched. The class stops deriving
from `juce::Thread`; the reading side becomes a private `struct Reader` with one implementation per platform, created
in `start()` and destroyed in `stop()` (constructing a `HidFootswitch` without `start()` creates nothing, as the
headless test relies on).

| | macOS (`HidFootswitchHidapi.cpp`, `#if !JUCE_WINDOWS`) | Windows (`HidFootswitchRawInput.cpp`, `#if JUCE_WINDOWS`) |
|---|---|---|
| Source of key data | hidapi reader thread, 8/9-byte boot reports, as today (code moved, not changed) | Raw Input: a message-only window (`HWND_MESSAGE`) created on the message thread; `RegisterRawInputDevices` for usage page 0x01 / usage 0x06 with `RIDEV_INPUTSINK` (input while another app is in front) and `RIDEV_DEVNOTIFY` (plug/unplug) |
| Device identity | enumeration + `hid_open_path` | `WM_INPUT` → `GetRawInputData` → `RAWINPUTHEADER::hDevice` → `GetRawInputDeviceInfo(RIDI_DEVICENAME)` once per handle → vendor/product ids parsed from the path (`\\?\HID#VID_0C45&PID_7403&MI_00#…`) |
| Eligibility | `isEligibleFootswitch` per enumerated collection | `isEligibleFootswitch` over hidapi's enumeration (it still enumerates keyboards on Windows, with usage page/usage and product string) gives the set of eligible (vid, pid); a Raw Input event counts only when its device's ids are in that set |
| Key codes | HID usages straight from the report | `RAWKEYBOARD::MakeCode` (+ `RI_KEY_E0`) → HID usage through `RawInputKeys::hidUsageFromScanCode` (PS/2 scan code set 1 table; layout-independent), falling back to `hidUsageFromVirtualKey(VKey)`; unknown keys are ignored; `RI_KEY_BREAK` = release. A keys-down set per device handle is kept; the union goes through `translate()` exactly like a report, so `KeyEdgeDetector` produces the edges and auto-repeat collapses as today |
| Status | Connected (exclusive or shared), NotFound, PermissionDenied, Busy | Connected with `exclusive() == false` while at least one eligible device is enumerated, else NotFound. PermissionDenied and Busy never occur |
| Hot-plug | 2 s rescan while nothing is open | `WM_INPUT_DEVICE_CHANGE` (arrival/removal) re-enumerates and updates the status; a removed device's held keys are released |
| Threading | reader thread → `post()` | all on the message thread; `post()` still hops through `callAsync`, so delivery order and `cancelPending()` semantics are unchanged |

`control/RawInputKeys.{h,cpp}` is pure, compiled and unit-tested on every platform:
```cpp
namespace amp::RawInputKeys {
std::optional<uint8_t> hidUsageFromScanCode(uint16_t makeCode, bool e0);  // letters, digits, Enter/Esc/Backspace/Tab/Space, F1–F24, arrows, keypad Enter
std::optional<uint8_t> hidUsageFromVirtualKey(uint16_t vk);                // VK_A..Z, VK_0..9, VK_F1..F24, VK_RETURN/ESCAPE/SPACE/TAB, arrows
std::optional<UsbIds>  usbIdsFromDevicePath(const juce::String& path);     // "VID_xxxx&PID_xxxx", any case; nullopt when absent
}
```
The table follows Microsoft's Keyboard Scan Code Specification (scan code set 1 ↔ HID usage); the F13–F15 row
(set 1 `0x64/0x65/0x66` ↔ HID `0x68/0x69/0x6A`) is the one the PCsensor pedals rely on and is covered by a unit test
and by the manual checklist on the PC.

**User-visible behaviour on Windows:** Settings shows "Connected — shared: presses also type into other apps — tip:
set your pedal to send F13/F14/F15" (the existing text), pedals toggle nodes while another window is in front, and
the L/M/R assign flow (`hidkey:<code>` events) works unchanged because the codes are HID usages on both platforms, so
`settings.json` from a Mac keeps meaning the same keys.

### 4.5 The other platform branches

| File | Change |
|---|---|
| `app/MicrophonePermission.cpp` | Behaviour unchanged (non-Apple: `juce::RuntimePermissions::request`, which answers "granted" immediately). The header comment names Windows explicitly. `Engine::openAudioDevice` appends, under `JUCE_WINDOWS`, to a failed `audioDevice` startup error: "If Windows blocks microphone access for desktop apps, allow it in Settings → Privacy & security → Microphone." |
| `engine/engine/RtSafety.cpp` | No code change. The header comment documents that on MSVC the same replacement of the global allocation functions is used (the object is pulled in by `ScopedNoAlloc`, so the exe's definitions win over the CRT library's), that `_aligned_malloc` is the aligned path there, and names `_CrtSetAllocHook` as the fallback. CI runs the Debug suite on Windows, which exercises it. |
| `engine/storage/RigStore.cpp` | `sanitizeFileName` (every platform, so Mac-saved names stay portable): after the existing rules, if the part before the first `.` equals, ignoring case, one of `CON PRN AUX NUL COM0–COM9 LPT0–LPT9`, a `_` is appended to that part: `CON` → `CON_`, `nul` → `nul_`, `com1.tone` → `com1_.tone`; `CONCERT` and `LPT10` are untouched. Band folders go through the same function, so `Rigs\CON\` cannot happen either. |
| `engine/dsp/NamAmpNode.cpp` | No change expected: the `JUCE_WINDOWS` wide-path branch exists. The MSVC compile sweep (plan) fixes anything the compiler rejects. |
| `engine/audio/AudioIO.cpp` | §4.2 (preference list, device types, control panel). |
| `engine/control/HidFootswitch*.cpp` | §4.4. |

### 4.6 Continuous integration

`.github/workflows/ci.yml`, triggered on pushes to `master`, pull requests and `workflow_dispatch`, with a
`concurrency` group that cancels superseded runs. Two independent jobs:

| Job | Runner | Steps |
|---|---|---|
| `macos` | `macos-15` | checkout; `actions/setup-node` 22; `cmake -B build -G Xcode`; `cmake --build build --config Debug --target amp_tests AMPerator`; `ctest --test-dir build -C Debug --output-on-failure`; `cd ui && npm ci && npx vitest run` |
| `windows` | `windows-latest` | checkout; `actions/setup-node` 22; `cmake -B build -A x64`; `cmake --build build --config Debug --target amp_tests`; `ctest --test-dir build -C Debug --output-on-failure`; `cmake --build build --config Release --target amp_package`; `cd ui && npm ci && npx vitest run`; `actions/upload-artifact` of `build/AMPerator-*-windows-x64.zip` named `AMPerator-windows-x64` (14-day retention) |

The Windows job builds JUCE twice (Debug for the allocation guard and the rest of the suite, Release for what ships),
roughly 30–40 minutes on a hosted runner; the macOS job is about half that. `FETCHCONTENT_BASE_DIR` is pointed at a
folder cached with `actions/cache` keyed on `cmake/Dependencies.cmake`, so the dependency clones (JUCE, Eigen) are
downloaded once per dependency change; the plan treats the cache as the last task, after the first green run.
Timeouts: 90 minutes (Windows), 60 (macOS). Secrets: none.

### 4.7 Packaging

`AMPerator-<version>-windows-x64.zip` holds `AMPerator.exe` (Release, static CRT, x64), `LICENSE`, `THIRD_PARTY.md`
and `README-windows.txt` (requirements: Windows 10 1809+/11 x64 and the WebView2 runtime with its download link; the
interface's ASIO driver recommended; the SmartScreen "unknown publisher" note because the exe is unsigned; where files
live: `Documents\AMPerator`). `<version>` is `PROJECT_VERSION` (0.1.0). No installer, no signing (later).

### 4.8 Documentation

README: a "Windows" subsection under Build (requirements, commands, `AMP_ASIO`), the WebView2 note under Running,
"Input Monitoring"/"exclusive" wording marked as macOS-only, and the data folder table already mentions
`Documents\AMPerator`. `THIRD_PARTY.md`: ASIO SDK headers (Steinberg, used under the GPLv3 option, bundled in JUCE)
and Microsoft.Web.WebView2 1.0.3485.44 (Microsoft Software License Terms; the loader library is redistributable
inside the app). `docs/manual-test-checklist.md`: a "Windows" section (§6). `docs/bridge-protocol.md`: §4.2 additions.

## 5. Error handling

| Situation | Behaviour |
|---|---|
| WebView2 runtime missing | Native dialog before any window: explanation, **Download** (opens Microsoft's page) and **Quit**; the app exits without creating the Engine or touching audio. |
| WebView2 user-data folder cannot be created (`%LOCALAPPDATA%` read-only) | JUCE/WebView2 report the failure in the browser creation; the page stays blank. Documented; not expected on a normal PC. |
| No ASIO driver installed | ASIO lists no devices and is skipped by the preference list; WASAPI Low Latency opens. Settings → Driver shows what is available. |
| Build with `AMP_ASIO=OFF` | WASAPI-only app; Settings shows the three WASAPI types. No reference to ASIO anywhere in the binary. |
| Audio device fails to open on Windows | Existing `audioDevice` banner plus the microphone-privacy hint. |
| Footswitch presses type into the front app | Expected on Windows (no exclusive grab); Settings shows "shared" with the F13–F15 tip. |
| Footswitch unplugged | `WM_INPUT_DEVICE_CHANGE` → held keys released (same edges as the macOS unplug path), status NotFound, Stage tiles follow. |
| Rig or band named `CON`, `COM1`, … | Saved as `CON_.amprig` / `Rigs\COM1_\`; the display name is kept exactly in the JSON, as with `/` and `:` today. |
| Unsigned exe on first run | SmartScreen "unknown publisher" prompt; `README-windows.txt` explains "More info → Run anyway". Signing is a later sub-project. |
| CI: Steinberg or Microsoft download unavailable | ASIO needs no download. The WebView2 fetch fails configure with CMake's download error naming the URL; re-run. |

## 6. Testing

**Unit tests (Catch2, run on both platforms by CI)**
- `rig_store_test`: `sanitizeFileName` reserved names (`CON`→`CON_`, `nul`→`nul_`, `com1.tone`→`com1_.tone`,
  `CONCERT`, `LPT10` unchanged, `"con "`→`con_` after trimming) and a save of a rig named `AUX` in band `COM1`
  producing `Rigs/COM1_/AUX_.amprig`.
- `control_input_test`: `RawInputKeys` table (`0x1E`→`0x04`, `0x64/0x65/0x66`→`0x68/0x69/0x6A`, `E0 0x48`→`0x52`,
  `0x00`→nullopt), VK fallback (`VK_F13 0x7C`→`0x68`, `VK_A 0x41`→`0x04`), device path parsing (upper/lower case,
  missing ids → nullopt). Existing tests unchanged.
- `audio_io_test`: with two fake types (`FakeType` gains a type-name parameter, default unchanged) and an empty saved
  type: the preference list picks `ASIO` when it lists a device, `Windows Audio (Low Latency Mode)` when ASIO lists
  none, and the first type with devices when no preferred type qualifies; an empty preference list reproduces today's
  behaviour. `deviceTypes()` lists only types with devices.
- `bridge_test`: `getSettings` carries `availableDeviceTypes` and `hasControlPanel`; `openAudioControlPanel`
  answers `ok` without a panel.
- The whole existing suite (257) compiles and passes under MSVC in Debug, including the allocation-guard tests.

**UI tests (vitest)**: `settings.test.ts`: Driver select rendered only with more than one type, selection sends
`{audioDeviceType, audioDeviceName: ""}`; Control panel button rendered only with `hasControlPanel` and sends
`openAudioControlPanel`.

**CI as proof**: a green `windows` job is success criterion 1. The Debug suite run on Windows is the only automated
proof that `RtSafety` works under MSVC.

**On the Windows PC (manual checklist, new "Windows" section)**: zip unpacks and runs with no VC++ redistributable;
WebView2 dialog appears when the runtime is renamed away (optional check); Stage view renders, Settings opens; Driver
shows ASIO + WASAPI types, Scarlett via ASIO plays, latency at 64/128 compared with the Mac numbers; Control panel…
opens the Focusrite panel and a buffer change rebuilds without a restart; WASAPI Low Latency plays when ASIO is
deselected; footswitch L/M/R assign and toggle with Notepad in front; F13–F15 pedals type nothing visible; a rig saved
on the Mac (copied into `Documents\AMPerator`) loads with the same sound; a rig named `CON` saves and reloads; dev
server (`set AMP_UI_DEV=1`) works in a Debug build; `/art/` images (plugin snapshots, user art) show; a VST3 editor
opens; quit is clean.

## 7. Decisions and alternatives considered

1. **ASIO headers from JUCE, not a configure-time Steinberg download.** JUCE 8 ships the three headers under a
   Steinberg-or-GPLv3 choice, and GPLv3 is compatible with this AGPLv3 project, so a download would add a fragile
   dependency (Steinberg's SDK zip sits behind a click-through agreement whose URL has changed before, and offline
   or cached builds would break) for nothing. The brief asked for the download; this spec deliberately departs from
   it and flags the point for review (§8). A `JUCE_ASIO_USE_EXTERNAL_SDK` path for a user-supplied SDK directory is
   a two-line addition if ever wanted; not built now.
2. **Raw Input for the footswitch.** hidapi reads are impossible for keyboard-class devices on Windows (§2). A
   low-level keyboard hook (`WH_KEYBOARD_LL`) sees keystrokes but not which device sent them, so an ordinary keyboard
   would toggle pedals; WinUSB/libusb would need the user to replace the pedal's driver. Raw Input identifies the
   device, works in the background with `RIDEV_INPUTSINK`, and needs no driver or permission.
3. **WebView2 SDK fetched as a pinned archive by CMake**, not installed with NuGet/PowerShell at configure time
   (JUCE's suggested route depends on per-user package folders and tooling absent on a bare PC) and not vendored into
   the repository (10 MB static library, Microsoft license terms).
4. **Static CRT** so the zip runs on a PC without the VC++ redistributable; hosted VST3 plugins carry their own CRT
   and ASIO drivers are COM servers, so nothing crosses the boundary.
5. **DirectSound off**: it duplicates every WASAPI device with worse latency and would clutter the Driver picker.
6. **Generator chosen by CMake** (`-A x64`, no `-G`) so the same commands work with Visual Studio 2022 on the user's
   PC and 2026 on `windows-latest`.
7. **Debug tests plus Release artifact on Windows CI** at the cost of compiling JUCE twice; Release-only CI would
   leave the allocation guard untested on MSVC, which is the one piece of Windows behaviour nobody can check by ear.
8. **x64 only**; ARM64 Windows waits for a user who has one.
9. **`sanitizeFileName` reserved names applied on every platform**, so a library moved from Mac to PC never meets an
   unopenable file.

## 8. Assumptions to confirm at review

- A. Dropping the Steinberg download in favour of JUCE's bundled, GPLv3-optioned headers (§7.1) is acceptable.
- B. A Windows CI job of 30–40 minutes per push is acceptable (the cache task shortens the dependency fetch, not the
  compile).
- C. The two small Settings additions (Driver picker, Control panel… button), both invisible on macOS, are in scope.
- D. The Raw Input approach means the footswitch on Windows is always "shared"; the F13–F15 reprogramming tip is the
  accepted answer, as it already is for shared mode on macOS.
- E. Zip naming `AMPerator-0.1.0-windows-x64.zip`; no installer or signing in this sub-project.

## 9. Risks

| Risk | Mitigation |
|---|---|
| MSVC rejects or double-counts the global `operator new` replacement (link conflict with the CRT's `new_scalar.obj`, or the DLL-CRT variant) | Static CRT removes the DLL variant; the Debug test on CI shows a conflict on the first run; fallback `_CrtSetAllocHook` keeps the same `ScopedNoAlloc` interface. |
| `windows-latest`'s Visual Studio 2026 compiles something JUCE 8.0.15 or Eigen dislikes | Pin `runs-on: windows-2022` for that job (documented switch), or bump JUCE within 8.0.x. |
| Raw Input key codes differ from the table for some pedal firmware | The VK fallback catches letters/digits/F-keys; `Settings → Last key` shows what arrives, and the assign flow adopts any code. |
| `RIDEV_DEVNOTIFY` misses an arrival (observed on some systems) | A 2 s `juce::Timer` rescan is a five-line addition to the Windows reader if the PC check shows it is needed. |
| WebView2 resource-provider URL shape differs from WKWebView's (the `/art/` prefix test, `index.html` mapping) | Both go through JUCE's `getResourceProviderRoot()`; the PC checklist covers UI load, art and the dev server; a fix would live in `uiResource`/`artResource` only. |
| The Focusrite ASIO driver ignores host buffer-size requests | Control panel… button (§4.2); the reported latency follows the actual buffer size. |
| First CI run reveals a cascade of MSVC compile errors | The plan starts with an "MSVC compile sweep" task driven by the CI log; `AMP_BUILD_APP=OFF` lets the engine and tests compile before the UI toolchain is involved. |
