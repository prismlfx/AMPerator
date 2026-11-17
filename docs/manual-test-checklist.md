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
- [ ] A rig that references a missing model/IR opens without crashing, shows the missing item in a banner, and "Locate…" on the card fixes it.
- [ ] Quitting (⌘Q or the window's close button) with unsaved changes asks Save / Discard / Cancel; with no changes it quits at once.
- [ ] Stomping a footswitch does not make the rig "unsaved" (no dot, no prompt on Next/Prev).
- [ ] A `.nam` dropped into `Models/` while the app runs shows up in the model picker without a relaunch.

## Plugins

- [ ] A rig whose VST3 is not installed opens with a placeholder in that slot (rest of the chain still plays).
- [ ] A VST3 editor opens, closes, and reopens; quitting with an editor open is clean (no crash dialog on quit or next launch).
- [ ] Plugin crash is blacklisted on relaunch, with a banner and a Retry (also under Settings → Disabled plugins). Only if you own a plugin known to crash; otherwise skip: ______
- [ ] Turning a knob in a plugin's own editor marks the rig unsaved.

## TONE3000

Needs a build with a client id (`cmake -B build -G Xcode -DAMP_TONE3000_CLIENT_ID=t3k_pub_...`), a TONE3000 account and network access. None of this has been verified by hand yet.

Sign-in and session
- [ ] A build without a client id: the TONE3000 tab and Settings say "This build has no TONE3000 client id." and nothing else breaks.
- [ ] Connect TONE3000: the sign-in window opens in the app, asks for your email and the emailed code, closes itself when you are in, and the tab shows your name. A Keychain item (service `com.amperator.tone3000`) is created, and the Keychain prompt appears once, not on every call.
- [ ] Quit and relaunch: still connected, no sign-in asked.
- [ ] Disconnect, then Connect: the window asks for your email again (the web session was really cleared), and the Keychain item is gone after Disconnect.
- [ ] Closing the sign-in window with the red button leaves the app in a sane state: no error toast, Connect works again.

Browsing and downloading
- [ ] Browse TONE3000: the picker's embedded content works inside the in-app window: audio previews play, any captcha or login step TONE3000 shows can be completed.
- [ ] Picking a tone opens its detail with models (A2 first); Download shows progress and ends with the file in `Models/TONE3000/<creator>/` (or `IRs/TONE3000/<creator>/`).
- [ ] A tone whose title has slashes, colons or emoji lands in a sane file name (no stray folders, no empty name).
- [ ] An A2 model plays at 48 kHz and at 44.1 kHz.
- [ ] An IR tone loads into a CabIR node and sounds right.
- [ ] The T3K mark and the creator line show on the node of a TONE3000 model; "View on TONE3000" opens tone3000.com in the default browser, and a non-TONE3000 link never does.
- [ ] Downloading the same model twice finishes at once without a second file; two different tones with the same name get ` (2)`.
- [ ] Cancel during a download leaves no partial file; quitting during a download leaves `<dataDir>/tone3000/.partial` empty after relaunch.

Restoring and replacing
- [ ] Delete a downloaded model file, reopen the rig: the node is a placeholder, the banner says "N models from TONE3000 are missing.", and Download restores it and the rig plays again.
- [ ] With Settings > "Download missing TONE3000 models automatically when opening a rig" on, the same restore happens by itself on load while connected; with it off, nothing downloads.
- [ ] `Replace from TONE3000...` on a node whose model is gone from TONE3000 opens the picker and swaps the node's file.

Offline and limits
- [ ] Offline launch keeps local playback working; the TONE3000 lists show "Can't reach TONE3000" with Retry, and Retry works once back online.
- [ ] If TONE3000 answers with a rate limit, the message names the wait and nothing retries on its own.

Release
- [ ] The placeholder logos in `ui/src/brand/tone3000/` are replaced with the official ones from TONE3000's logo pack, and "Powered by TONE3000" links to https://www.tone3000.com, before a public release.

## Robustness

- [ ] Quitting while audio is running exits cleanly (no crash report).
- [ ] Changing the audio device or sample rate while playing recovers without a restart.
- [ ] Launch with the Scarlett unplugged: another device plays; Settings still show the Scarlett ("not connected"); plugging it in switches back to it with the same channels.
- [ ] Bypass toggles (footswitch and the card LED) switch without a click.

## Results

Date: ______  macOS: ______  Buffer/latency readouts: 64 = ____ ms, 128 = ____ ms  Notes: ______

## Cleanup

Earlier automated testing created `Models/amp-test-tiny.nam`, `IRs/amp-test-ir.wav` and
`Rigs/Zeppelin/Black Dog.amprig` (+ `.bak`) in `~/Music/AMPerator`. Delete them if unwanted.
