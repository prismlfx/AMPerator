# AMPerator Core Rig Engine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A standalone macOS app (cross-platform code) that plays NAM amp + cab IR + VST3/AU chains with low latency, saves rigs by band, and lets a USB footswitch toggle pedals.

**Architecture:** A headless C++ engine library (`amp_engine`) holds everything: rig model, graph, DSP nodes, plugin hosting, storage, control input and a JSON command bridge. A thin JUCE app target (`AMPerator`) hosts the engine and a WebBrowserComponent that loads a Vite/TypeScript UI built into app resources. All engine logic is tested headless with Catch2.

**Tech Stack:** C++20, CMake ≥ 3.25, JUCE 8 (AGPLv3), NeuralAmpModelerCore (MIT), hidapi (BSD), Catch2 v3, JSON via `juce::var`/`juce::JSON` (no extra JSON library), Vite + TypeScript (no UI framework) for the web UI.

**Spec:** `docs/superpowers/specs/2026-10-19-amperator-core-design.md`

## Global Constraints

- Language/standard: C++20; namespace `amp`; engine sources under `engine/`, app under `app/`, UI under `ui/`, tests under `tests/`.
- Dependencies via CMake `FetchContent`, each pinned to an exact tag or commit recorded in `cmake/Dependencies.cmake`: JUCE newest `8.0.x` tag, NeuralAmpModelerCore a specific commit on `main`, hidapi `hidapi-0.15.0`, Catch2 `v3.8.1`.
- License: AGPLv3 — `LICENSE` at repo root; third-party notices in `THIRD_PARTY.md`.
- Latency target: round-trip ≤ ~10 ms on Scarlett Solo at 64–128 sample buffer.
- Audio thread: no allocation, no locks, no file I/O. Loading happens off-thread; plan swaps crossfade ~30 ms equal-power.
- Rig file: `.amprig` JSON, `"format": "amperator.rig"`, `"version": 1`.
- Library root: `~/Music/AMPerator/` (Windows `Documents\AMPerator\`), subfolders `Models/`, `IRs/`, `Rigs/<Band>/`, `Art/`, plus `settings.json`. Default band `"My Tones"`.
- Rig saves: temp file + atomic rename; keep last 5 `.bak` per rig.
- Device routing lives only in `settings.json`, never in a rig. Default: first input channel → outputs 1+2.
- Footswitch control IDs `footswitch:L`, `footswitch:M`, `footswitch:R`; MIDI IDs `midi:<device>:cc<n>` / `midi:<device>:note<n>`. v1 action only `toggleBypass`.
- Built-in art: no brand names or logos anywhere in shipped assets or archetype IDs.
- Out of scope: parallel routing UI, expression pedals, recording, out-of-process plugins, built-in drive/delay/reverb, TONE3000 search, AI.

## Review Focus

1. **Device rate ≠ model rate** (e.g. Scarlett at 44.1 kHz, NAM model trained at 48 kHz) — output must be correctly resampled, not pitch-shifted. → Task 6 test `NamAmp resamples to host rate`.
2. **Band/rig names with filesystem-illegal characters** (`AC/DC`, `Guns N' Roses: Live?`) — display name kept exactly, file path sanitized, no crash or stray subfolder. → Task 10 test `illegal characters sanitized`.
3. **Holding a footswitch** (HID keyboards auto-repeat while held) — one toggle per physical press, not a stream. → Task 13 test `auto-repeat collapses to one press`.
4. **Pedal mapped to a node that was deleted / pedal with no mapping** — press is a no-op, no crash, mapping dropped on next save. → Task 14 test `stale mapping is ignored and pruned`.
5. **Rapid rig switching while playing** (mashing next-rig) — no crash, no leaks, ends on the last requested rig, no output spike above input-derived level. → Task 8 test `rapid plan swaps settle on last plan`.

---

## File Structure

```
CMakeLists.txt                     root: options, adds engine/, app/, tests/
cmake/Dependencies.cmake           FetchContent pins
engine/
  CMakeLists.txt                   amp_engine static lib
  model/RigDocument.{h,cpp}        Rig data types + JSON (de)serialize
  model/ChainGraph.{h,cpp}         graph edits, linear-chain helpers, topo order
  assets/AssetIndex.{h,cpp}        folder scan, sha256, resolve
  dsp/Node.h                       ProcessorNode interface
  dsp/BuiltinNodes.{h,cpp}         Gate, EQ, Output gain, Placeholder
  dsp/NamAmpNode.{h,cpp}
  dsp/CabIRNode.{h,cpp}
  dsp/TunerTap.{h,cpp}
  engine/ProcessingPlan.{h,cpp}    compiled RT-safe chain
  engine/RealtimeEngine.{h,cpp}    audio callback, plan swap/crossfade, meters
  engine/RtSafety.{h,cpp}          debug allocation guard
  plugins/PluginHost.{h,cpp}       scan, blacklist, create, state
  storage/RigStore.{h,cpp}         library tree, atomic save, backups
  storage/Settings.{h,cpp}         settings.json
  storage/RigLoader.{h,cpp}        RigDocument <-> live nodes
  audio/AudioIO.{h,cpp}            device mgmt, reconnect, xruns
  control/ControlEvent.h
  control/HidFootswitch.{h,cpp}
  control/MidiControl.{h,cpp}
  control/MappingEngine.{h,cpp}
  bridge/Bridge.{h,cpp}            command dispatch + event emit
  Engine.{h,cpp}                   facade wiring all units
app/
  CMakeLists.txt, Main.cpp, MainWindow.{h,cpp}, PluginWindow.{h,cpp}
ui/
  package.json, vite.config.ts, index.html
  src/bridge.ts, src/state.ts, src/main.ts
  src/views/stage.ts, edit.ts, library.ts, settings.ts
  src/art/archetypes.ts, src/art/*.svg
tests/
  CMakeLists.txt, *_test.cpp, fixtures/ (DI clips, tiny .nam, IR .wav, rigs)
docs/manual-test-checklist.md
```

---

### Task 1: Build scaffold

**Files:**
- Create: `CMakeLists.txt`, `cmake/Dependencies.cmake`, `engine/CMakeLists.txt`, `engine/Engine.{h,cpp}`, `app/CMakeLists.txt`, `app/Main.cpp`, `tests/CMakeLists.txt`, `tests/smoke_test.cpp`, `LICENSE`, `THIRD_PARTY.md`, `README.md`
- Modify: `.gitignore` (add `ui/node_modules/`, `ui/dist/`)

**Interfaces:**
- Produces: CMake targets `amp_engine` (static lib, links juce_audio_utils, juce_dsp, juce_audio_processors), `AMPerator` (juce_add_gui_app, `MICROPHONE_PERMISSION_ENABLED TRUE`, bundle id `org.amperator.app`), `amp_tests` (Catch2, registered with CTest). `amp::engineVersion() -> juce::String` returning `"0.1.0"`.

- [ ] **Step 1: Write the failing test** `tests/smoke_test.cpp`: `TEST_CASE("engine version") { REQUIRE(amp::engineVersion() == "0.1.0"); }`
- [ ] **Step 2: Configure and build; expect failure** — Run: `cmake -B build -G Xcode && cmake --build build --target amp_tests` → link error, `engineVersion` undefined.
- [ ] **Step 3: Implement** `engineVersion()` in `engine/Engine.cpp`; `app/Main.cpp` opens an empty 1100×700 DocumentWindow titled "AMPerator". Pin deps in `Dependencies.cmake`.
- [ ] **Step 4: Verify** — `cmake --build build --target amp_tests && ctest --test-dir build -C Debug` → 1 passed. `cmake --build build --target AMPerator` → app launches with empty window.
- [ ] **Step 5: Commit** — `git add -A && git commit -m "build: CMake scaffold with JUCE, Catch2, app and engine targets"`

---

### Task 2: Rig document model + JSON

**Files:** Create `engine/model/RigDocument.{h,cpp}`; Test `tests/rig_document_test.cpp`; fixture `tests/fixtures/rigs/interstate.amprig` (the spec §5 example, filled out with n1–n5 + `in`/`out`).

**Interfaces:**
- Produces:
  ```cpp
  namespace amp {
  enum class NodeType { Input, Output, Gate, EQ, NamAmp, CabIR, Plugin, Placeholder };
  struct AssetSource { juce::String provider, id; };
  struct AssetRef   { juce::String kind /*"nam"|"ir"*/, file, sha256; std::optional<AssetSource> source; };
  struct PluginRef  { juce::String format, uid, name, vendor; };
  struct NodeDoc    { juce::String id; NodeType type; bool bypass=false;
                      juce::NamedValueSet params; std::optional<AssetRef> asset;
                      std::optional<PluginRef> plugin; juce::MemoryBlock state;
                      juce::String artArchetype; juce::var extra; /* unknown keys preserved */ };
  struct Mapping    { juce::String control, action, node; };
  struct RigDocument{ juce::String id, name, band; juce::StringArray tags;
                      std::vector<NodeDoc> nodes; std::vector<std::pair<juce::String,juce::String>> connections;
                      std::vector<Mapping> mappings; };
  struct ParseResult { std::optional<RigDocument> rig; juce::String error; };
  ParseResult parseRig(const juce::String& json);
  juce::String serializeRig(const RigDocument&);
  RigDocument makeDefaultRig(const juce::String& name, const juce::String& band); // in→Gate→NamAmp(empty)→CabIR(empty)→out
  }
  ```
  `Placeholder` is never written; a placeholder serializes as its original type with all original fields (stored in `extra`).

- [ ] **Step 1: Write failing tests**: `round trip preserves all fields` (parse fixture → serialize → parse, compare every field incl. base64 state bytes); `rejects wrong format` (`"format":"other"` → `rig` empty, error contains `"format"`); `rejects newer version` (`"version":2` → error contains `"version"`); `malformed JSON returns error, no throw`; `unknown keys survive round trip` (node with `"futureKey":1` keeps it); `default rig shape` (5 nodes in order Input,Gate,NamAmp,CabIR,Output, 4 connections, ids unique).
- [ ] **Step 2: Run** `ctest --test-dir build -C Debug -R rig_document` → FAIL (undefined).
- [ ] **Step 3: Implement** with `juce::JSON::parse` / `juce::var`; `state` via `MemoryBlock::toBase64Encoding`/`fromBase64Encoding`; ids via `juce::Uuid`.
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(model): rig document JSON with versioning and unknown-field preservation`

---

### Task 3: ChainGraph edits

**Files:** Create `engine/model/ChainGraph.{h,cpp}`; Test `tests/chain_graph_test.cpp`.

**Interfaces:**
- Consumes: `RigDocument`, `NodeDoc` (Task 2).
- Produces:
  ```cpp
  namespace amp::graph {
  std::vector<juce::String> linearOrder(const RigDocument&);            // in..out; empty if not a single chain
  bool isLinearChain(const RigDocument&);
  juce::String insertNode(RigDocument&, NodeDoc, int chainIndex);       // index among non-I/O nodes; rewires; returns id
  bool moveNode(RigDocument&, const juce::String& id, int newChainIndex);
  bool removeNode(RigDocument&, const juce::String& id);                // rewires neighbors; prunes mappings to id
  std::optional<std::vector<juce::String>> topoOrder(const RigDocument&); // nullopt on cycle
  }
  ```

- [ ] **Step 1: Write failing tests**: `insert at 0 goes after input`; `insert past end goes before output`; `move reorders and keeps chain linear`; `remove rewires neighbors and prunes mappings`; `input/output cannot be moved or removed` (return false); `cycle detected` (hand-built doc with a→b→a → `topoOrder` nullopt); `linearOrder of default rig` = in,Gate,NamAmp,CabIR,out ids.
- [ ] **Step 2: Run** `-R chain_graph` → FAIL.
- [ ] **Step 3: Implement** (Kahn's algorithm for `topoOrder`).
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(model): chain graph editing and topological order`

---

### Task 4: AssetIndex

**Files:** Create `engine/assets/AssetIndex.{h,cpp}`; Test `tests/asset_index_test.cpp`; fixtures `tests/fixtures/assets/tiny.nam`, `tests/fixtures/assets/ir_48k.wav` (short mono IR).

**Interfaces:**
- Produces:
  ```cpp
  namespace amp {
  struct AssetEntry { juce::File file; juce::String kind, sha256; juce::int64 size, modTime; };
  enum class ResolveKind { ByHash, ByFilename, Missing };
  struct Resolved { ResolveKind how; juce::File file; };
  class AssetIndex {
  public:
    void setRoots(juce::Array<juce::File> roots);      // owned + watched folders
    void rescan();                                     // recursive; .nam → "nam", .wav/.aiff → "ir"; caches hash by (path,size,modTime)
    Resolved resolve(const AssetRef&) const;           // hash → filename (case-insensitive) → Missing
    std::vector<AssetEntry> list(const juce::String& kind) const;
    static juce::String sha256Of(const juce::File&);   // lowercase hex
  };
  }
  ```

- [ ] **Step 1: Write failing tests**: `resolves by hash after rename` (copy fixture to temp root under new name; resolve with original sha → ByHash, new path); `falls back to filename when hash differs`; `missing when neither`; `list filters by kind`; `rescan picks up new file` ; `unchanged files are not rehashed` (expose a test counter `hashCount()`; second rescan adds 0).
- [ ] **Step 2: Run** `-R asset_index` → FAIL.
- [ ] **Step 3: Implement** with `juce::SHA256`; scan on caller thread (callers put it on a background thread).
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(assets): content-hash asset index with filename fallback`

---

### Task 5: Node interface, built-in nodes, offline render harness

**Files:** Create `engine/dsp/Node.h`, `engine/dsp/BuiltinNodes.{h,cpp}`, `tests/support/OfflineRender.{h,cpp}`; Test `tests/builtin_nodes_test.cpp`.

**Interfaces:**
- Produces:
  ```cpp
  namespace amp {
  struct ProcessSpec { double sampleRate; int maxBlock; };
  class ProcessorNode {                      // prepare/release off audio thread; process on audio thread
  public:
    virtual ~ProcessorNode() = default;
    virtual void prepare(const ProcessSpec&) = 0;
    virtual void process(juce::AudioBuffer<float>& io, int numSamples) = 0;  // io has 2 channels; mono nodes read/write ch0
    virtual bool isStereo() const { return false; }
    virtual void setParam(const juce::String& name, float value) {}          // RT-safe (atomics)
    virtual juce::NamedValueSet getParams() const { return {}; }
    virtual juce::MemoryBlock getState() const { return {}; }
    virtual void setState(const juce::MemoryBlock&) {}
  };
  std::unique_ptr<ProcessorNode> makeGate();        // params: threshold dB (-55), release ms (100)
  std::unique_ptr<ProcessorNode> makeEQ();          // params: low, mid, high dB (0), mid freq Hz (800)
  std::unique_ptr<ProcessorNode> makeOutputGain();  // param: level dB (0)
  std::unique_ptr<ProcessorNode> makePlaceholder(); // passthrough
  }
  // tests/support
  juce::AudioBuffer<float> renderOffline(amp::ProcessorNode&, const juce::AudioBuffer<float>& in, double sr, int block = 128);
  ```
  Bypass is handled by the plan (Task 8), not by nodes.

- [ ] **Step 1: Write failing tests**: `gate silences signal below threshold` (−70 dBFS sine → output RMS < −90 dBFS after 200 ms); `gate passes signal above threshold` (−20 dBFS → within 0.5 dB); `EQ flat is identity` (max abs diff < 1e-5); `EQ +6 dB high shelf boosts 8 kHz by ~6 dB` (±1 dB); `output gain -6 dB halves amplitude` (±0.01); `placeholder is passthrough`; `params are block-size independent` (render with block 64 vs 512, diff < 1e-4).
- [ ] **Step 2: Run** `-R builtin_nodes` → FAIL.
- [ ] **Step 3: Implement** using `juce::dsp::NoiseGate`, `juce::dsp::IIR` shelves/peak, `juce::SmoothedValue` for gain.
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(dsp): node interface, gate, EQ, gain, placeholder`

---

### Task 6: NAM amp node

**Files:** Create `engine/dsp/NamAmpNode.{h,cpp}`; Modify `cmake/Dependencies.cmake`, `engine/CMakeLists.txt` (add NAM core sources + Eigen include); Test `tests/nam_amp_test.cpp`; fixture `tests/fixtures/assets/tiny.nam` (smallest available LSTM/WaveNet test model from NAM core repo, sample rate 48000) and `tests/fixtures/di/di_48k.wav` (5 s guitar DI, mono).

**Interfaces:**
- Consumes: `ProcessorNode` (Task 5).
- Produces:
  ```cpp
  namespace amp {
  class NamAmpNode : public ProcessorNode {
  public:
    struct Loaded;                                                 // opaque: model + resampler state
    static std::unique_ptr<Loaded> loadModel(const juce::File&, juce::String& error); // off audio thread
    void setModel(std::unique_ptr<Loaded>);                        // call before prepare, or via plan rebuild
    double modelSampleRate() const;                                // from model metadata, default 48000
    // params: inputGain dB (0), outputGain dB (0), normalize bool (1) — uses model "loudness" metadata to target -18 dB
  };
  std::unique_ptr<NamAmpNode> makeNamAmp();
  }
  ```

- [ ] **Step 1: Write failing tests**: `loads fixture model` (no error, `modelSampleRate()==48000`); `bad file returns error without throwing`; `processes DI at native rate` (output finite, non-silent, RMS within ±6 dB of −18 dBFS with normalize on); `NamAmp resamples to host rate` (prepare at 44100, feed 440 Hz sine; dominant output frequency within ±2 Hz of 440 via FFT peak); `no model = passthrough`; `processing allocates nothing` (wrap `process` in `RtSafety::ScopedNoAlloc` from Task 8 — mark this test `[!mayfail]` until Task 8 lands, then remove the tag in Task 8).
- [ ] **Step 2: Run** `-R nam_amp` → FAIL.
- [ ] **Step 3: Implement** with `nam::get_dsp(path)`; resample host→model→host using `juce::LagrangeInterpolator` (or `juce::dsp` oversampling when ratio is integer) with buffers sized in `prepare`; call model `prewarm` in `loadModel`.
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(dsp): NAM amp node with host-rate resampling and loudness normalize`

---

### Task 7: Cab IR node

**Files:** Create `engine/dsp/CabIRNode.{h,cpp}`; Test `tests/cab_ir_test.cpp`; fixture `tests/fixtures/assets/ir_48k.wav`.

**Interfaces:**
- Produces: `std::unique_ptr<CabIRNode> makeCabIR();` with `bool loadIR(const juce::File&, juce::String& error)` (off audio thread; uses `juce::dsp::Convolution::loadImpulseResponse` with `Stereo::no, Trim::yes, Normalise::yes`), params `lowCut` Hz (80), `highCut` Hz (8000), `level` dB (0).

- [ ] **Step 1: Write failing tests**: `impulse in yields IR out` (unit impulse, cuts disabled at 20/20000 → output correlates with normalized IR, correlation > 0.99); `IR at 48k works at 44.1k host` (no error, output non-silent); `high cut attenuates 12 kHz by ≥ 12 dB at 6 kHz setting`; `missing file returns error`; `no IR = passthrough`.
- [ ] **Step 2: Run** `-R cab_ir` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(dsp): cab IR convolution node with cuts`

---

### Task 8: Processing plan, realtime engine, RT-safety guard

**Files:** Create `engine/engine/ProcessingPlan.{h,cpp}`, `engine/engine/RealtimeEngine.{h,cpp}`, `engine/engine/RtSafety.{h,cpp}`; Test `tests/realtime_engine_test.cpp`; Modify `tests/nam_amp_test.cpp` (drop `[!mayfail]`).

**Interfaces:**
- Consumes: `ProcessorNode` (5), node factories (5–7).
- Produces:
  ```cpp
  namespace amp {
  struct PlanNode { juce::String id; std::shared_ptr<ProcessorNode> node; std::atomic<bool> bypass; };
  class ProcessingPlan {          // immutable order; bypass flags mutable atomically
  public:
    ProcessingPlan(std::vector<std::shared_ptr<PlanNode>> ordered, ProcessSpec);
    void process(juce::AudioBuffer<float>& io, int n);   // mono until first isStereo() node; copies ch0→ch1 at end if never stereo
    PlanNode* find(const juce::String& id);
  };
  class RealtimeEngine : public juce::AudioIODeviceCallback {
  public:
    void setInputChannel(int ch); void setOutputPair(int firstCh);
    void swapPlan(std::shared_ptr<ProcessingPlan>);      // any thread; 30 ms equal-power crossfade; old plan freed off audio thread
    bool setBypass(const juce::String& nodeId, bool);    // false if not in current plan
    float inputPeakDb() const; float outputPeakDb() const;
    std::function<void(const float*, int)> analyzerTap; // set before start; receives input ch, used by TunerTap
  };
  namespace RtSafety { struct ScopedNoAlloc { ScopedNoAlloc(); ~ScopedNoAlloc(); }; int violations(); void reset(); }
  }
  ```
  `RtSafety` (debug builds only) overrides global `operator new/delete` to count allocations on a thread flagged by `ScopedNoAlloc`; release builds compile it to no-ops. Retired plans go to a lock-free queue drained by a `juce::Timer` on the message thread.

- [ ] **Step 1: Write failing tests** (drive the callback directly with fake buffers): `bypassed node is skipped` (gain −6 node bypassed → unity); `mono input reaches both outputs`; `swap crossfades without discontinuity` (constant DC input, plan A gain 0 dB → plan B gain 0 dB; max sample-to-sample jump < 0.01); `rapid plan swaps settle on last plan` (100 swaps across 50 callbacks alternating −6/0 dB, last is −12 dB; after 100 ms output = −12 dB ±0.1; no peak exceeds input peak; retired-queue drained to 0 after timer pump); `callback performs no allocation` (`RtSafety::violations()==0` across 1000 callbacks with gate+NAM+cab plan); `setBypass unknown id returns false`.
- [ ] **Step 2: Run** `-R realtime_engine` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run all tests** `ctest --test-dir build -C Debug` → PASS (incl. NAM no-alloc test).
- [ ] **Step 5: Commit** `feat(engine): RT-safe processing plan with crossfaded swaps and alloc guard`

---

### Task 9: Plugin host

**Files:** Create `engine/plugins/PluginHost.{h,cpp}`; Test `tests/plugin_host_test.cpp`.

**Interfaces:**
- Consumes: `ProcessorNode` (5), `PluginRef` (2).
- Produces:
  ```cpp
  namespace amp {
  class PluginHost {
  public:
    explicit PluginHost(juce::File dataDir);               // stores known-plugins.xml, blacklist.txt, crash-marker
    void scan(std::function<void(float)> progress);        // VST3 + AU (mac); writes crash marker before each plugin, clears after
    void checkCrashMarkerOnStartup();                      // marker present → that plugin id appended to blacklist
    juce::Array<juce::PluginDescription> available() const; // excludes blacklisted
    juce::StringArray blacklisted() const; void unblacklist(const juce::String& id);
    std::unique_ptr<ProcessorNode> create(const PluginRef&, const ProcessSpec&, juce::String& error); // null if missing/blacklisted
    static PluginRef refFor(const juce::PluginDescription&);
  };
  }
  ```
  The returned node wraps `juce::AudioPluginInstance`; `isStereo()` true if plugin has ≥ 2 outputs; `getState/setState` via `getStateInformation/setStateInformation`; exposes `juce::AudioPluginInstance* instance()` for editor windows (Task 18). Creation also writes the crash marker.

- [ ] **Step 1: Write failing tests** (register a tiny in-test `juce::AudioPluginFormat` subclass `FakeFormat` that yields a gain plugin, so tests need no installed plugins): `create known plugin returns node`; `unknown uid returns null with error`; `state round trips`; `crash marker on startup blacklists that plugin`; `blacklisted plugin cannot be created and is hidden from available()`; `unblacklist restores it`.
- [ ] **Step 2: Run** `-R plugin_host` → FAIL.
- [ ] **Step 3: Implement** with `juce::AudioPluginFormatManager` (add default formats + optional injected format for tests), `juce::KnownPluginList`, `juce::PluginDirectoryScanner` (scan in-process; crash marker covers crashes).
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(plugins): VST3/AU host with crash marker blacklist`

---

### Task 10: Settings and RigStore

**Files:** Create `engine/storage/Settings.{h,cpp}`, `engine/storage/RigStore.{h,cpp}`; Test `tests/rig_store_test.cpp`, `tests/settings_test.cpp`.

**Interfaces:**
- Consumes: `RigDocument`, `parseRig`, `serializeRig` (2).
- Produces:
  ```cpp
  namespace amp {
  struct Settings {
    juce::String audioDeviceType, audioDeviceName; int inputChannel=0, outputFirstChannel=0;
    double sampleRate=48000; int bufferSize=128;
    juce::StringArray watchedFolders; std::vector<Mapping> globalMappings;
    juce::String lastRigId; juce::StringArray recentRigIds /*max 10*/, favoriteRigIds;
    static Settings load(const juce::File&); bool save(const juce::File&) const;  // missing/corrupt file → defaults
  };
  struct RigEntry { juce::String id, name, band; juce::File file; };
  class RigStore {
  public:
    explicit RigStore(juce::File libraryRoot);   // creates Models/ IRs/ Rigs/ Art/ if absent
    juce::File root() const;
    std::vector<RigEntry> list() const;          // all rigs, sorted band then name
    juce::StringArray bands() const;             // always contains "My Tones"
    ParseResult load(const juce::String& rigId) const;
    juce::Result save(const RigDocument&);       // Rigs/<sanitized band>/<sanitized name>.amprig; temp+rename; rotate .bak.1..5
    juce::Result remove(const juce::String& rigId);
    std::vector<juce::File> backups(const juce::String& rigId) const; // newest first
    static juce::String sanitizeFileName(const juce::String&);        // replaces / \ : * ? " < > | and control chars with '_', trims dots/spaces, empty → "Untitled"
    static juce::File defaultLibraryRoot();                           // ~/Music/AMPerator or Documents\AMPerator
  };
  }
  ```
  Display names come from the JSON, never the filename. Two rigs whose sanitized names collide get ` (2)` appended to the filename.

- [ ] **Step 1: Write failing tests**: `save then load round trips`; `illegal characters sanitized` (band `"AC/DC"`, name `"Guns N' Roses: Live?"` → file is `Rigs/AC_DC/Guns N' Roses_ Live_.amprig`, no `DC` subfolder, loaded `band=="AC/DC"` and name exact); `filename collision gets suffix`; `sixth save keeps five backups` (`.bak.1`..`.bak.5`, newest = previous content); `corrupt rig file returns error and is not overwritten`; `renaming a rig's band moves the file` (old path gone); `bands includes My Tones on empty library`; `settings defaults when file missing`; `settings corrupt file yields defaults`; `recent list capped at 10`.
- [ ] **Step 2: Run** `-R "rig_store|settings"` → FAIL.
- [ ] **Step 3: Implement** (index rigs by scanning `Rigs/**.amprig` and reading `id`).
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(storage): settings and rig store with atomic saves and backups`

---

### Task 11: RigLoader

**Files:** Create `engine/storage/RigLoader.{h,cpp}`; Test `tests/rig_loader_test.cpp`.

**Interfaces:**
- Consumes: `RigDocument` (2), `graph::topoOrder` (3), `AssetIndex` (4), node factories (5–7), `ProcessingPlan`/`PlanNode` (8), `PluginHost` (9).
- Produces:
  ```cpp
  namespace amp {
  struct LoadReport { juce::StringArray missingAssets, missingPlugins; juce::StringArray placeholderNodeIds; };
  struct LiveRig { RigDocument doc; std::shared_ptr<ProcessingPlan> plan; LoadReport report; };
  class RigLoader {
  public:
    RigLoader(AssetIndex&, PluginHost&);
    std::optional<LiveRig> build(const RigDocument&, const ProcessSpec&, juce::String& error); // off audio thread
    RigDocument capture(const LiveRig&) const;  // current params/bypass/plugin state back into doc; placeholders unchanged
  };
  }
  ```
  Unresolvable asset or plugin → `makePlaceholder()` with `bypass=true`, id listed in report; original `NodeDoc` kept untouched in `doc`. Non-linear or cyclic doc → error.

- [ ] **Step 1: Write failing tests**: `builds default rig with fixture assets`; `missing NAM asset becomes bypassed placeholder and is reported`; `missing plugin keeps its state through capture` (state bytes identical after build→capture→serialize); `asset resolved by hash after rename`; `capture reflects param and bypass changes`; `cyclic doc returns error`.
- [ ] **Step 2: Run** `-R rig_loader` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(storage): rig loader with missing-asset placeholders`

---

### Task 12: AudioIO

**Files:** Create `engine/audio/AudioIO.{h,cpp}`; Test `tests/audio_io_test.cpp`.

**Interfaces:**
- Consumes: `Settings` (10), `RealtimeEngine` (8).
- Produces:
  ```cpp
  namespace amp {
  enum class DeviceStatus { Running, Disconnected, NoDevice };
  class AudioIO : private juce::ChangeListener {
  public:
    AudioIO(juce::AudioDeviceManager&, RealtimeEngine&);
    juce::Result apply(const Settings&);                 // opens device; falls back to default device with first input / outputs 1+2
    void writeBack(Settings&) const;
    DeviceStatus status() const; int xrunCount() const;
    double roundTripLatencyMs() const;                   // (inLatency + outLatency + 2*buffer) / sr * 1000
    std::function<void(DeviceStatus)> onStatus;
    juce::StringArray availableDevices() const; juce::Array<double> sampleRates() const; juce::Array<int> bufferSizes() const;
  };
  }
  ```
  On device removal: status `Disconnected`, output silent; on `ChangeListener` notification that the remembered device name reappears → reopen with same settings, status `Running`.

- [ ] **Step 1: Write failing tests** using a fake `juce::AudioIODeviceType` registered on the device manager (devices "Fake Solo", "Fake Other"): `applies settings to named device`; `unknown device falls back to default`; `disconnect sets status and reconnect restores same settings` (remove/re-add "Fake Solo" in fake type, call `callDeviceChangeListeners`); `latency formula` (fake device in=32, out=32, buffer 64 @48k → 4.0 ms ±0.01); `writeBack captures current device`.
- [ ] **Step 2: Run** `-R audio_io` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(audio): device manager wrapper with reconnect and latency report`

---

### Task 13: Control input (HID footswitch + MIDI)

**Files:** Create `engine/control/ControlEvent.h`, `engine/control/HidFootswitch.{h,cpp}`, `engine/control/MidiControl.{h,cpp}`; Modify `engine/storage/Settings.{h,cpp}` (add `footswitchKeys`), `cmake/Dependencies.cmake` (hidapi; mac `Info.plist` key `NSInputMonitoringUsageDescription`); Test `tests/control_input_test.cpp`.

**Interfaces:**
- Produces:
  ```cpp
  namespace amp {
  struct ControlEvent { juce::String control; bool pressed; };    // pressed=true on down edge, false on up edge
  class ControlSource { public: virtual ~ControlSource()=default; std::function<void(const ControlEvent&)> onEvent; };
  class KeyEdgeDetector {                                          // pure logic, shared by HID path
  public:
    std::vector<ControlEvent> feed(const std::set<uint8_t>& keysDownInReport); // HID boot-keyboard report → edges
    void assign(uint8_t keycode, const juce::String& control);    // e.g. 0x05 → "footswitch:L"
    std::optional<uint8_t> lastUnassignedKey() const;              // for setup learning
  };
  class HidFootswitch : public ControlSource {
  public:
    enum class Status { Connected, NotFound, PermissionDenied };
    void start(); void stop(); Status status() const;              // background thread; hid_open by VID/PID list + any keyboard-usage device with ≤ 8 keys; exclusive open on mac
    static std::vector<std::pair<uint16_t,uint16_t>> knownDevices(); // PCsensor 0x0c45:0x7403, 0x0c45:0x7404, 0x3553:0xb001, iKKEGOL 0x1a86:0xe026
  };
  class MidiControl : public ControlSource { public: void start(); void stop(); };  // all inputs; CC value ≥ 64 → pressed
  }
  ```
  Events are delivered on the message thread (`juce::MessageManager::callAsync`), never on the audio thread. Footswitch key assignment persists in `Settings::footswitchKeys` (`std::map<int, juce::String>`; add to `Settings` from Task 10), default `{0x04:"footswitch:L",0x05:"footswitch:M",0x06:"footswitch:R"}` = HID codes for the PCsensor factory keys a/b/c; confirmed on hardware in the Task 20 checklist.

- [ ] **Step 1: Write failing tests** (pure `KeyEdgeDetector` + `MidiControl` message translation via an injectable `handleMidi(const juce::MidiMessage&, const juce::String& device)`): `down edge emits pressed`; `up edge emits released`; `auto-repeat collapses to one press` (same key held across 20 identical reports → exactly one pressed event); `two pedals at once emit two presses`; `unassigned key is remembered for learning and emits nothing`; `midi CC 127 then 0 → pressed then released with id "midi:Dev:cc64"`; `midi note on → "midi:Dev:note60" pressed`.
- [ ] **Step 2: Run** `-R control_input` → FAIL.
- [ ] **Step 3: Implement**; HID reading via `hid_read_timeout` loop parsing 8-byte boot reports (byte 2..7 keycodes).
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(control): HID footswitch and MIDI control sources with edge detection`

---

### Task 14: MappingEngine

**Files:** Create `engine/control/MappingEngine.{h,cpp}`; Test `tests/mapping_engine_test.cpp`.

**Interfaces:**
- Consumes: `ControlEvent` (13), `Mapping` (2), `RealtimeEngine::setBypass` (8).
- Produces:
  ```cpp
  namespace amp {
  class MappingEngine {
  public:
    using BypassFn = std::function<bool(const juce::String& nodeId, std::optional<bool> set)>; // set=nullopt → toggle; returns new state
    explicit MappingEngine(BypassFn);
    void setMappings(std::vector<Mapping> rig, std::vector<Mapping> global); // rig overrides global for same control
    void setValidNodes(juce::StringArray ids);
    void handle(const ControlEvent&);            // only pressed=true acts
    void beginLearn(const juce::String& nodeId); void cancelLearn(); bool isLearning() const;
    std::function<void(const Mapping&)> onLearned;            // fired; caller writes into rig doc
    std::function<void(const juce::String& node, bool bypass)> onBypassChanged;
    std::vector<Mapping> effectiveMappings() const;           // pruned of invalid nodes
  };
  }
  ```

- [ ] **Step 1: Write failing tests**: `press toggles mapped node`; `release does nothing`; `rig mapping overrides global for same control`; `global mapping applies when rig has none`; `learn captures next press and does not toggle` (onLearned fired with control+node, bypass unchanged); `learning replaces an existing mapping for that control`; `stale mapping is ignored and pruned` (mapping to deleted node: press → no callback, `effectiveMappings()` excludes it); `unmapped press is a no-op`.
- [ ] **Step 2: Run** `-R mapping_engine` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(control): mapping engine with learn and override precedence`

---

### Task 15: Tuner tap

**Files:** Create `engine/dsp/TunerTap.{h,cpp}`; Test `tests/tuner_test.cpp`.

**Interfaces:**
- Produces: `class TunerTap { public: void prepare(double sr); void push(const float*, int); /* audio thread, lock-free FIFO */ std::optional<TunerReading> poll(); /* message thread */ };` with `struct TunerReading { float hz; juce::String note; int octave; float cents; };`. YIN pitch detection over a 2048-sample window, run in `poll()`; A4 = 440 Hz; returns nullopt below −50 dBFS.

- [ ] **Step 1: Write failing tests**: `detects low E` (82.41 Hz sine → note "E", octave 2, |cents| < 2); `detects A4 sharp` (445 Hz → "A", ~+19.6 cents ±2); `detects high E with harmonics` (329.63 Hz sawtooth → "E", octave 4); `silence returns nullopt`.
- [ ] **Step 2: Run** `-R tuner` → FAIL.
- [ ] **Step 3: Implement**; wire `RealtimeEngine::analyzerTap` to `TunerTap::push` in Task 16's facade.
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(dsp): YIN tuner tap`

