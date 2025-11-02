# Test configuration

find_package(Catch2 3 CONFIG REQUIRED)

# Unit tests
add_executable(unit_tests
    unit/test_main.cpp
    unit/test_com_wrapper.cpp
    unit/test_config_session.cpp
    unit/test_markdown_table_formatter.cpp
    unit/test_toon_encoder.cpp
    unit/test_connection_launcher.cpp
    unit/test_screenshot_handler.cpp
    unit/test_screen_reader.cpp
    unit/test_element_metadata_extractor.cpp
    ../src/sap_gui_base.cpp
    ../src/com/wrapper.cpp
    ../src/com/wrapper_helpers.cpp
    ../src/com/element.cpp
    ../src/com/window.cpp
    ../src/com/session.cpp
    ../src/com/connection.cpp
    ../src/com/application.cpp
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
    ../src/formatters/markdown_table_formatter.cpp
    ../src/formatters/toon_encoder.cpp
    ../src/string_utils.cpp
    ../src/base64.cpp
    ../src/semantic_classifier.cpp
    ../src/element_renderer_registry.cpp
    ../src/element_renderers.cpp
    ../src/grid_analyzer.cpp
    ../src/formatters/grid_renderer.cpp
    ../src/formatters/screen_markdown_formatter.cpp
    ../src/connection_launcher.cpp
    ../src/screenshot_handler.cpp
    ../src/screen_reader.cpp
    ../src/element_metadata_extractor.cpp
)

# Include directories for tests
target_include_directories(unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(unit_tests PRIVATE
    Catch2::Catch2WithMain
    CImg::CImg
    ZLIB::ZLIB
    PNG::PNG
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    fmt::fmt
    ole32
    oleaut32
    gdi32
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
