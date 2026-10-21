# AMPerator bridge protocol — version 1

The web UI talks to the engine (`amp::Engine`, `engine/Engine.h`) through `amp::Bridge` (`engine/bridge/Bridge.h`).
Every message is one JSON object. Everything runs on the app's message thread.

## Transport (app ↔ web UI)

The app (`app/MainWindow.cpp`) hosts the UI in a JUCE `WebBrowserComponent` and registers two native functions,
called from `ui/src/bridge.ts` with JUCE's `__juce__invoke` / `__juce__complete` events:

| Native function | Argument | Completes with | Notes |
|---|---|---|---|
| `amp` | one command envelope as a JSON string (an already-decoded object is accepted too) | `undefined`, immediately | Forwarded to `Bridge::handle`. The reply, like every event, arrives as a JUCE event named `amp` (`Bridge::send` → `emitEventIfBrowserIsVisible("amp", message)`). The UI correlates replies by `id` and gives up after 10 s. |
| `chooseFolder` | — | the chosen absolute path, or `""` if cancelled | Native folder chooser (the web view has none); used to add watched folders. One chooser at a time (a second call completes with `""`). |

**Node art** (`art/` resource path): the app's resource provider also serves images from the library's `Art/` folder
at `getResourceProviderRoot() + "art/<path relative to Art/>"` (the page, served from the provider root, uses the
relative URL `art/<path>`, each component URI-encoded). Only existing `png`/`jpg`/`jpeg`/`gif`/`webp`/`svg` files
inside `Art/` are served (`Engine::artFileForRequest`): `..`, `.`, empty or `~` components, backslashes, colons,
symlinks and every other file (e.g. `user-art.json`) get a 404. In dev mode the provider allows the dev-server origin,
and the UI uses the absolute `juce://juce.backend/art/...` URL (macOS).

**Plugin snapshots:** the first time a plugin is loaded (rig load or structural edit, before `rigLoaded` is sent), the
engine creates its editor offscreen on the message thread and caches a PNG (`Art/cache/<plugin id>-<hash>.png`, at
most 480 px wide) if it drew anything. The attempt runs under its own marker, not the plugin crash marker (a crash
there never blacklists the plugin): `<name>.blank` is written before the editor is created and removed only after the
PNG is written. A leftover `.blank` (blank editor, e.g. most native VST3/AU views, or a crash) means the plugin is
never snapshotted again and falls back to archetype art. Delete `Art/cache/` to retry.

The UI is served from the embedded `ui/dist` (`WebBrowserComponent::getResourceProviderRoot()`); with the environment
variable `AMP_UI_DEV=1` a Debug build of the app loads the Vite dev server at `http://localhost:5173` instead (`cd ui && npm run dev`).

## Envelopes

| Direction | Shape |
|---|---|
| UI → engine (command) | `{"v":1, "cmd":"<name>", "id":<n>, "args":{...}}` (`args` optional, must be an object) |
| engine → UI (reply) | `{"v":1, "reply":<id>, "ok":true, "result":{...}}` or `{"v":1, "reply":<id>, "ok":false, "error":"<message>"}` |
| engine → UI (event) | `{"v":1, "event":"<name>", "data":{...}}` |

- Every command gets exactly one reply carrying the request's `id` (echoed verbatim; `null` if the message had none).
- `v` other than `1` → `ok:false`, error `unsupported protocol version <v>; this engine speaks version 1`.
- Unknown command → `ok:false`, error `unknown command: <cmd>`.
- Missing / wrongly typed arguments → `ok:false` with a message naming the argument (e.g.
  `missing or invalid string argument "id"`); ids and parameter names must be non-empty; `args`/objects must be
  JSON objects (not arrays). Malformed input never crashes the engine.
- `result` is always an object (`{}` when the command has nothing to return). Most state changes are also announced
  by events, so the UI can treat events as the single source of truth.
- Numbers are JSON numbers; a boolean is accepted where a number is expected (`true` = 1). Integers must be whole.

## Shared shapes

**Rig** (`rig` in `rigLoaded` / `getState`): the `.amprig` JSON (`format`, `version`, `id`, `name`, `band`, `tags`,
`nodes`, `connections`, `mappings`, see the spec §5) with two differences:
- `nodes` are listed in **signal order** (Input first, Output last; nodes off the chain follow).
- `tags` is always present (`[]` when the rig has none): free-form labels such as a style, a decade, a band or a
  song (`setTags`).