---

### Task 16: Engine facade + JSON bridge

**Files:** Create `engine/bridge/Bridge.{h,cpp}`; Modify `engine/Engine.{h,cpp}`; Test `tests/bridge_test.cpp`; `docs/bridge-protocol.md` (command/event table, protocol version 1).

**Interfaces:**
- Consumes: everything from Tasks 2–15.
- Produces:
  ```cpp
  namespace amp {
  class Engine {                              // owns AssetIndex, PluginHost, RigStore, Settings, RigLoader, AudioIO, RealtimeEngine,
  public:                                     // HidFootswitch, MidiControl, MappingEngine, TunerTap; all on message thread
    struct Options { juce::File libraryRoot, dataDir; bool startAudio=true, startControls=true; };
    explicit Engine(Options);
    Bridge& bridge();
  };
  class Bridge {
  public:
    // in: {"v":1,"cmd":"...","id":n,"args":{...}}; out reply: {"v":1,"reply":n,"ok":bool,"result"|"error":...}
    void handle(const juce::var& message);
    std::function<void(const juce::var&)> send;  // replies + events {"v":1,"event":"...","data":{...}}
  };
  }
  ```
  Commands: `getState`, `listRigs`, `loadRig{id}`, `newRig{name,band}`, `saveRig`, `deleteRig{id}`, `nextRig`, `prevRig`, `addNode{type,index,plugin?,asset?}`, `moveNode{id,index}`, `removeNode{id}`, `setParam{id,name,value}`, `toggleBypass{id}`, `setAsset{id,file}`, `learnMapping{id}`, `cancelLearn`, `listAssets{kind}`, `listPlugins`, `rescanPlugins`, `openPluginEditor{id}`, `getSettings`, `setSettings{...}`, `setTuner{on}`, `toggleFavorite{id}`, `restoreBackup{id}` (copies newest `.bak` over the rig and loads it), `setNodeArt{id,archetype?,imageFile?}` (archetype is saved in the rig; a user image is copied into `Art/` and recorded in `Art/user-art.json` keyed by `<rigId>/<nodeId>`, never in the rig file). Events: `rigLoaded{rig,report}`, `rigDirty{dirty}`, `bypassChanged{id,bypass}`, `paramChanged{id,name,value}`, `meter{inDb,outDb}` (30 Hz), `tuner{...}` (20 Hz when on), `deviceStatus{status,latencyMs,xruns}`, `controlStatus{hid,midiDevices}`, `controlEvent{control,pressed}`, `learned{control,node}`, `error{message,context}`.
  Structural edits (`addNode/moveNode/removeNode/setAsset/loadRig`) rebuild via `RigLoader` on a background thread then `swapPlan`. `setParam`/`toggleBypass` act on the live plan without rebuild. On startup load `Settings.lastRigId`, else a new default rig. `nextRig/prevRig` walk `RigStore::list()` order.

