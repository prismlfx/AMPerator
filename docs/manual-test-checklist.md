# AMPerator manual test checklist

Things automated tests cannot cover: real audio hardware, the USB footswitch, macOS permission prompts and
your own VST3s. Setup: Scarlett Solo, a 3-pedal PCsensor-style USB footswitch, headphones or monitors.

Legend: `[x]` = verified by the automated/headless run (evidence given); `[ ]` = **needs you**.

## Headless pre-checks (done by the implementer, 2026-10-19, no hardware)

- [x] C++ suite green: `ctest --test-dir build -C Debug` -> all passed (includes `audio_regression`, run 3x, stable).
- [x] UI suite green: `cd ui && npx vitest run` -> 6 files, 77 tests passed.
- [x] Release configuration builds and the suite passes (RtSafety compiles out): `ctest --test-dir build -C Release` -> 214/214 passed.
- [x] Debug app process launches and stays alive for 8 s (no crash on startup; no screen access, so UI not seen).
- [ ] App window shows the Stage view (needs you).

## Audio & latency

- [ ] Microphone permission prompt appears on first launch without the app hanging; denying it shows the System Settings guidance.
- [ ] Select the Scarlett Solo; guitar is audible through the default rig.
- [ ] Guitar is audible through a NAM + IR rig.
- [ ] At buffer 64: the latency readout is <= 10 ms and playing feels immediate.
- [ ] At buffer 128: the latency readout is <= 10 ms and playing feels immediate.
- [ ] Input meter moves with playing, and CLIP lights when you hit the input hot.
- [ ] Tuner reads the open strings correctly: E A D G B E.
- [ ] Rig switching mid-strum has no click.
- [ ] Unplug and replug the Scarlett: a banner appears, then audio returns without restarting the app.

## Footswitch

- [ ] Input Monitoring permission prompt flow works (prompt, grant in System Settings, relaunch if asked).
- [ ] Settings shows status Connected and whether the mode is "exclusive" or "shared".
- [ ] Settings > Footswitch shows "Last key: 0x.." when you stomp a pedal. Note the key codes: L ______  M ______  R ______
- [ ] Assign L/M/R in Settings > Footswitch (click Assign, stomp the pedal; Esc cancels), then confirm each pedal lights its Stage tile.
- [ ] Works with another app focused (e.g. a text editor in front), and in exclusive mode types no stray characters.
- [ ] Stomping L / M / R lights the matching Stage tiles and toggles the mapped node.
- [ ] Tip: if the mode is "shared" (pedal still types into other apps), reprogram the pedal to send F13 / F14 / F15 with the vendor tool, then re-check the items above.

## Rigs & files

- [ ] A rig opens, and after quit and relaunch it sounds the same (same models, IRs, knob values, bypass states).
- [ ] A rig that references a missing model/IR opens without crashing and shows the missing item.

## Plugins

- [ ] A rig whose VST3 is not installed opens with a placeholder in that slot (rest of the chain still plays).
- [ ] A VST3 editor opens, closes, and reopens; quitting with an editor open is clean (no crash dialog on quit or next launch).
- [ ] Plugin crash is blacklisted on relaunch. Only if you own a plugin known to crash; otherwise skip: ______

## Robustness

- [ ] Quitting while audio is running exits cleanly (no crash report).
- [ ] Changing the audio device or sample rate while playing recovers without a restart.

## Results

Date: ______  macOS: ______  Buffer/latency readouts: 64 = ____ ms, 128 = ____ ms  Notes: ______

## Cleanup

Earlier automated testing created `Models/amp-test-tiny.nam`, `IRs/amp-test-ir.wav` and
`Rigs/Zeppelin/Black Dog.amprig` (+ `.bak`) in `~/Music/AMPerator`. Delete them if unwanted.
