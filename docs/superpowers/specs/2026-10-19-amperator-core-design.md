# AMPerator — Core Rig Engine Design (Sub-project 1)

**Date:** 2026-10-19
**Status:** Draft for review
**License intent:** Open source (AGPLv3, to match JUCE's open-source license)

## 1. Purpose

A standalone guitar "rig player" that sits between Neural Amp Modeler (great amp tones, but just a single loader) and Ableton (can do everything, but too heavy to open just to jam). You open it, it loads your last rig, and you play.

**The user said:**
- Load NAM amp models and cab IRs.
- Save rigs by band (e.g. Led Zeppelin, Bush, Pink Floyd).
- Ableton-style FX chains, including third-party plugins (VST3/AU).
- USB devices (a 3-pedal USB footswitch) toggle pedals in the chain.
- Later: built-in search of public model/IR libraries, and an AI that builds a rig from a song name ("Interstate Love Song by STP").
- Mac first, then open-sourced so Windows users can use it.

**User's setup:** Apple Silicon Mac (macOS 27), Focusrite Scarlett Solo, 2.1 desktop monitors, generic 3-pedal USB HID footswitch (shows up as a keyboard).

**Success criteria for this sub-project:**
1. From launch to playing the last rig takes one click or none, with round-trip latency at or under ~10 ms on the Scarlett Solo (64–128 sample buffer).
2. You can build a chain from NAM models, IRs, built-ins and VST3/AU plugins, then save it as a rig under a band and reload it exactly.
3. Each of the 3 footswitches can be learned onto any node's bypass and works while the app is in the background.
4. A rig opened on a machine missing an asset or plugin still loads and plays, with the missing parts bypassed and their state kept.

## 2. Scope and decomposition

| # | Sub-project | Status |
|---|---|---|
| 1 | **Core rig engine** (this doc) | Designing |
| 2 | Advanced foot control (tap/long-press, scenes, expression, per-rig layouts) | Later |
| 3 | Tone library: TONE3000 search/download in the app | Later |
| 4 | AI tone builder: song → rig recipe → matched assets → saved rig | Later |
| 5 | Windows build and release (ASIO) | Later; the code stays cross-platform from day 1 |

Sub-projects 3 and 4 are UI and network features on top of the engine. This design leaves room for them (asset source IDs, a library panel slot) but builds nothing for them yet.

**Out of scope for v1:** parallel routing in the UI, expression pedals, recording/looping, running plugins in a separate process, built-in drive/delay/reverb DSP, presets for individual plugins.

## 3. Architecture

**Approach:** a C++ audio engine on **JUCE 8**, with a **web-view UI** built into the same app (JUCE `WebBrowserComponent` plus its native function bridge).

```
┌────────────────── AMPerator.app ──────────────────┐
│  UI (HTML/TS in WebView)                           │
│    Stage view · Edit view · Library panel · Settings│
│            ▲ state events      │ commands          │
│            │   (JSON bridge)   ▼                   │
│  Engine (C++/JUCE, headless-capable)               │
│    AudioIO · ChainGraph · NodeTypes · PluginHost   │
│    RigStore · AssetIndex · ControlInput · Mapping  │
└────────────────────────────────────────────────────┘
```

- **The engine never depends on the UI.** It can run headless for tests.
- **The bridge protocol** is small and versioned. The UI sends commands like `loadRig`, `setParam`, `toggleBypass`, `addNode`, `moveNode`, `removeNode`, `learnMapping` and `saveRig`. The engine sends events like `rigLoaded`, `paramChanged`, `bypassChanged`, `meter`, `tuner`, `deviceStatus`, `controlEvent` and `error`.
- **Dependencies:** JUCE 8 (AGPLv3), NeuralAmpModelerCore (MIT), the VST3 SDK (MIT, via JUCE), hidapi (BSD) and Eigen (MPL2, used by NAM). The build uses CMake.

### 3.1 Engine units

| Unit | Responsibility | Depends on |
|---|---|---|
| `AudioIO` | Device selection, channels, sample rate, buffer size. Detects disconnects and reconnects. | JUCE AudioDeviceManager |
| `ChainGraph` | Nodes plus connections data model. Builds a real-time-safe processing plan and swaps it in atomically. | NodeTypes |
| `NodeTypes` | `Input`, `Output`, `Gate`, `EQ`, `NamAmp`, `CabIR`, `Plugin`, `Tuner` (an analyzer tap that doesn't touch the audio). Every node has bypass, params and a state blob. | NAM core, JUCE DSP |
| `PluginHost` | Scans VST3/AU plugins, keeps a crash blacklist, creates and destroys plugin instances, opens editor windows, captures snapshots. | JUCE AudioPluginFormatManager |
| `RigStore` | Reads and writes `.amprig`, manages the band/rig tree, recents, favorites and last rig. | AssetIndex |
| `AssetIndex` | Indexes model and IR folders (owned and watched) by content hash and metadata. Resolves references. | filesystem |
| `ControlInput` | hidapi footswitch listener (exclusive grab) and MIDI input. Turns both into `ControlEvent`s. | hidapi, JUCE MIDI |
| `MappingEngine` | Maps ControlEvent → action (v1 action: toggle a node's bypass). Holds the learn state. | ChainGraph |

## 4. Signal chain and audio

**The default chain for a new rig:**
```
Input (device ch.1, mono) → Gate → [pre FX…] → NamAmp → CabIR → [post FX…] → Output (stereo)
```

- **Amp and cab are ordinary node types.** They go in the usual places by default but can be moved, removed or duplicated (e.g. two NAM models in a row).
- **The data model is a graph** (nodes plus connections). The v1 UI only creates and shows a single in-series chain. Parallel routing comes later with no file format change.
- **Mono to stereo:** processing is mono up to the first stereo-capable node. Mono is copied to both channels at the output if nothing upstream is stereo.
- **NamAmp:** loads a `.nam` file and has input gain, output gain and the model's loudness normalization (on by default). Models are loaded and resampled off the audio thread.
- **CabIR:** loads a `.wav` IR, resampled to the device rate, and runs partitioned convolution (JUCE `Convolution`). Has low-cut, high-cut and level.
- **Real-time safety:** all loading (rigs, models, IRs, plugins) happens on a background thread. The new processing plan is swapped in on the audio thread with a ~30 ms equal-power crossfade. The audio thread never allocates memory or takes a lock.
- **Global device routing** lives in settings, never in a rig. It defaults to the first device input → outputs 1+2.

## 5. Rigs, bands and the asset library

**Organization:** Library → Band (a collection) → Rig. A rig belongs to one band and can have free-form tags. A default band is "My Tones".

**Rig file `.amprig` (JSON, versioned; excerpt, nodes n2/n4 omitted):**
```json
{
  "format": "amperator.rig", "version": 1,
  "id": "uuid", "name": "Interstate Love Song", "band": "STP",
  "tags": ["90s", "clean-to-crunch"],
  "nodes": [
    { "id": "n1", "type": "Gate", "bypass": false, "params": { "threshold": -55 } },
    { "id": "n3", "type": "NamAmp", "bypass": false,
      "asset": { "kind": "nam", "file": "Mesa Rect Ch2.nam",
                 "sha256": "…", "source": { "provider": "tone3000", "id": "12345" } },
      "params": { "inputGain": 0, "outputGain": -3, "normalize": true },
      "art": { "archetype": "high-gain-rectifier" } },
    { "id": "n5", "type": "Plugin", "bypass": true,
      "plugin": { "format": "VST3", "uid": "…", "name": "ValhallaDelay", "vendor": "Valhalla DSP" },
      "state": "base64…" }
  ],
  "connections": [["in","n1"],["n1","n3"],["n3","n4"],["n4","n5"],["n5","out"]],
  "mappings": [ { "control": "footswitch:L", "action": "toggleBypass", "node": "n2" } ]
}
```

- **Asset resolution order:** sha256 match in the AssetIndex → filename match (with a warning) → "missing". A missing asset or plugin turns its node into a **bypassed placeholder that keeps all its data**, and the UI flags it. `source` lets sub-project 3 fetch missing assets automatically later.
- **On disk:** `~/Music/AMPerator/` (Windows: `Documents\AMPerator\`) containing `Models/`, `IRs/`, `Rigs/<Band>/<Rig>.amprig`, `Art/` and `settings.json`. Users can add **watched folders** (e.g. an existing NAM download folder), which are indexed in place without copying.
- **Global default mappings** are stored in `settings.json`. A rig's own mappings override them for the same control.
- **Saving:** write to a temp file, then atomically rename it into place. Keep the last 5 versions of each rig as `.bak`.

## 6. UI

**Stage mode** (opens on launch with the last rig): band and rig name, an amp → cab line with art thumbnails, three large footswitch lights showing each mapped node's name and on/off state, previous/next rig, an input meter, a tuner toggle, and an Edit button.

**Edit mode** (pedalboard): nodes drawn left to right as pedal, amp and cab cards with bypass lights. Click to show knobs, drag to reorder, **+** to add a built-in or plugin, right-click → "Learn footswitch". Plugin editors open in separate native windows.

**Library panel** (slides out in both modes): a band → rig tree, recents and favorites, plus Models / IRs / Plugins browsers. It has an empty placeholder tab for "Search online" (sub-project 3).

**Settings:** audio device, input channel, output pair, sample rate, buffer size (with the measured latency shown), watched folders, and footswitch/MIDI device status.

**Node art (first available is used):**
1. A user-assigned image (stored locally in `Art/`, not shared in the rig).
2. The image from the asset's provider page (once sub-project 3 exists), credited to the uploader.
3. A snapshot of the plugin's editor (Plugin nodes).
4. Built-in, project-drawn **generic archetype art with no brand names or logos**: amps (british-stack, american-clean-combo, high-gain-rectifier, ac-chime, generic), cabs (4x12, 2x12, 1x12-open), pedals (drive, fuzz, delay, chorus, reverb, eq, gate, generic). The archetype comes from asset tags or the user's choice.

## 7. Control input

- **The HID footswitch** is opened with hidapi and **grabbed exclusively** where the OS allows it, so presses don't type into other apps and keep working while the app is in the background. Each key code maps to a stable control ID. A 3-pedal board defaults to `footswitch:L/M/R`, learned from the first press of each pedal during setup.
  - macOS: IOHIDManager seize via hidapi. The OS may require Input Monitoring permission, so the app prompts with guidance.
  - Windows (later): use Raw Input to tell the device apart. Exclusive grab may not be possible, so documented fallbacks apply.
- **MIDI input:** any CoreMIDI/WinMM device. Notes and CCs become `midi:<dev>:cc<n>` / `note<n>`.
- **Mapping in v1:** one action, `toggleBypass(node)`. Learn flow: choose a node → Learn → press any control → saved in the rig. Events are delivered to the engine thread, never to the audio thread.

## 8. Error handling

| Situation | Behavior |
|---|---|
| Audio device unplugged | A banner appears and the output is silent. Reconnects automatically when the same device comes back, and settings are kept. |
| Missing model, IR or plugin | Bypassed placeholder node that keeps its data. A notice lists what's missing. |
| Plugin crashes on load or scan | A crash marker is written before risky calls. On the next launch, the plugin is blacklisted with an option to retry. |
| Corrupt or unknown-version rig file | An error toast appears and the rig isn't loaded. The file is never overwritten. The last backup is offered. |
| Footswitch permission denied | The Settings panel shows a status and steps to fix it. The app works without the footswitch. |
| CPU overload (xruns) | An xrun counter appears in settings with a hint to raise the buffer size. |

## 9. Testing

- **Unit (engine, headless):** rig read/write round trip, version handling, graph edits and plan building, asset resolution (hash, then filename, then missing), mapping and learn logic, settings defaults.
- **Offline audio regression:** render bundled DI clips through reference rigs (built-ins + NAM + IR, no third-party plugins) and compare against stored outputs within a tolerance.
- **Real-time safety:** a debug-build check that fails on memory allocation or locking during the audio callback (a hook on allocations in the audio thread).
- **Control input:** a fake HID/MIDI source feeds events. Tests check routing, learn and override precedence.
- **Manual checklist:** latency on the Scarlett Solo at 64/128 samples, footswitch working while the app is in the background, rig switching with no clicks, reconnecting after a device unplug.

## 10. Open questions to settle during planning

- Exact built-in archetype art style (to mock up when building the UI).
- Whether v1 includes a simple built-in boost/drive so a new user without plugins gets a full rig. (Leaning yes, as a small stretch goal.)