- [ ] **Step 1: Write failing tests** (`Engine` with `startAudio=false, startControls=false`, temp library, capture `send`): `unknown command replies ok=false`; `wrong protocol version replies error mentioning version`; `newRig then saveRig then listRigs contains it`; `addNode emits rigLoaded with node in order`; `toggleBypass emits bypassChanged`; `learnMapping then injected control press emits learned and saves mapping in rig on saveRig`; `nextRig wraps around`; `startup loads last rig from settings`; `loadRig with missing asset reports it in rigLoaded.report`; `corrupt rig loadRig replies error and restoreBackup recovers it`; `user art image is not written into the rig file`.
- [ ] **Step 2: Run** `-R bridge` → FAIL.
- [ ] **Step 3: Implement** (add a test hook `Engine::injectControl(const ControlEvent&)`).
- [ ] **Step 4: Run** → PASS (whole suite).
- [ ] **Step 5: Commit** `feat(engine): engine facade and versioned JSON bridge`

---

### Task 17: App shell + web UI scaffold + Settings panel

**Files:** Create `app/MainWindow.{h,cpp}`, modify `app/Main.cpp`, `app/CMakeLists.txt` (build `ui/` with `npm ci && npm run build` as a custom command; embed `ui/dist` via `juce_add_binary_data`); Create `ui/package.json`, `ui/vite.config.ts`, `ui/index.html`, `ui/src/bridge.ts`, `ui/src/state.ts`, `ui/src/main.ts`, `ui/src/views/settings.ts`, `ui/src/styles.css`; Test `ui/src/bridge.test.ts` (vitest).

