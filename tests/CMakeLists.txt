include(Catch)

add_executable(amp_tests main.cpp smoke_test.cpp asset_index_test.cpp rig_document_test.cpp chain_graph_test.cpp builtin_nodes_test.cpp nam_amp_test.cpp cab_ir_test.cpp realtime_engine_test.cpp plugin_host_test.cpp rig_store_test.cpp settings_test.cpp rig_loader_test.cpp audio_io_test.cpp control_input_test.cpp mapping_engine_test.cpp tuner_test.cpp bridge_test.cpp bridge_online_test.cpp audio_regression_test.cpp nam_a2_test.cpp online_pkce_test.cpp online_secure_store_test.cpp online_auth_test.cpp online_client_test.cpp online_provider_index_test.cpp online_placement_test.cpp online_download_test.cpp
  support/OfflineRender.cpp support/OfflineRender.h support/FakePlugins.cpp support/FakePlugins.h support/FakeAudioDevice.cpp support/FakeAudioDevice.h support/FakeHttp.cpp support/FakeHttp.h support/BridgeFixture.h)
target_link_libraries(amp_tests PRIVATE amp_engine Catch2::Catch2)
target_compile_definitions(amp_tests PRIVATE AMP_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_compile_definitions(amp_tests PRIVATE AMP_NAM_EXAMPLES_DIR="${neuralampmodelercore_SOURCE_DIR}/example_models")
if(APPLE)
  # Apple Silicon kills unsigned binaries (the Xcode generator leaves command-line tools unsigned);
  # ad-hoc sign before catch_discover_tests runs the executable.
  add_custom_command(TARGET amp_tests POST_BUILD
    COMMAND codesign --force --sign - "$<TARGET_FILE:amp_tests>")
endif()
catch_discover_tests(amp_tests)
