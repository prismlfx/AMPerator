# AMPerator

**A standalone guitar rig player: NAM amp models, cab IRs, your VST3/AU plugins, and a USB footswitch — without opening a DAW.**

AMPerator fills the gap between [Neural Amp Modeler](https://www.neuralampmodeler.com/) (great tones, but just an amp loader) and a full DAW (does everything, but too heavy to open when you just want to jam). You open it, it loads the rig you played last, and you play.

- **Amps and cabs:** load any `.nam` capture and any `.wav` cabinet impulse response.
- **FX chains:** gate, EQ, and any VST3 or Audio Unit plugin you already own, in any order, with a master level.
- **Rigs by band:** save a chain as *Led Zeppelin › Black Dog* or *STP › Interstate Love Song* and flip between them.
- **Footswitch:** a cheap 3-pedal USB footswitch (or any MIDI controller) toggles pedals, Ableton-style "learn".
- **Stage mode:** a big, readable jam screen with footswitch tiles, an input meter and a tuner.
- **Low latency:** ~10 ms round trip on a Focusrite Scarlett at a 128-sample buffer; lower at 64.

Open source under the AGPLv3. macOS today; the code is cross-platform (JUCE 8) and a Windows build is planned.

## Screenshots

**Stage mode** — what the app opens into. Band and rig name, amp → cab, three footswitch tiles, prev/next rig, meter, tuner.

![Stage mode](docs/screenshots/stage.png)

**Edit mode** — the pedalboard. Click a card for its knobs, drag to reorder, `+` to add a built-in or a plugin.

![Edit mode](docs/screenshots/edit.png)

**Settings** — audio device, channels, buffer size with the live latency readout, watched folders, footswitch and MIDI.

![Settings](docs/screenshots/settings.png)

## Status

Sub-project 1 (the core rig engine) is complete: 261 C++ tests and 139 UI tests pass, including an offline audio regression suite. Real-hardware checks are tracked in [docs/manual-test-checklist.md](docs/manual-test-checklist.md). Planned next:

1. **AI tone builder** — "I want to play Interstate Love Song by STP" → the app finds the closest models, builds the chain, and you save it.
2. **Advanced foot control** — tap/long-press, scenes (clean / crunch / lead per song), expression pedals.
3. **Windows build** (ASIO).

## Build

Requirements: macOS with Xcode (Apple clang, C++20), CMake 3.24+, Node 20+ (for the web UI). Every C++ dependency is fetched and pinned by CMake; the first JUCE fetch takes a few minutes.

```sh
cmake -B build -G Xcode
cmake --build build --target AMPerator      # the app → build/app/AMPerator_artefacts/Debug/AMPerator.app
open build/app/AMPerator_artefacts/Debug/AMPerator.app
```

Tests:

```sh
cmake --build build --target amp_tests && ctest --test-dir build -C Debug   # C++ engine (Catch2)
(cd ui && npm install && npx vitest run)                                     # web UI (vitest)
```

Set `-DAMP_BUILD_APP=OFF` to build only the engine and tests (no Node needed). In a Debug build, `AMP_UI_DEV=1` loads the UI from a Vite dev server at `localhost:5173` (`cd ui && npm run dev`) for hot reload.

## Using it

### Your library

Everything lives in `~/Music/AMPerator/` (Windows: `Documents\AMPerator\`):

| Folder | What goes there |
|---|---|
| `Models/` | NAM amp captures (`.nam`) |
| `IRs/` | cabinet impulse responses (`.wav`, `.aiff`) |
| `Rigs/<Band>/` | saved rigs (`.amprig`, plain JSON) plus their last 5 backups |
| `Art/` | images you assign to pedals, amps or cabs |
| `settings.json` | audio device, footswitch keys, global mappings, recents |

Drop files into `Models/` and `IRs/`, or point Settings → *Watched folders* at the folder where you already keep your NAM downloads; nothing is copied. Rigs reference their files by content hash, so you can rename or move files freely. A rig that references a model or plugin you don't have still loads — the missing node is shown as a bypassed placeholder with its settings intact, and you can **Locate…** the file.

### First launch

1. macOS asks for **microphone** access (that's your audio interface input). Allow it.
2. Open Settings (gear icon), pick your interface, input channel, output pair and buffer size. The round-trip latency is shown live.
3. Switch to **Edit**, click the NAM Amp card to pick a model and the Cab IR card to pick an IR, then **Save** under a band name.

### Rigs

- **Save** keeps the last 5 versions as `.bak` files beside the rig; **Save as…** makes a copy under a new name.
- A rig file that gets corrupted shows an error with an **Open last backup** action instead of silently disappearing.
- Quitting or switching rigs with unsaved changes asks first.

**Tags.** Give a rig as many free-form tags as you like — a style or decade (`clean`, `70s`), and the bands and songs you use it for (`Parkway Drive`, `Wild Eyes`) — so a rig called *Peavey 5150 - Djent* is still easy to find without renaming it. Add them in Edit, in the *Tags* row above the pedalboard (Enter or comma adds one, ✕ removes one, known tags are suggested as you type); they are saved with the rig and shown under its name on the Stage. In the library, the search field matches rig names, bands and tags, and the tag bar below it filters by one or more tags (all must match). `⌘F`, or `/`, jumps to the search.

### Plugins

`+` → *Plugins…* lists your VST3 and AU plugins after a scan (the *Rescan* button in that picker; the scan runs in the background with a progress readout). The picker sorts them into groups (Drive, Amps & Cabs, Modulation, Delay, Reverb, Dynamics, EQ, Filter & Wah, Pitch, Utility & Other, Instruments) from each plugin's declared category and name, with a search field and a **Guitar** toggle (on by default, remembered) that hides the utilities and instruments. The scan loads each plugin in a separate helper process (the app relaunched in scanner mode), so a plugin that crashes or hangs while it is scanned never takes the app down: it gets one more try in a fresh helper, and if it fails that too it is switched off (the scan carries on) and shows up in Settings → *Disabled plugins* with a **Retry**. A plugin that crashes later, while it is loaded into a rig, is disabled on the next launch the same way. Each plugin node has an **Open editor** action for its own window.

If a scan misbehaves, run the same scan from a terminal and watch it: `AMPerator.app/Contents/MacOS/AMPerator --scan-plugins` prints each plugin as it is scanned (with `AMP_SCAN_TRACE=1` in the environment, also every worker launch, crash and retry).

### Footswitch and MIDI

AMPerator reads a USB footswitch directly over HID so pedal presses control the rig even when another app is in front. Supported out of the box: the common 3-pedal boards (PCsensor / iKKEGOL style) that act as a keyboard; any other HID keyboard-type device can be chosen manually in Settings → *Footswitch & MIDI* → *Use as footswitch*.

- macOS asks for **Input Monitoring** permission the first time. Grant it in System Settings → Privacy & Security → Input Monitoring, then relaunch.
- The app tries to take the pedal **exclusively** (presses don't reach other apps). If macOS won't allow that, Settings shows *shared* mode; reprogram the pedal to send **F13 / F14 / F15** with its vendor tool so it types nothing visible.
- If your pedal sends keys other than the defaults, use **Assign L / M / R**: click, stomp, done. The dialog shows the last key code it saw.
- Mapping a pedal to an effect: in Edit, right-click a card → **Learn footswitch**, then stomp. Mappings are saved in the rig; global defaults live in settings.
- Any MIDI controller works the same way (notes and CCs).

### TONE3000

AMPerator can browse, download and restore amp models and IRs from [TONE3000](https://www.tone3000.com) without leaving the app. The library's **TONE3000** tab lists your favorites, downloads, creations, and what is trending or new; **Browse TONE3000** opens TONE3000's own tone picker (with audio previews) in a window inside AMPerator; picking a tone shows its models and a **Download** that puts the file into your library and, from a node's menu, straight onto that node (**Browse TONE3000…** / **Replace from TONE3000…**).

- **Sign-in:** **Connect TONE3000** opens TONE3000's sign-in page in an in-app window: your email, then a one-time code they send you. AMPerator never sees a password; the window closes itself when you are in. The resulting tokens are kept in the macOS Keychain (service `com.amperator.tone3000`), never in a file. **Disconnect** (in Settings or on the TONE3000 tab) signs out and clears the window's session, so the next sign-in asks for the email again.
- **Where files land:** `Models/TONE3000/<creator>/<tone title> — <model name>.nam` and `IRs/TONE3000/<creator>/<tone title> — <model name>.wav` inside your library, as ordinary files you can move, rename or delete. Nothing is downloaded until you press Download (or the auto-download setting below fires); lists are never saved to disk.
- **Attribution and licenses:** the creator, the license TONE3000 shows for the tone and a link back to its page are kept with every downloaded file (`tone3000/index.json` in the app's data folder) and shown with it in the library and on the node.
- **Missing models:** a rig stores a reference to the TONE3000 model next to its file hash. If the file is gone, the rig still loads (the node is a bypassed placeholder) and a banner says `N models from TONE3000 are missing.` with a **Download** button that restores them. Settings → *Download missing TONE3000 models automatically when opening a rig* does the same on its own when a rig loads, only while you are signed in; it is off by default.
- **Using it for free:** AMPerator uses TONE3000's free-tier API for non-commercial open-source use. Models remain the property of their creators and are used under the license shown with each tone.

**Building with TONE3000 enabled.** The integration needs a TONE3000 publishable key (OAuth client id), compiled in:

```sh
cmake -B build -G Xcode -DAMP_TONE3000_CLIENT_ID=t3k_pub_...
```

Without it the build works as before and the TONE3000 tab says the build has no client id (every `tone3000*` bridge command answers `not configured`). Create a key in your own TONE3000 account under Settings → API Keys. The publishable key (`t3k_pub_…`) is public by design and safe to ship; never use the secret key anywhere in this project. A developer can also put a key in `settings.json` as `tone3000ClientId`, which overrides the compiled-in one the next time the app starts.

**Live tests.** The TONE3000 tests run offline against fakes. The few tests that reach the real service (unauthenticated GET requests to www.tone3000.com only; they never sign in) are skipped unless you opt in: `AMP_NETWORK_TESTS=1 ctest --test-dir build -C Debug` (needs network access).

Powered by [TONE3000](https://www.tone3000.com).

### Keyboard shortcuts (Stage)

`←` / `→` previous / next rig · `T` tuner · `E` edit.

## How it's built

```
engine/   headless C++ library (JUCE 8): rig model + graph edits, content-hash asset index,
          DSP nodes (gate, EQ, NAM amp with host-rate resampling, cab IR convolution),
          real-time processing plan with crossfaded swaps and click-free bypass,
          VST3/AU hosting with crash blacklist, rig store with atomic saves + backups,
          audio device wrapper with auto-reconnect, HID footswitch + MIDI, mapping/learn,
          YIN tuner, and a versioned JSON bridge for the UI
app/      the macOS app: a JUCE window hosting a WKWebView, plugin editor windows
ui/       the interface: Vite + TypeScript, no framework (Stage, Edit, library, settings)
tests/    Catch2 suites for every engine unit + offline audio regression renders
docs/     bridge protocol, manual hardware checklist, design specs and plans
```

Design principles: the audio thread never allocates, locks or touches a file (a debug-build guard proves it); everything that can fail — a missing model, a crashing plugin, a corrupt rig file, an unplugged interface — degrades to something you can still play through. The UI talks to the engine only through the JSON protocol in [docs/bridge-protocol.md](docs/bridge-protocol.md), so the engine runs headless under test.

### Audio regression references

`tests/fixtures/reference/*.wav` are offline renders of reference rigs. After an intentional DSP change, regenerate them with

```sh
AMP_UPDATE_REFERENCES=1 ctest --test-dir build -C Debug -R audio_regression
```

That run deliberately reports a failure after writing the files, so it can never pass by comparing against itself. Listen to the new WAVs, commit them, and rerun without the variable.

## License

AMPerator is licensed under the [GNU AGPLv3](LICENSE). It builds on JUCE (AGPLv3), NeuralAmpModelerCore (MIT), hidapi (BSD) and Eigen (MPL-2.0); see [THIRD_PARTY.md](THIRD_PARTY.md). The amp, cab and pedal artwork is generic and original — no manufacturer names or logos are used, and model names shown in the app are the names of the files you load.