**Interfaces:**
- Consumes: `Engine`, `Bridge` (16).
- Produces (TS):
  ```ts
  export function call<T>(cmd: string, args?: object): Promise<T>;       // wraps window.__JUCE__.backend native function "amp"
  export function on(event: string, fn: (data: any) => void): () => void;
  export const store: { get(): AppState; subscribe(fn: (s: AppState) => void): () => void }; // updated from events
  export type View = 'stage' | 'edit';
  ```
  JUCE side: `WebBrowserComponent::Options().withNativeIntegrationEnabled().withNativeFunction("amp", ...)` → `Bridge::handle`; `Bridge::send` → `emitEventIfBrowserIsVisible("amp", ...)`; resources served with `withResourceProvider` from binary data. Dev mode: env `AMP_UI_DEV=1` loads `http://localhost:5173`.

- [ ] **Step 1: Write failing test** `bridge.test.ts`: `call resolves on matching reply id`; `call rejects on ok:false with error message`; `on() receives events and unsubscribe stops them` (mock `window.__JUCE__`).
- [ ] **Step 2: Run** `cd ui && npx vitest run` → FAIL.
- [ ] **Step 3: Implement** bridge/store; Settings view: device select, input channel, output pair, sample rate, buffer size with live `latencyMs` and xruns, watched folders add/remove, footswitch status (Connected / NotFound / PermissionDenied with "Open System Settings → Privacy & Security → Input Monitoring" text), MIDI device list. Gear icon opens it as a modal.
- [ ] **Step 4: Verify** — `npx vitest run` PASS; build app; launch; Settings shows Scarlett Solo, changing buffer to 64 updates latency readout; play guitar through default rig (empty amp/cab = gate only) and hear it.
- [ ] **Step 5: Commit** `feat(app): WebView UI shell with bridge and settings panel`

