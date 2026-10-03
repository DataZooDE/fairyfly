# Set vcpkg triplet for static linking
set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")

# Set toolchain file - must be before project() call
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT})
        file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" VCPKG_ROOT_PATH)
        set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT_PATH}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "" FORCE)
    elseif(EXISTS "C:/Users/jr/Desktop/Projects/vcpkg/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "C:/Users/jr/Desktop/Projects/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "" FORCE)
    endif()
endif()

cmake_minimum_required(VERSION 3.20)

# Set vcpkg root for package location
if(NOT DEFINED VCPKG_INSTALLED_DIR)
    set(VCPKG_INSTALLED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed" CACHE STRING "vcpkg installed directory")
endif()

# Force static linking on Windows
if(WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Set the project name and version
project(fairyfly VERSION 2026.9.30 LANGUAGES CXX)

# Require C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable SEH (Structured Exception Handling) for catching OS-level exceptions
# /EHa enables both C++ exceptions and SEH with proper unwinding
if(MSVC)
    add_compile_options(/EHa)
    # Enable multiprocessor compilation - uses all available CPU cores
    add_compile_options(/MP)
endif()

# Group generated projects in Visual Studio
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Use ccache if it is available with the selected generator
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "Using ccache: ${CCACHE_PROGRAM}")
endif()

# Set default build type to Release if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Find packages via vcpkg
find_package(CLI11 CONFIG REQUIRED)
find_package(CImg CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(PNG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)

# Platform-specific settings
if(WIN32)
    # Windows-specific settings for COM support
    add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# Enable PNG support in CImg
add_compile_definitions(cimg_use_png)

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/src)

# Core library sources (shared between main executable and tests)
set(FAIRYFLY_LIB_SOURCES
    src/action_status.cpp
    src/field_fill_info.cpp
    src/tab_guard.cpp
    src/server_clock.cpp
    src/read_only_guard.cpp
    src/cli_entry.cpp
    src/audit_log.cpp
    src/cli_handler_audit.cpp
    src/automation_engine.cpp
    src/com/wrapper.cpp
    src/com/wrapper_helpers.cpp
    src/com/element.cpp
    src/com/window.cpp
    src/com/session.cpp
    src/com/connection.cpp
    src/com/application.cpp
    src/com_automation_engine.cpp
    src/trace.cpp
    src/cli_handler.cpp
    src/connection_manager.cpp
    src/sap_gui_base.cpp
    src/element_type_registry.cpp
    src/screen_element_collector.cpp
    src/table_data_extractor.cpp
    src/formatters/tree_formatter.cpp
    src/formatters/table_formatter.cpp
    src/formatters/markdown_table_formatter.cpp
    src/formatters/toon_encoder.cpp
    src/string_utils.cpp
    src/base64.cpp
    src/semantic_classifier.cpp
    src/sensitive_data.cpp
    src/html_viewer_reader.cpp
    src/object_tree_diag.cpp
    src/object_tree.cpp
    src/element_renderer_registry.cpp
    src/element_renderers.cpp
    src/grid_analyzer.cpp
    src/formatters/grid_renderer.cpp
    src/formatters/screen_markdown_formatter.cpp
    src/formatters/compact_json.cpp
    src/connection_launcher.cpp
    src/login_flow.cpp
    src/screenshot_handler.cpp
    src/screen_reader.cpp
    src/element_metadata_extractor.cpp
    src/commands/command_registry.cpp
    src/commands/attach_command.cpp
    src/commands/launch_command.cpp
    src/commands/login_command.cpp
    src/commands/disconnect_command.cpp
    src/commands/connections_command.cpp
    src/commands/tcode_command.cpp
    src/commands/click_command.cpp
    src/commands/fill_command.cpp
    src/commands/get_command.cpp
    src/commands/list_command.cpp
    src/commands/screen_command.cpp
    src/commands/mcp_command.cpp
    src/commands/menu_command.cpp
    src/commands/command_groups.cpp
    src/commands/cli_app.cpp
    src/command_table.cpp
    src/commands/press_f4_command.cpp
    src/commands/doctor_command.cpp
    src/commands/send_key_command.cpp
    src/commands/close_command.cpp
    src/commands/batch_command.cpp
    src/com_automation_engine_actions.cpp
    src/cli_handler_window_actions.cpp
    src/cli_handler_credentials.cpp
    src/credential_store.cpp
    src/credential_resolver.cpp
    src/console_prompt.cpp
    src/commands/credentials_command.cpp
    src/menu_navigation.cpp
    src/mcp/json_rpc.cpp
    src/mcp/transport.cpp
    src/mcp/server.cpp
    src/mcp/policy.cpp
    src/mcp/tool_catalog.cpp
    src/mcp/tool_catalog_write.cpp
    src/mcp/result_shaper.cpp
    src/mcp/dispatcher.cpp
    src/mcp/mcp_audit.cpp
    src/mcp/run_mcp.cpp
    src/mcp/http_tray_runner.cpp
    src/system/powershell_runner.cpp
    src/system/cert_scripts.cpp
    src/setup/setup_validators.cpp
    src/setup/setup_model.cpp
    src/setup/setup_service.cpp
    src/setup/http_sys_config_win.cpp
    src/setup/cert_store_win.cpp
    src/setup/firewall_win.cpp
    src/setup/elevator_win.cpp
    src/setup/system_probe_win.cpp
    src/setup/winhttp_client.cpp
    src/setup/verify_win.cpp
    src/setup/setup_doctor.cpp
    src/auth/crypto.cpp
    src/auth/ip.cpp
    src/auth/secret_backend.cpp
    src/auth/token_store.cpp
    src/auth/authenticator.cpp
    src/auth/scopes.cpp
    src/auth/authorize.cpp
    src/auth/token_cli.cpp
    src/commands/mcp_token_command.cpp
    src/mcp/call_executor.cpp
    src/mcp/protocol_session.cpp
    src/mcp/http_endpoint.cpp
    src/mcp/http_sys_mapping.cpp
    src/mcp/http_sys_server.cpp
    src/mcp/http_auth.cpp
    src/config/mcp_config.cpp
    src/config/client_config.cpp
    src/config/mcp_doctor.cpp
    src/tray/tray_model.cpp
    src/tray/tray_controller.cpp
    src/tray/tray_win32.cpp
    src/commands/mcp_config_command.cpp
    src/commands/mcp_client_config_command.cpp
    src/commands/mcp_doctor_command.cpp
    src/commands/mcp_setup_command.cpp
)

# Create static library for core functionality (compiled once, linked by both exe and tests)
add_library(fairyfly_core STATIC ${FAIRYFLY_LIB_SOURCES})

if(MSVC)
    target_precompile_headers(fairyfly_core PRIVATE
        <nlohmann/json.hpp>
        <spdlog/spdlog.h>
        <fmt/format.h>
    )
endif()

# Set library properties
target_include_directories(fairyfly_core PUBLIC ${CMAKE_SOURCE_DIR}/src)

# Link library dependencies
target_link_libraries(fairyfly_core PUBLIC
    CLI11::CLI11
    CImg::CImg
    ZLIB::ZLIB
    PNG::PNG
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    fmt::fmt
    yaml-cpp::yaml-cpp
)

# Windows-specific COM libraries
if(WIN32)
    target_link_libraries(fairyfly_core PUBLIC
        ole32
        oleaut32
        gdi32
        user32
        uiautomationcore
        advapi32
        bcrypt
        ws2_32
        httpapi
        crypt32
        winhttp
        shell32
        ws2_32
        wtsapi32
    )
endif()

# Compiler warnings for library
if(MSVC)
    target_compile_options(fairyfly_core PRIVATE /W4)
else()
    target_compile_options(fairyfly_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Create executable (only main.cpp, links against library)
if(WIN32)
    # Tray icon resources (assets/icons via src/tray/fairyfly.rc)
    enable_language(RC)
    # Version information (ProductName "fairyfly", calendar version) embedded in fairyfly.exe: used for code signing
    # (product name = project name, product version consistent across builds).
    set(FAIRYFLY_VER_MAJOR ${PROJECT_VERSION_MAJOR})
    set(FAIRYFLY_VER_MINOR ${PROJECT_VERSION_MINOR})
    set(FAIRYFLY_VER_PATCH ${PROJECT_VERSION_PATCH})
    string(CONCAT FAIRYFLY_VERSION_STRING "${PROJECT_VERSION_MAJOR}." "${PROJECT_VERSION_MINOR}." "${PROJECT_VERSION_PATCH}")
    if(PROJECT_VERSION_MINOR LESS 10 OR PROJECT_VERSION_PATCH LESS 10)
        # keep the zero-padded calendar form of FAIRYFLY_VERSION (2026.09.30)
        math(EXPR _ff_minor "${PROJECT_VERSION_MINOR}")
        math(EXPR _ff_patch "${PROJECT_VERSION_PATCH}")
        if(_ff_minor LESS 10)
            set(_ff_minor "0${_ff_minor}")
        endif()
        if(_ff_patch LESS 10)
            set(_ff_patch "0${_ff_patch}")
        endif()
        set(FAIRYFLY_VERSION_STRING "${PROJECT_VERSION_MAJOR}.${_ff_minor}.${_ff_patch}")
    endif()
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/version_info.rc.in ${CMAKE_CURRENT_BINARY_DIR}/version_info.rc @ONLY)
    add_executable(fairyfly src/main.cpp src/tray/fairyfly.rc ${CMAKE_CURRENT_BINARY_DIR}/version_info.rc)
else()
    add_executable(fairyfly src/main.cpp)
endif()

# Link executable against core library (all dependencies inherited)
target_link_libraries(fairyfly PRIVATE fairyfly_core)

# The Visual Studio output path can have path-specific SAP GUI ROT attachment
# problems on this workstation. Stage the runnable binary away from build
# intermediates after every link; copy_if_different keeps this inexpensive.
add_custom_command(TARGET fairyfly POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:fairyfly>"
            "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/$<TARGET_FILE_NAME:fairyfly>"
    VERBATIM
)

# Enable testing
enable_testing()

# Add subdirectories
add_subdirectory(tests)

# Installation rules
install(TARGETS fairyfly
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

# CPack configuration for packaging
set(CPACK_PACKAGE_NAME "fairyfly")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "LLM-powered SAP GUI automation CLI")
set(CPACK_PACKAGE_VENDOR "fairyfly")
include(CPack)
