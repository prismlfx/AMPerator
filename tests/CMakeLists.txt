include(Catch)

add_executable(amp_tests smoke_test.cpp asset_index_test.cpp rig_document_test.cpp chain_graph_test.cpp)
target_link_libraries(amp_tests PRIVATE amp_engine Catch2::Catch2WithMain)
target_compile_definitions(amp_tests PRIVATE AMP_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
if(APPLE)
  # Apple Silicon kills unsigned binaries (the Xcode generator leaves command-line tools unsigned);
  # ad-hoc sign before catch_discover_tests runs the executable.
  add_custom_command(TARGET amp_tests POST_BUILD
    COMMAND codesign --force --sign - "$<TARGET_FILE:amp_tests>")
endif()
catch_discover_tests(amp_tests)