---

### Task 18: Edit view (pedalboard) + library panel + plugin windows

**Files:** Create `ui/src/views/edit.ts`, `ui/src/views/library.ts`, `ui/src/components/knob.ts`, `app/PluginWindow.{h,cpp}`; Test `ui/src/views/edit.test.ts`.

**Interfaces:**
- Consumes: `call/on/store` (17); bridge commands (16); `PluginHost` node `instance()` (9).
- Produces: `renderEdit(root: HTMLElement): void`, `renderLibrary(root: HTMLElement): void`; `openPluginEditor` handled in C++ by `PluginWindow` (one window per node id, closed when node removed or rig changes).

- [ ] **Step 1: Write failing tests** (vitest + jsdom, mocked `call`): `renders nodes in chain order excluding in/out`; `bypassed node has .off class`; `dragging card from index 0 to 2 calls moveNode{id,index:2}`; `right-click → Learn footswitch calls learnMapping`; `placeholder node shows "missing" badge with original name`; `library groups rigs by band and calls loadRig on click`.
- [ ] **Step 2: Run** `npx vitest run` → FAIL.
- [ ] **Step 3: Implement** pedalboard per spec §6: cards with bypass LED, click to expand knobs (`setParam`), **+** menu (built-ins: Gate, EQ, NAM Amp, Cab IR; Plugins list from `listPlugins`), NAM/IR file picker from `listAssets`, footswitch label under mapped nodes, rig name/band editable + Save (dirty indicator), slide-out library (bands → rigs, recents, favorites star, Models/IRs/Plugins tabs, disabled "Search online" tab labeled "coming soon"); error toasts from `error` events, and a failed `loadRig` toast offers "Open last backup" (`restoreBackup`).
- [ ] **Step 4: Verify** — vitest PASS; in app: build Zeppelin › Black Dog with a NAM model + IR + one VST3, save, quit, relaunch → same rig loads and sounds the same; open the plugin's editor window.
- [ ] **Step 5: Commit** `feat(ui): pedalboard edit view, library panel, plugin editor windows`

