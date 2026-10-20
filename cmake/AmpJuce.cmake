# amp_juce: the one static library that compiles the JUCE modules used by the project, with one consistent set of
# JUCE_* options. amp_engine, the AMPerator app and amp_tests link it (through amp_engine) and never compile JUCE
# module sources themselves, so every JUCE symbol exists exactly once and every translation unit sees the same
# configuration.
#
# JUCE's module targets (juce::juce_*) are INTERFACE libraries that add the module sources to whoever links them.
# They are linked PRIVATE here, so the sources compile only into amp_juce; the module include paths and compile
# definitions (JUCE_MODULE_AVAILABLE_*, the options below, ...) are re-exported to dependents through the
# INTERFACE properties, and the modules' system frameworks/libraries propagate as link-only dependencies.
# This mirrors how juce_add_plugin shares its 'shared code' static library with the plugin wrapper targets.
add_library(amp_juce STATIC)
target_link_libraries(amp_juce
  PRIVATE
  juce::juce_audio_utils
  juce::juce_audio_processors
  juce::juce_dsp
  juce::juce_cryptography
  juce::juce_gui_extra
  juce::juce_recommended_config_flags)
target_compile_definitions(amp_juce PRIVATE
  JUCE_WEB_BROWSER=1          # the app's UI is a WebBrowserComponent (WKWebView on macOS)
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0
  JUCE_PLUGINHOST_VST3=1
  JUCE_PLUGINHOST_AU=1)
target_include_directories(amp_juce INTERFACE $<TARGET_PROPERTY:amp_juce,INCLUDE_DIRECTORIES>)
target_compile_definitions(amp_juce INTERFACE $<TARGET_PROPERTY:amp_juce,COMPILE_DEFINITIONS>)
target_compile_features(amp_juce PUBLIC cxx_std_20)
if(NOT MSVC)
  target_compile_options(amp_juce PRIVATE -w) # third-party: keep our build output clean
  set_target_properties(amp_juce PROPERTIES XCODE_ATTRIBUTE_GCC_WARN_INHIBIT_ALL_WARNINGS YES)
endif()
