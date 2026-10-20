juce_add_gui_app(AMPerator
  PRODUCT_NAME "AMPerator"
  COMPANY_NAME "AMPerator"
  BUNDLE_ID org.amperator.app
  MICROPHONE_PERMISSION_ENABLED TRUE
  PLIST_TO_MERGE [[<plist><dict>
    <key>NSInputMonitoringUsageDescription</key>
    <string>AMPerator reads your USB footswitch so pedal presses control the rig even when another app is in front.</string>
  </dict></plist>]])

# ---- Web UI (ui/): built with Vite and embedded as binary data -----------------------------------------------
# npm ci runs when ui/node_modules is missing (or package-lock.json changed); npm run build (tsc + vite) whenever a
# UI source changes. Either failing fails the app build. The embedded file list is fixed (ui/vite.config.ts emits
# stable names); CheckUiDist.cmake fails the build if the Vite output stops matching AMP_UI_FILES.
find_program(AMP_NODE_EXECUTABLE node)
find_program(AMP_NPM_EXECUTABLE npm)
if(NOT AMP_NODE_EXECUTABLE OR NOT AMP_NPM_EXECUTABLE)
  message(FATAL_ERROR "Node.js and npm are required to build the AMPerator UI (ui/). Install Node.js 20+ and re-run CMake.")
endif()
get_filename_component(_amp_node_dir "${AMP_NODE_EXECUTABLE}" DIRECTORY)
set(_amp_npm ${CMAKE_COMMAND} -E env "PATH=${_amp_node_dir}:$ENV{PATH}" "${AMP_NPM_EXECUTABLE}")

set(AMP_UI_DIR "${PROJECT_SOURCE_DIR}/ui")
set(AMP_UI_DIST "${CMAKE_CURRENT_BINARY_DIR}/ui-dist")
set(AMP_UI_FILES index.html assets/index.js assets/index.css)
list(TRANSFORM AMP_UI_FILES PREPEND "${AMP_UI_DIST}/" OUTPUT_VARIABLE AMP_UI_OUTPUTS)
string(REPLACE ";" "|" _amp_ui_expected "${AMP_UI_FILES}")
file(GLOB_RECURSE AMP_UI_SOURCES CONFIGURE_DEPENDS "${AMP_UI_DIR}/src/*")

add_custom_command(
  OUTPUT "${AMP_UI_DIR}/node_modules/.package-lock.json"
  COMMAND ${_amp_npm} ci --no-audit --no-fund
  DEPENDS "${AMP_UI_DIR}/package-lock.json" "${AMP_UI_DIR}/package.json"
  WORKING_DIRECTORY "${AMP_UI_DIR}"
  COMMENT "Installing UI dependencies (npm ci)"
  VERBATIM)
add_custom_command(
  OUTPUT ${AMP_UI_OUTPUTS}
  COMMAND ${_amp_npm} run build -- --outDir "${AMP_UI_DIST}" --emptyOutDir
  COMMAND ${CMAKE_COMMAND} "-DDIST=${AMP_UI_DIST}" "-DEXPECTED=${_amp_ui_expected}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckUiDist.cmake"
  DEPENDS
    "${AMP_UI_DIR}/node_modules/.package-lock.json"
    ${AMP_UI_SOURCES}
    "${AMP_UI_DIR}/index.html" "${AMP_UI_DIR}/vite.config.ts" "${AMP_UI_DIR}/tsconfig.json" "${AMP_UI_DIR}/package.json"
  WORKING_DIRECTORY "${AMP_UI_DIR}"
  COMMENT "Building the web UI (npm run build)"
  VERBATIM)
add_custom_target(amp_ui DEPENDS ${AMP_UI_OUTPUTS})

juce_add_binary_data(amp_ui_data HEADER_NAME AmpUiData.h NAMESPACE AmpUiData SOURCES ${AMP_UI_OUTPUTS})
add_dependencies(amp_ui_data amp_ui)

# ---- App ------------------------------------------------------------------------------------------------------
target_sources(AMPerator PRIVATE Main.cpp MainWindow.cpp MainWindow.h)
target_compile_definitions(AMPerator PRIVATE
  JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:AMPerator,JUCE_PRODUCT_NAME>"
  JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}")
target_link_libraries(AMPerator PRIVATE
  amp_engine # brings amp_juce: the JUCE modules are compiled there, not here
  amp_ui_data
  juce::juce_recommended_config_flags
  juce::juce_recommended_warning_flags)