- every node carries `art: {archetype, image, snapshot}` — `archetype` is the rig's built-in art id (may be `""`),
  `image` is the absolute path of a user-assigned image in `Art/` (or `""`), `snapshot` the absolute path of a cached
  PNG of the plugin's editor in `Art/cache/` (Plugin nodes, or `""`). Neither file is stored in the rig file. The UI
  loads both through the art resource path below and picks the picture in the spec §6 order (user image → provider
  image → snapshot → archetype; `ui/src/art/archetypes.ts`). Built-in archetype ids: amps `british-stack`,
  `american-clean-combo`, `high-gain-rectifier`, `ac-chime`, `amp-generic`; cabs `cab-4x12`, `cab-2x12`,
  `cab-1x12-open`; pedals `drive`, `fuzz`, `delay`, `chorus`, `reverb`, `eq`, `gate`, `pedal-generic` (an unknown or
  empty id means "guess from the node's type and name").
- `params` hold the live values (every parameter the node exposes), `bypass` the live state, `state` the plugin state
  (base64) as of the last capture.
- The **Output** node's `params.level` (dB, -60..12, default 0) is the master volume: `setParam{id: <Output id>, name:
  "level"}`; it is applied after the chain and is never bypassed.
- After a load with a non-empty report the UI shows a banner listing the missing assets/plugins and the files matched by
  file name; placeholder NamAmp/CabIR nodes offer "Locate…" (asset picker → `setAsset`).

**Report** (`report`): `{missingAssets: [file], missingPlugins: [name], placeholderNodeIds: [id], resolvedByFilename: [path]}`.
Placeholder nodes keep all their data but run as bypassed passthroughs; `toggleBypass`/`setParam` on them reply
`ok:false`.

**PluginRef**: `{format, uid, name, vendor}`.

**PluginInfo** (`listPlugins` / `rescanPlugins`): a PluginRef plus what the scan reported, for sorting the plugin
picker: `{format, uid, name, vendor, category, isInstrument, numInputChannels, numOutputChannels}`. `category` is
JUCE's `PluginDescription::category` verbatim (VST3: e.g. `"Fx|Distortion"`, `"Instrument|Synth"`; AU: e.g.
`"Effect"`, `"Synth"`, `"MusicEffect"`, `"Generator"`; may be `""`), `isInstrument` is
`PluginDescription::isInstrument`, and the channel counts are the totals over all buses the scan saw (an instrument
typically has 0 inputs; a sidechain bus adds inputs). A PluginInfo is accepted wherever a PluginRef is (the extra
fields are ignored), and only the PluginRef fields are written to rig files.

**Asset**: `{file: absolute path, name: file name without extension, kind: "nam"|"ir", sha256}`.

**Settings**: `{audioDeviceType, audioDeviceName, inputChannel, outputFirstChannel, sampleRate, bufferSize,
watchedFolders: [path], globalMappings: [{control, action, node}], footswitchKeys: {"0x04": "footswitch:L", ...},
footswitchDevice: {vid, pid} | null}` (`footswitchDevice`: a keyboard the user picked as the footswitch; it is opened in
addition to the automatically recognised models — no other keyboard is ever grabbed. `setSettings` takes `{vid, pid}`
(16-bit ids) or `null` to clear.)
(+ in `getSettings`/`setSettings` results: `availableDevices: [name]`, `availableSampleRates: [hz]`,
`availableBufferSizes: [n]`, `availableInputChannels: [name]`, `availableOutputChannels: [name]` of the open device /
current type; empty without audio. `inputChannel` / `outputFirstChannel` index the channel-name lists).

**DeviceStatus**: `{status: "Running"|"Disconnected"|"NoDevice", latencyMs, xruns}` (`xruns` = -1 when unknown).

**ControlStatus**: `{hid: "Connected"|"NotFound"|"PermissionDenied"|"Busy", exclusive: bool, lastKey: int|null, assigning: "footswitch:L"|"footswitch:M"|"footswitch:R"|null, midiDevices: [name]}`. `lastKey` is the HID keycode of the most recent pedal key-down (assigned or not; null until one is seen); `assigning` is the control waiting for its key after `assignFootswitchKey`.

## Commands

| Command | Args | Result | Notes / events |
|---|---|---|---|
| `getState` | — | `{protocol: 1, rig, report, dirty, startupErrors: [{message, context, rigId?}], saved, learning, settings, deviceStatus, controlStatus, tunerOn, favorites: [id], recents: [id], bands: [name]}` | Call on UI start-up. `startupErrors` lists problems found while starting (`context` `loadRig` with the `rigId` of a last rig that could not be loaded — offer `restoreBackup`; `audioDevice`; `audioPermission` when microphone access was denied — the app opens the device only after asking, so this entry may appear after the first `getState`; `pluginBlacklisted` with `pluginId`: "<plugin> crashed last time and was disabled" — offer Retry → `unblacklistPlugin`). |
| `listRigs` | — | `{rigs: [{id, name, band, tags: [tag], favorite}], bands: [name]}` | Sorted by band, then name. `tags` as stored in each file. Corrupt files and hidden files/folders (`.DS_Store`, ...) are not listed. Two files with the same `id` (e.g. a Finder copy): the file the engine already knew keeps the id (else the oldest file, then the shorter name) and the other is rewritten (atomically) with a fresh id, so both list and open independently. |
| `loadRig` | `id` | `{id}` | `rigLoaded`, `rigDirty{false}` if it was dirty. Corrupt / unreadable rig → `ok:false` with the parse error (file untouched; offer `restoreBackup`). Unsaved changes of the current rig are discarded (UI confirms using `dirty`). |
| `newRig` | `name?` (default `"New Rig"`), `band?` (default `"My Tones"`) | `{id}` | Default chain In → Gate → NamAmp → CabIR → Out; **unsaved** until `saveRig` (`saved:false`). `rigLoaded`. |
| `saveRig` | `name?`, `band?`, `asNew?` (bool) | `{id, name, band}` | Captures live params/bypass/plugin state, writes atomically (keeps 5 `.bak`). Becomes the last rig. `rigDirty{false}` (always). A non-blank `name` / `band` (trimmed) renames the rig / moves it to that band's folder first (same id; backups move with it, and the version saved under the old name becomes `.bak.1`); blank or absent keeps the current value. The result carries the saved name and band (no `rigLoaded`). **`asNew: true`** ("Save as…"): writes a copy under a fresh id (file name made unique), leaves the original file untouched, keeps the nodes' user images, and makes the copy the current rig → `rigLoaded` (new `id`, `saved:true`), then `rigDirty{false}`. |
| `deleteRig` | `id` | `{}` | Moves the file and its backups to the system Trash (`juce::File::moveToTrash`), drops it from favorites/recents. If it was the current rig, the rig now at its list position (else a new default rig) is loaded → `rigLoaded`. |
| `nextRig` / `prevRig` | — | `{id}` | Walks `listRigs` order with wrap-around. An unsaved current rig counts as "before the first": next → first, prev → last. No saved rigs → `ok:false`. |
| `addNode` | `type` (`Gate`,`EQ`,`NamAmp`,`CabIR`,`Plugin`), `index` (chain position among non-I/O nodes, clamped), `plugin?` (PluginRef or PluginInfo from `listPlugins`, or its `uid`; required for `Plugin`), `asset?` (absolute path from `listAssets`, NamAmp/CabIR only) | `{id}` | Structural edit: rebuild + crossfaded swap. `rigLoaded`, `rigDirty{true}`. |
| `moveNode` | `id`, `index` | `{}` | Structural. `rigLoaded`, `rigDirty`. |
| `removeNode` | `id` | `{}` | Structural; also removes the rig's mappings to that node. `rigLoaded`, `rigDirty`. |
| `setAsset` | `id`, `file` (absolute path from `listAssets`; `""` clears) | `{}` | NamAmp (`.nam`) / CabIR (IR) nodes. File must be in the asset library. Structural. |
| `setParam` | `id`, `name`, `value` (number) | `{value}` (value the node stored, after clamping to the documented range: Gate `threshold` -90..0 dB, `release` 5..1000 ms; EQ `low`/`mid`/`high` -12..12 dB, `midFreq` 200..5000 Hz; NamAmp `inputGain`/`outputGain` -24..24 dB, `normalize` 0/1; CabIR `lowCut` 20..500 Hz, `highCut` 1000..20000 Hz, `level` -24..12 dB — `engine/dsp/ParamRanges.h`; rig files are clamped the same way) | Live, no rebuild. `paramChanged`, `rigDirty{true}`. Unknown parameter → `ok:false`. Plugin parameters are edited in the plugin's own editor; a parameter, program or state change the plugin reports there marks the rig dirty (`rigDirty{true}`). |
| `toggleBypass` | `id` | `{bypass}` | Live. `bypassChanged` — **not** `rigDirty`: bypass is a performance state (footswitch presses toggle it too), so stomping never makes the rig "unsaved"; `saveRig` still stores the current bypass states. The Output node cannot be bypassed (`ok:false`). The audio crossfades over ~10 ms (no click); a node switched back on starts from a clean state (filter / convolution / model history cleared) instead of replaying what it held when it was bypassed. |
| `learnMapping` | `id` | `{}` | Cancels any armed learn, then arms learn (unknown/unmappable node → `ok:false`): the next control press maps to `toggleBypass(id)` in the current rig (replacing that control's mapping) → `learned`, `rigDirty{true}`. Loading another rig cancels learn. |
| `cancelLearn` | — | `{}` | |
| `clearMapping` | `control` | `{}` | Removes the current rig's mapping of that control (global mappings in Settings stay) → `mappingCleared{control}`, `rigDirty{true}`. No rig mapping for it → `ok:false`. |
| `assignFootswitchKey` | `{control}` (`footswitch:L`/`M`/`R`) | `{}` | The next HID key pressed becomes that control's key (replacing the control's previous key and taking the key from any other control); that press does not trigger a control. Saved to `settings.footswitchKeys`, applied immediately, `controlStatus` emitted with `assigning:null` and the new `lastKey`. |
| `cancelAssignFootswitchKey` | — | `{}` | Aborts a pending assign (`controlStatus.assigning` → null). |
| `listAssets` | `kind` (`"nam"`\|`"ir"`) | `{assets: [Asset]}` | From `Models/`, `IRs/` and watched folders, rescanned on every call (hashes are cached), so files dropped in while the app runs appear at once. `loadRig`, `newRig`, `addNode` with an asset and `setAsset` rescan before resolving too. |
| `listPlugins` | — | `{plugins: [PluginInfo]}` | From the last scan (blacklisted plugins excluded). |
| `listBlacklisted` | — | `{plugins: [{id, name}]}` | Plugins disabled because they crashed AMPerator (blacklist ids: a plugin id or a plugin file; `name` is the known plugin name, else the file name, else the id). |
| `unblacklistPlugin` | `id` | `{plugins: [{id, name}]}` (the remaining list) | Re-enables it. A plugin that crashed while it was being scanned only shows up again after `rescanPlugins`. Unknown id → `ok:false`. |
| `listHidKeyboards` | — | `{keyboards: [{vid, pid, id: "vvvv:pppp", manufacturer, product}]}` | Connected keyboard-type HID devices (usage page 1, usage 6), one per vendor/product id, for picking an unrecognised footswitch (`setSettings{footswitchDevice}`). Empty when the engine runs without controls. |
| `rescanPlugins` | — | `{plugins: [PluginInfo]}` | **Blocks** the engine/UI thread for the whole scan (in-process scanning); show a busy state, and wait longer than the default 10 s for the reply (`call(cmd, args, timeoutMs)`). Never run automatically at startup. |
| `openPluginEditor` | `id` | `{}` | Asks the app to open the plugin's native editor window (one per node; opening it again brings it to the front). `ok:false "not available"` if the node isn't a loaded plugin or the host app has no editor support. Editor windows close whenever the rig's nodes are rebuilt (`Engine::beforeNodesReplaced`: rig load, structural edit, device-rate change); after a rebuild of the same rig the app reopens the editors of nodes that still exist. |
| `getSettings` | — | Settings (+ available lists) | |
| `setSettings` | any subset of Settings fields | Settings (+ available lists) | Device fields that actually change reopen the device (plan rebuilt at the new rate/buffer); any device field reopens it also when the device is not running or a fallback runs (a retry, even with unchanged values). The device fields are the user's **preferred** device: when it is missing at open the type's default device runs as a fallback, but Settings keep naming the preferred device (and its channels) — only an open of the preferred device itself (or of a fallback when no device was named) writes the actual rate/buffer/channels back. While a fallback runs the engine watches hot-plug changes and switches to the preferred device, with its saved channels, as soon as it is listed again; `watchedFolders` (absolute paths) rescans assets; `globalMappings` (`action` defaults to `toggleBypass`); `footswitchKeys` (keycode `"0x04"`/`"4"` → control). Saved to `settings.json`. A device that fails to open → `ok:false` with the driver's message (other fields still applied). |
| `setTuner` | `on` (bool) | `{on}` | On: `tuner` events at 20 Hz until switched off. |
| `toggleFavorite` | `id` | `{id, favorite}` | |
| `setTags` | `tags` (array of strings, the complete new list) | `{tags}` (the list as stored) | Replaces the current rig's tags. Each tag is trimmed and must be non-empty, at most 40 characters, without control characters (U+0000–U+001F, U+007F–U+009F); duplicates are dropped case-insensitively, keeping the first spelling; at most 32 tags (after dropping duplicates). Any invalid tag → `ok:false` naming the problem, rig unchanged. A changed list → `tagsChanged{tags}`, `rigDirty{true}` (written by the next `saveRig`; no `rigLoaded`, nothing is rebuilt). The same list again (exact spelling) changes nothing. |
| `listTags` | — | `{tags: [{tag, count}]}` | Every tag of the saved rigs in the library (not unsaved edits of the current rig), merged case-insensitively (the spelling seen first in `listRigs` order is shown), `count` = number of rigs carrying it. Sorted by `count` descending, then tag name (case-insensitive). For tag autocomplete and the library's tag filter bar. |
| `restoreBackup` | `id` | `{id}` | Copies the newest `.bak` over the rig file (the current file is first kept as `<file>.corrupt` if unreadable, else `<file>.replaced`), then loads it. Works for rigs corrupted before this launch (found through their backups) → `rigLoaded`. No backup → `ok:false`. |
| `confirmQuit` | `action` (`"save"`\|`"discard"`\|`"cancel"`) | `{}` | The answer to `quitRequested`. `save` saves the rig (as `saveRig` with no args) and quits; a failed save replies `ok:false` and does not quit. `discard` quits without saving; `cancel` keeps running. Other values → `ok:false`. |
| `setNodeArt` | `id`, `archetype?`, `imageFile?` | `{id, art: {archetype, image, snapshot}}` | `archetype` is stored in the rig (`rigDirty{true}`). `imageFile` (absolute path) is copied into `Art/` under a generated ASCII name (`<rigId>-<nodeId>-<hash>.<ext>`, characters `[A-Za-z0-9._-]` only, so its `art/` URL needs no encoding on any platform) and recorded in `Art/user-art.json` under `"<rigId>/<nodeId>"` — never in the rig; `""` removes the assignment. |

## Events

| Event | Data | When |
|---|---|---|
| `rigLoaded` | `{rig, report, saved}` | A rig was loaded/created, or a structural edit rebuilt it. |
| `rigDirty` | `{dirty}` | Dirty state changed; always after `saveRig`. |
| `tagsChanged` | `{tags}` | `setTags` changed the current rig's tags (the rig payload's `tags`). |
| `bypassChanged` | `{id, bypass}` | `toggleBypass` or a mapped control press (never followed by `rigDirty`). |
| `paramChanged` | `{id, name, value}` | After `setParam`. |
| `meter` | `{inDb, outDb}` | 30 Hz while audio is enabled; peak dBFS since the previous event, floor -100. |
| `tuner` | `{active, hz, note, octave, cents}` | 20 Hz while the tuner is on. `active:false` (hz 0, note `""`) when no stable pitch. |
| `deviceStatus` | DeviceStatus | On device status change, and when latency/xruns change (polled every 1 s). |
| `controlStatus` | ControlStatus | On change (polled every 1 s while controls run). |
| `controlEvent` | `{control, pressed}` | Every footswitch/MIDI edge (`footswitch:L`, `midi:<device>:cc<n>`, ...). |
| `learned` | `{control, node}` | A learn completed. |
| `learnCancelled` | `{}` | An armed learn was dropped because its node went away (e.g. `removeNode`); hide the "press a footswitch" overlay. |
| `mappingCleared` | `{control}` | After `clearMapping`. |
| `quitRequested` | `{rigName}` | The app is quitting (Cmd-Q / window close) and the rig has unsaved changes. Show Save / Discard / Cancel and answer with `confirmQuit`. If no answer arrives within ~5 s the app asks with a native dialog instead. A clean rig quits without this event. |
| `error` | `{message, context}` | Asynchronous problems (`context`: `audioDevice`, `audioPermission` (microphone access denied; also listed in `startupErrors`), `pluginBlacklisted` (+ `pluginId`; also in `startupErrors`), `deviceRestart`, `swapPlan`, `settings`). Command failures are replies, not events. Errors raised before the UI set `send` (startup) are queued and delivered ahead of the first reply/event. |
