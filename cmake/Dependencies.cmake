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

# hidapi (BSD-3-Clause / GPLv3 / original HIDAPI license; used under BSD) - footswitch input.
# Static, no test tools. macOS backend uses IOKit + CoreFoundation (linked by engine/CMakeLists.txt too).
FetchContent_Declare(hidapi
  GIT_REPOSITORY https://github.com/libusb/hidapi.git
  GIT_TAG hidapi-0.15.0
  GIT_SHALLOW TRUE)
set(BUILD_SHARED_LIBS OFF)
set(HIDAPI_WITH_TESTS OFF CACHE BOOL "" FORCE)
set(HIDAPI_BUILD_HIDTEST OFF CACHE BOOL "" FORCE)
set(HIDAPI_BUILD_PP_DATA_DUMP OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(JUCE Catch2 NeuralAmpModelerCore hidapi)
if(TARGET hidapi_darwin AND NOT MSVC)
  target_compile_options(hidapi_darwin PRIVATE -w) # third-party: keep our build output clean
  set_target_properties(hidapi_darwin PROPERTIES XCODE_ATTRIBUTE_GCC_WARN_INHIBIT_ALL_WARNINGS YES)
endif()
