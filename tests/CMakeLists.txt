# Test configuration

find_package(Catch2 3 CONFIG REQUIRED)
find_package(ZXing CONFIG REQUIRED)  # decodes the QR codes link pair draws (test_link_console.cpp)

# Unit tests (link against shared fairyfly_core library to avoid recompilation)
add_executable(unit_tests
    unit/test_main.cpp
    unit/test_com_wrapper.cpp
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
    unit/test_bulk_fault_injection.cpp
    unit/test_read_only_popup_guard.cpp
    unit/test_element_error_codes.cpp
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
    unit/test_fill_fields.cpp
    unit/test_mcp_types.cpp
    unit/test_mcp_policy.cpp
    unit/test_environment.cpp
    unit/test_env_paths.cpp
    unit/test_core_result.cpp
    unit/test_string_utils.cpp
    unit/test_text_utf8.cpp
    unit/test_command_line.cpp
    unit/test_constants.cpp
    unit/test_sensitive_data.cpp
    unit/test_read_only_guard.cpp
    unit/test_sap_identity.cpp
    unit/test_command_handler.cpp
    unit/test_doctor_checks.cpp
    unit/test_screen_markdown_golden.cpp
    unit/test_semantic_classifier.cpp
    unit/test_session_lease_policy.cpp
    unit/test_grid_renderer.cpp
    unit/test_unique_handle.cpp
    unit/test_http_probe.cpp
    unit/test_engine_actions.cpp
    unit/test_login_handler.cpp
    unit/test_trail_screen_source.cpp
    unit/test_session_worker_main.cpp
    unit/test_mcp_config_command.cpp
    unit/test_mcp_json_rpc.cpp
    unit/test_mcp_protocol.cpp
    unit/test_mcp_tools.cpp
    unit/test_mcp_dispatcher.cpp
    unit/test_mcp_http.cpp
    unit/test_http_sys_mapping.cpp
    unit/test_command_table.cpp
    unit/test_link_bridge.cpp
    unit/test_link_control.cpp
    unit/test_desktop_approval.cpp
    unit/test_desktop_approval_win32.cpp
    unit/test_link_console.cpp
    unit/test_trail_recorder.cpp
    unit/test_update.cpp
    unit/test_update_version.cpp
    unit/test_packaging.cpp
    unit/test_command_registry_threads.cpp
    unit/test_agent_help.cpp
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
    unit/test_mcp_session_leases.cpp
    unit/test_mcp_session_executor_pool.cpp
    unit/test_mcp_session_worker_protocol.cpp
    unit/test_mcp_session_route_plan.cpp
    unit/test_mcp_session_route_coordinator.cpp
    unit/test_mcp_session_worker_process.cpp
    unit/test_window_owner.cpp
    unit/test_mcp_session_worker_broker.cpp
    unit/test_mcp_session_worker_provider.cpp
    unit/test_selection_input_guard.cpp
    unit/test_round2_usability.cpp
    unit/test_tray_win32_manual.cpp
)

# Small SAP-free child process used to verify cross-process cache locking.
add_executable(connection_cache_worker EXCLUDE_FROM_ALL unit/connection_cache_worker.cpp)
target_link_libraries(connection_cache_worker PRIVATE fairyfly_core)
add_dependencies(unit_tests connection_cache_worker)

add_executable(mcp_worker_fixture EXCLUDE_FROM_ALL unit/mcp_worker_fixture.cpp)
target_link_libraries(mcp_worker_fixture PRIVATE fairyfly_core)
add_dependencies(unit_tests mcp_worker_fixture)

# Shows the pairing QR code in a new console and reads the screen back (test_link_console.cpp).
if(WIN32)
    add_executable(link_console_fixture EXCLUDE_FROM_ALL unit/link_console_fixture.cpp)
    target_link_libraries(link_console_fixture PRIVATE fairyfly_core)
    add_dependencies(unit_tests link_console_fixture)
endif()
add_dependencies(unit_tests fairyfly)

# Manual live SAP gate: independent COM STAs in one process (read-only; excluded from normal tests).
add_executable(sap_sta_parallel_probe EXCLUDE_FROM_ALL integration/sap_sta_parallel_probe.cpp)
target_link_libraries(sap_sta_parallel_probe PRIVATE fairyfly_core)

# Recorded SAP screens (object trees and legacy reads) used by the bulk reader parity tests
target_compile_definitions(unit_tests PRIVATE FAIRYFLY_UNIT_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/unit/fixtures"
                                              FAIRYFLY_SOURCE_DIR="${PROJECT_SOURCE_DIR}/src")

# Link against fairyfly_core library (all dependencies inherited)
target_link_libraries(unit_tests PRIVATE
    fairyfly_core
    Catch2::Catch2
    ZXing::ZXing
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
