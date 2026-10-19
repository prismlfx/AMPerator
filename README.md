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
