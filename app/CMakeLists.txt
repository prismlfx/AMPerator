juce_add_gui_app(AMPerator
  PRODUCT_NAME "AMPerator"
  COMPANY_NAME "AMPerator"
  BUNDLE_ID org.amperator.app
  MICROPHONE_PERMISSION_ENABLED TRUE
  PLIST_TO_MERGE [[<plist><dict>
    <key>NSInputMonitoringUsageDescription</key>
    <string>AMPerator reads your USB footswitch so pedal presses control the rig even when another app is in front.</string>
  </dict></plist>]])

target_sources(AMPerator PRIVATE Main.cpp)
target_compile_definitions(AMPerator PRIVATE
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:AMPerator,JUCE_PRODUCT_NAME>"
  JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}")
target_link_libraries(AMPerator PRIVATE
  amp_engine
  juce::juce_gui_extra
  juce::juce_recommended_config_flags
  juce::juce_recommended_warning_flags)
