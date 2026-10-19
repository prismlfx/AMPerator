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

# Wired in by Task 6: NeuralAmpModelerCore (MIT) - pin to a specific commit on main.
# FetchContent_Declare(NeuralAmpModelerCore
#   GIT_REPOSITORY https://github.com/sdatkinson/NeuralAmpModelerCore.git
#   GIT_TAG <commit-sha>)

# Wired in by Task 13: hidapi (BSD)
# FetchContent_Declare(hidapi
#   GIT_REPOSITORY https://github.com/libusb/hidapi.git
#   GIT_TAG hidapi-0.15.0)

FetchContent_MakeAvailable(JUCE Catch2)
