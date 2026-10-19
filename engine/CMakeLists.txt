add_library(amp_engine STATIC
  Engine.cpp
  Engine.h
  model/RigDocument.cpp
  model/RigDocument.h)

target_include_directories(amp_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(amp_engine PUBLIC cxx_std_20)
target_compile_definitions(amp_engine PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0
  JUCE_PLUGINHOST_VST3=1
  JUCE_PLUGINHOST_AU=1)
target_link_libraries(amp_engine PUBLIC
  juce::juce_audio_utils
  juce::juce_audio_processors
  juce::juce_dsp
  PRIVATE
  juce::juce_recommended_config_flags
  juce::juce_recommended_warning_flags)
