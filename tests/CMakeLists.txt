# Test configuration

find_package(Catch2 3 CONFIG REQUIRED)

# Unit tests (link against shared fairyfly_core library to avoid recompilation)
add_executable(unit_tests
    unit/test_main.cpp
    unit/test_com_wrapper.cpp
    unit/test_config_session.cpp
    unit/test_markdown_table_formatter.cpp
    unit/test_toon_encoder.cpp
    unit/test_connection_launcher.cpp
    unit/test_login_flow.cpp
    unit/test_screenshot_handler.cpp
    unit/test_screen_reader.cpp
    unit/test_screen_read_usability.cpp
    unit/test_compact_json.cpp
    unit/test_html_viewer_reader.cpp
    unit/test_object_tree_diag.cpp
    unit/test_object_tree.cpp
    unit/test_screen_reader_bulk.cpp
    unit/test_element_metadata_extractor.cpp
    unit/test_element_metadata_builder.cpp
    unit/test_display_text_policy.cpp
    unit/test_raii_core_renderers.cpp
    unit/test_doctor.cpp
    unit/test_connection_manager.cpp
    unit/test_grid_analyzer.cpp
    unit/test_batch_command.cpp
    unit/test_credential_store.cpp
    unit/test_credential_resolver.cpp
    unit/test_audit_log.cpp
    unit/test_mcp_types.cpp
    unit/test_mcp_policy.cpp
    unit/test_mcp_json_rpc.cpp
    unit/test_mcp_protocol.cpp
    unit/test_mcp_tools.cpp
    unit/test_mcp_dispatcher.cpp
    unit/test_mcp_http.cpp
    unit/test_http_sys_mapping.cpp
    unit/test_command_table.cpp
    unit/test_setup_validators.cpp
    unit/test_auth.cpp
    unit/test_auth_policy.cpp
    unit/test_auth_scopes.cpp
    unit/test_mcp_config.cpp
    unit/test_mcp_client_config.cpp
    unit/test_mcp_doctor.cpp
    unit/test_mcp_setup.cpp
    unit/test_tray.cpp
    unit/test_mcp_tray_runner.cpp
    unit/test_mcp_tool_metadata.cpp
    unit/test_mcp_usability.cpp
    unit/test_mcp_selection_input.cpp
    unit/test_selection_input_guard.cpp
    unit/test_round2_usability.cpp
    unit/test_tray_win32_manual.cpp
    ../src/config.cpp
    ../src/session.cpp
)

# Small SAP-free child process used to verify cross-process cache locking.
add_executable(connection_cache_worker EXCLUDE_FROM_ALL unit/connection_cache_worker.cpp)
target_link_libraries(connection_cache_worker PRIVATE fairyfly_core)
add_dependencies(unit_tests connection_cache_worker)

# Recorded SAP screens (object trees and legacy reads) used by the bulk reader parity tests
target_compile_definitions(unit_tests PRIVATE FAIRYFLY_UNIT_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/unit/fixtures")

# Link against fairyfly_core library (all dependencies inherited)
target_link_libraries(unit_tests PRIVATE
    fairyfly_core
    Catch2::Catch2
)
if(WIN32)
    target_link_libraries(unit_tests PRIVATE shell32)
endif()

# Add test discovery
include(CTest)
include(Catch)
catch_discover_tests(unit_tests)

# VBScript comparison tests
add_subdirectory(vbs)

# Integration tests (will be added later when SAP components exist)
# add_executable(integration_tests
#     integration/test_sap_connection.cpp
# )
