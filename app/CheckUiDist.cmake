# Fails the build when the Vite output (DIST) differs from the file list the app embeds (EXPECTED, '|'-separated).
# See AMP_UI_FILES in app/CMakeLists.txt and ui/vite.config.ts.
string(REPLACE "|" ";" expected "${EXPECTED}")
file(GLOB_RECURSE actual RELATIVE "${DIST}" "${DIST}/*")
list(SORT expected)
list(SORT actual)
if(NOT expected STREQUAL actual)
  message(FATAL_ERROR
    "The UI build produced files the app does not embed.\n"
    "  expected: ${expected}\n"
    "  actual:   ${actual}\n"
    "Keep ui/vite.config.ts emitting exactly AMP_UI_FILES (app/CMakeLists.txt), or update that list.")
endif()