---

### Task 19: Stage view + node art

**Files:** Create `ui/src/views/stage.ts`, `ui/src/art/archetypes.ts`, `ui/src/art/*.svg` (amps: `british-stack`, `american-clean-combo`, `high-gain-rectifier`, `ac-chime`, `amp-generic`; cabs: `cab-4x12`, `cab-2x12`, `cab-1x12-open`; pedals: `drive`, `fuzz`, `delay`, `chorus`, `reverb`, `eq`, `gate`, `pedal-generic`); Modify `engine/plugins/PluginHost.cpp` (`snapshotEditor(nodeId) -> juce::File` PNG into `Art/cache/`), `engine/bridge/Bridge.cpp` (include `art` URL per node in `rigLoaded`); Test `ui/src/art/archetypes.test.ts`, `tests/bridge_test.cpp` (add art case).

**Interfaces:**
- Produces: `resolveArt(node: NodeView): string` — order: user image (`node.art.image`) → provider image (`node.art.providerImage`, unused until sub-project 3) → plugin snapshot (`node.art.snapshot`) → archetype SVG (`node.art.archetype`, else guessed from type/name keywords, else generic). `guessArchetype(type: string, name: string): string`.

- [ ] **Step 1: Write failing tests**: `user image wins over everything`; `plugin snapshot used for Plugin nodes without user image`; `guessArchetype("Plugin","Valhalla Delay") === "delay"`; `guessArchetype("NamAmp","Rectifier ch2") === "high-gain-rectifier"`; `unknown falls back to generic per type`; C++: `rigLoaded includes art.archetype for nodes that set it`.
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement** Stage view per spec §6 and approved mockup C: band, rig name, amp → cab line with art thumbnails, three big footswitch tiles (label = mapped node name or "—", lit when node active), prev/next, input meter, tuner overlay (note, octave, cents needle), ✎ Edit. App launches into Stage. Draw SVG art (simple flat illustrations, no logos or brand names).
- [ ] **Step 4: Verify** — tests PASS; in app: stomp each footswitch, tiles light in sync with Edit view LEDs; tuner reads open strings correctly.
- [ ] **Step 5: Commit** `feat(ui): stage mode and node art resolution`

