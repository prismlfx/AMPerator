# Third-party notices

| Component | License | Use |
|---|---|---|
| [JUCE](https://github.com/juce-framework/JUCE) 8.0.15 | AGPLv3 | Audio, plugin hosting, GUI |
| [Catch2](https://github.com/catchorg/Catch2) v3.8.1 | BSL-1.0 | Tests only |
| [NeuralAmpModelerCore](https://github.com/sdatkinson/NeuralAmpModelerCore) commit 0b3d3c97b0859a3a8c92a8628c4dd89a25eb5842 | MIT | NAM amp model inference |
| [nlohmann/json](https://github.com/nlohmann/json) 3.12.0 (vendored in NeuralAmpModelerCore) | MIT | NAM model file parsing |
| [hidapi](https://github.com/libusb/hidapi) hidapi-0.15.0 (planned) | BSD | USB footswitch input |
| [Eigen](https://eigen.tuxfamily.org) 5.0.1, commit bc3b39870ecb690a623a3f49149a358b95c5781d (NeuralAmpModelerCore submodule) | MPL-2.0 (some files BSD/other MPL2-compatible) | Linear algebra |

## Test fixtures

| File | Source | License |
|---|---|---|
| `tests/fixtures/assets/tiny.nam` | NeuralAmpModelerCore `example_models/lstm.nam` (at the commit above), unmodified | MIT |
| `tests/fixtures/di/di_48k.wav` | Synthesized for this project (Karplus-Strong strummed chords) | AGPLv3 (project) |
