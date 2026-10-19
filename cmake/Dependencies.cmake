include(FetchContent)

# JUCE (AGPLv3) - newest 8.0.x tag
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.15
  GIT_SHALLOW TRUE)

# Catch2 (BSL-1.0)
FetchContent_Declare(Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.8.1
  GIT_SHALLOW TRUE)

# NeuralAmpModelerCore (MIT) - commit on main (2026-09-18, "Instantiate (linear) models from .wav files (#337)", v0.6.0+).
# Only the Eigen submodule is pulled; it is pinned by this commit's gitlink to
# Eigen (MPL-2.0) bc3b39870ecb690a623a3f49149a358b95c5781d. nlohmann/json ships vendored in the repo.
# SOURCE_SUBDIR points at a directory without a CMakeLists.txt so MakeAvailable only downloads:
# upstream's CMakeLists builds its tools; engine/CMakeLists.txt compiles just NAM/*.cpp + NAM/wavenet/*.cpp.
FetchContent_Declare(NeuralAmpModelerCore
  GIT_REPOSITORY https://github.com/sdatkinson/NeuralAmpModelerCore.git
  GIT_TAG 0b3d3c97b0859a3a8c92a8628c4dd89a25eb5842
  GIT_SUBMODULES Dependencies/eigen
  SOURCE_SUBDIR amp-no-cmake)

# Wired in by Task 13: hidapi (BSD)
# FetchContent_Declare(hidapi
#   GIT_REPOSITORY https://github.com/libusb/hidapi.git
#   GIT_TAG hidapi-0.15.0)

FetchContent_MakeAvailable(JUCE Catch2 NeuralAmpModelerCore)
