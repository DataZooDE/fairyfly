# Test configuration

find_package(Catch2 3 CONFIG REQUIRED)

# Unit tests
add_executable(unit_tests
    unit/test_main.cpp
    unit/test_com_wrapper.cpp
    unit/test_config_session.cpp
    ../src/sap_gui_base.cpp
    ../src/com_wrapper.cpp
    ../src/com_automation_engine.cpp
    ../src/automation_engine.cpp
    ../src/trace.cpp
    ../src/config.cpp
    ../src/session.cpp
    ../src/cli_handler.cpp
    ../src/connection_manager.cpp
    ../src/element_type_registry.cpp
    ../src/screen_element_collector.cpp
    ../src/table_data_extractor.cpp
    ../src/formatters/tree_formatter.cpp
    ../src/formatters/table_formatter.cpp
)

# Include directories for tests
target_include_directories(unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(unit_tests PRIVATE
    Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    fmt::fmt
    ole32
    oleaut32
)

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