---

### Task 20: Audio regression suite + manual checklist

**Files:** Create `tests/audio_regression_test.cpp`, `tests/fixtures/rigs/regression_*.amprig` (3 rigs: clean gate+EQ+IR; NAM+IR; NAM+IR with bypassed gate), `tests/fixtures/reference/*.wav` (generated once by the test in `AMP_UPDATE_REFERENCES=1` mode and committed), `docs/manual-test-checklist.md`.

**Interfaces:**
- Consumes: `RigLoader` (11), `ProcessingPlan` (8), `renderOffline` pattern (5).

- [ ] **Step 1: Write the test**: for each regression rig, render `di_48k.wav` at 48 kHz/128 block → compare to reference; pass if RMS of difference < −80 dBFS relative to reference RMS. With `AMP_UPDATE_REFERENCES=1` it writes references instead.
- [ ] **Step 2: Generate references** `AMP_UPDATE_REFERENCES=1 ctest --test-dir build -C Debug -R audio_regression`, listen to each WAV, commit them.
- [ ] **Step 3: Run** `ctest --test-dir build -C Debug` → whole suite PASS.
- [ ] **Step 4: Write `docs/manual-test-checklist.md`** covering: latency readout ≤ 10 ms at 64 and 128 on Scarlett Solo and it feels immediate; footswitch L/M/R key codes confirmed and work with app in background (another app focused, no stray keystrokes typed); Input Monitoring permission prompt flow; rig switching mid-strum has no click; unplug/replug Scarlett recovers; missing-plugin rig opens with placeholder; plugin crash → blacklisted on relaunch. Run through it once and record results in the file.
- [ ] **Step 5: Commit** `test: offline audio regression suite and manual checklist`
