# AMPerator bridge protocol — version 1

The web UI talks to the engine (`amp::Engine`, `engine/Engine.h`) through `amp::Bridge` (`engine/bridge/Bridge.h`).
Every message is one JSON object. Everything runs on the app's message thread.

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
  `missing or invalid string argument "id"`). Malformed input never crashes the engine.
- `result` is always an object (`{}` when the command has nothing to return). Most state changes are also announced
  by events, so the UI can treat events as the single source of truth.
- Numbers are JSON numbers; a boolean is accepted where a number is expected (`true` = 1). Integers must be whole.

## Shared shapes

**Rig** (`rig` in `rigLoaded` / `getState`): the `.amprig` JSON (`format`, `version`, `id`, `name`, `band`, `tags`,
`nodes`, `connections`, `mappings`, see the spec §5) with two differences:
- `nodes` are listed in **signal order** (Input first, Output last; nodes off the chain follow).
- every node carries `art: {archetype, image}` — `archetype` is the rig's built-in art id (may be `""`), `image` is
  the absolute path of a user-assigned image in `Art/` (or `""`). The image is never stored in the rig file.
- `params` hold the live values (every parameter the node exposes), `bypass` the live state, `state` the plugin state
  (base64) as of the last capture.

**Report** (`report`): `{missingAssets: [file], missingPlugins: [name], placeholderNodeIds: [id], resolvedByFilename: [path]}`.
Placeholder nodes keep all their data but run as bypassed passthroughs; `toggleBypass`/`setParam` on them reply
`ok:false`.

**PluginRef**: `{format, uid, name, vendor}`.

**Asset**: `{file: absolute path, name: file name without extension, kind: "nam"|"ir", sha256}`.

**Settings**: `{audioDeviceType, audioDeviceName, inputChannel, outputFirstChannel, sampleRate, bufferSize,
watchedFolders: [path], globalMappings: [{control, action, node}], footswitchKeys: {"0x04": "footswitch:L", ...}}`
(+ in `getSettings`/`setSettings` results: `availableDevices: [name]`, `availableSampleRates: [hz]`,
`availableBufferSizes: [n]` of the open device / current type; empty without audio).

**DeviceStatus**: `{status: "Running"|"Disconnected"|"NoDevice", latencyMs, xruns}` (`xruns` = -1 when unknown).

**ControlStatus**: `{hid: "Connected"|"NotFound"|"PermissionDenied"|"Busy", exclusive: bool, midiDevices: [name]}`.

## Commands

