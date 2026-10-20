# NeuralAmpModelerCore inference sources only (no upstream tools). NAM_SAMPLE stays the default (double).
set(NAM_ROOT ${neuralampmodelercore_SOURCE_DIR})
file(GLOB NAM_CORE_SOURCES CONFIGURE_DEPENDS ${NAM_ROOT}/NAM/*.cpp ${NAM_ROOT}/NAM/wavenet/*.cpp)
add_library(nam_core STATIC ${NAM_CORE_SOURCES})
target_include_directories(nam_core SYSTEM PUBLIC
  ${NAM_ROOT}
  ${NAM_ROOT}/NAM
  ${NAM_ROOT}/Dependencies/eigen
  ${NAM_ROOT}/Dependencies/nlohmann)
target_compile_features(nam_core PUBLIC cxx_std_20)
target_compile_definitions(nam_core PUBLIC NAM_ENABLE_A2_FAST)
if(WIN32)
  target_compile_definitions(nam_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(NOT MSVC)
  target_compile_options(nam_core PRIVATE -w) # third-party: keep our build output clean
  set_target_properties(nam_core PROPERTIES XCODE_ATTRIBUTE_GCC_WARN_INHIBIT_ALL_WARNINGS YES)
  # Unoptimised Eigen is too slow to run real models in real time, even in Debug builds.
  target_compile_options(nam_core PRIVATE $<$<CONFIG:Debug>:-O2>)
endif()

add_library(amp_engine STATIC
  Engine.cpp
  Engine.h
  assets/AssetIndex.cpp
  assets/AssetIndex.h
  dsp/BuiltinNodes.cpp
  dsp/BuiltinNodes.h
  dsp/CabIRNode.cpp
  dsp/CabIRNode.h
  dsp/NamAmpNode.cpp
  dsp/NamAmpNode.h
  dsp/Node.h
  engine/ProcessingPlan.cpp
  engine/ProcessingPlan.h
  engine/RealtimeEngine.cpp
  engine/RealtimeEngine.h
  engine/RtSafety.cpp
  engine/RtSafety.h
  model/ChainGraph.cpp
  model/ChainGraph.h
  model/RigDocument.cpp
  model/RigDocument.h
  plugins/PluginHost.cpp
  plugins/PluginHost.h)

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
  juce::juce_cryptography
  juce::juce_audio_processors
  juce::juce_dsp
  PRIVATE
  # NAM registers its architectures (LSTM, WaveNet, ...) from static initialisers in otherwise
  # unreferenced objects; whole-archive keeps the linker from dropping them.
  $<LINK_LIBRARY:WHOLE_ARCHIVE,nam_core>
  juce::juce_recommended_config_flags
  juce::juce_recommended_warning_flags)
