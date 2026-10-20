# AMPerator

An open-source (AGPLv3) guitar rig player built on JUCE 8: NAM amp models, cabinet IRs, and VST3/AU plugin chains.

## Build

Requires CMake 3.22+ and a C++20 compiler. Dependencies are fetched automatically (the first JUCE fetch is slow).

```sh
cmake -B build -G Xcode          # macOS; use any generator elsewhere
cmake --build build --target amp_tests
ctest --test-dir build -C Debug
cmake --build build --target AMPerator
```

## Running AMPerator

```sh
cmake -B build -G Xcode
cmake --build build --target AMPerator    # the app
cmake --build build --target amp_tests && ctest --test-dir build -C Debug   # C++ tests
(cd ui && npm install && npx vitest run)                                     # UI tests
```

Your library lives in `~/Music/AMPerator/` (Windows: `Documents\AMPerator\`): `Models/` for NAM amp models
(`.nam`), `IRs/` for cabinet impulse responses (`.wav`), `Rigs/<Band>/` for saved rigs (`.amprig`), `Art/`,
and `settings.json`. Drop `.nam` and IR `.wav` files into `Models/` and `IRs/`, or add any other folder as a
watched folder in Settings; rigs find their files by content hash, so renaming or moving them is fine.

Footswitch: a 3-pedal USB footswitch is read via HID as `footswitch:L`, `footswitch:M`, `footswitch:R`.
macOS asks for Input Monitoring permission the first time. If your pedal sends other keys, use Settings > Footswitch & MIDI > Assign L / M / R (the dialog shows the last key seen) to teach AMPerator which key is which. Settings shows whether the pedal is held in
"exclusive" mode (its key presses don't reach other apps) or "shared"; if shared, reprogram the pedal to send
F13/F14/F15 so it doesn't type characters. See `docs/manual-test-checklist.md` for the hardware test pass.

Audio regression tests render reference rigs offline and compare against `tests/fixtures/reference/*.wav`;
after an intentional DSP change, regenerate them with `AMP_UPDATE_REFERENCES=1 ctest --test-dir build -C Debug -R audio_regression`. That run deliberately reports a failure after writing the files (so it can never pass by comparing against itself); review the new WAVs, commit them, and rerun without the variable.
