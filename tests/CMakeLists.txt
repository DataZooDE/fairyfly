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
    unit/test_compact_json.cpp
    unit/test_element_metadata_extractor.cpp
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
    ../src/config.cpp
    ../src/session.cpp
)

# Small SAP-free child process used to verify cross-process cache locking.
add_executable(connection_cache_worker EXCLUDE_FROM_ALL unit/connection_cache_worker.cpp)
target_link_libraries(connection_cache_worker PRIVATE fairyfly_core)
add_dependencies(unit_tests connection_cache_worker)

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