| Command | Args | Result | Notes / events |
|---|---|---|---|
| `getState` | — | `{protocol: 1, rig, report, dirty, saved, learning, settings, deviceStatus, controlStatus, tunerOn, favorites: [id], recents: [id], bands: [name]}` | Call on UI start-up; startup events are sent before the UI listens. |
| `listRigs` | — | `{rigs: [{id, name, band, favorite}], bands: [name]}` | Sorted by band, then name. Corrupt files are not listed. |
| `loadRig` | `id` | `{id}` | `rigLoaded`, `rigDirty{false}` if it was dirty. Corrupt / unreadable rig → `ok:false` with the parse error (file untouched; offer `restoreBackup`). Unsaved changes of the current rig are discarded (UI confirms using `dirty`). |
| `newRig` | `name?` (default `"New Rig"`), `band?` (default `"My Tones"`) | `{id}` | Default chain In → Gate → NamAmp → CabIR → Out; **unsaved** until `saveRig` (`saved:false`). `rigLoaded`. |
| `saveRig` | — | `{id}` | Captures live params/bypass/plugin state, writes atomically (keeps 5 `.bak`). Becomes the last rig. `rigDirty{false}` (always). |
| `deleteRig` | `id` | `{}` | Removes the file and its backups, drops it from favorites/recents. If it was the current rig, the rig now at its list position (else a new default rig) is loaded → `rigLoaded`. |
| `nextRig` / `prevRig` | — | `{id}` | Walks `listRigs` order with wrap-around. An unsaved current rig counts as "before the first": next → first, prev → last. No saved rigs → `ok:false`. |
| `addNode` | `type` (`Gate`,`EQ`,`NamAmp`,`CabIR`,`Plugin`), `index` (chain position among non-I/O nodes, clamped), `plugin?` (PluginRef from `listPlugins`, or its `uid`; required for `Plugin`), `asset?` (absolute path from `listAssets`, NamAmp/CabIR only) | `{id}` | Structural edit: rebuild + crossfaded swap. `rigLoaded`, `rigDirty{true}`. |
| `moveNode` | `id`, `index` | `{}` | Structural. `rigLoaded`, `rigDirty`. |
| `removeNode` | `id` | `{}` | Structural; also removes the rig's mappings to that node. `rigLoaded`, `rigDirty`. |
| `setAsset` | `id`, `file` (absolute path from `listAssets`; `""` clears) | `{}` | NamAmp (`.nam`) / CabIR (IR) nodes. File must be in the asset library. Structural. |
| `setParam` | `id`, `name`, `value` (number) | `{value}` (value the node stored, after clamping) | Live, no rebuild. `paramChanged`, `rigDirty{true}`. Unknown parameter → `ok:false`. Plugin parameters are edited in the plugin's own editor. |
| `toggleBypass` | `id` | `{bypass}` | Live. `bypassChanged`, `rigDirty{true}`. |
| `learnMapping` | `id` | `{}` | Arms learn: the next control press maps to `toggleBypass(id)` in the current rig (replacing that control's mapping) → `learned`, `rigDirty{true}`. Loading another rig cancels learn. |
| `cancelLearn` | — | `{}` | |
| `listAssets` | `kind` (`"nam"`\|`"ir"`) | `{assets: [Asset]}` | From `Models/`, `IRs/` and watched folders. |
| `listPlugins` | — | `{plugins: [PluginRef]}` | From the last scan (blacklisted plugins excluded). |
| `rescanPlugins` | — | `{plugins: [PluginRef]}` | **Blocks** the engine/UI thread for the whole scan (in-process scanning); show a busy state. Never run automatically at startup. |
| `openPluginEditor` | `id` | `{}` | Asks the app to open the plugin's native editor window. `ok:false "not available"` if the node isn't a loaded plugin or the host app has no editor support. |
| `getSettings` | — | Settings (+ available lists) | |
| `setSettings` | any subset of Settings fields | Settings (+ available lists) | Device fields reopen the device (plan rebuilt at the new rate/buffer); `watchedFolders` (absolute paths) rescans assets; `globalMappings` (`action` defaults to `toggleBypass`); `footswitchKeys` (keycode `"0x04"`/`"4"` → control). Saved to `settings.json`. A device that fails to open → `ok:false` with the driver's message (other fields still applied). |
| `setTuner` | `on` (bool) | `{on}` | On: `tuner` events at 20 Hz until switched off. |
| `toggleFavorite` | `id` | `{id, favorite}` | |
| `restoreBackup` | `id` | `{id}` | Copies the newest `.bak` over the rig file (an unreadable rig file is first kept as `<file>.corrupt`), then loads it → `rigLoaded`. No backup → `ok:false`. |
| `setNodeArt` | `id`, `archetype?`, `imageFile?` | `{id, art: {archetype, image}}` | `archetype` is stored in the rig (`rigDirty{true}`). `imageFile` (absolute path) is copied into `Art/` and recorded in `Art/user-art.json` under `"<rigId>/<nodeId>"` — never in the rig; `""` removes the assignment. |

## Events

| Event | Data | When |
|---|---|---|
| `rigLoaded` | `{rig, report, saved}` | A rig was loaded/created, or a structural edit rebuilt it. |
| `rigDirty` | `{dirty}` | Dirty state changed; always after `saveRig`. |
| `bypassChanged` | `{id, bypass}` | `toggleBypass` or a mapped control press. |
| `paramChanged` | `{id, name, value}` | After `setParam`. |
| `meter` | `{inDb, outDb}` | 30 Hz while audio is enabled; peak dBFS since the previous event, floor -100. |
| `tuner` | `{active, hz, note, octave, cents}` | 20 Hz while the tuner is on. `active:false` (hz 0, note `""`) when no stable pitch. |
| `deviceStatus` | DeviceStatus | On device status change, and when latency/xruns change (polled every 1 s). |
| `controlStatus` | ControlStatus | On change (polled every 1 s while controls run). |
| `controlEvent` | `{control, pressed}` | Every footswitch/MIDI edge (`footswitch:L`, `midi:<device>:cc<n>`, ...). |
| `learned` | `{control, node}` | A learn completed. |
| `error` | `{message, context}` | Asynchronous problems (`context`: `audioDevice`, `deviceRestart`, `swapPlan`, `settings`). Command failures are replies, not events. |
