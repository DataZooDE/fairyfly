# Set vcpkg triplet for static linking
set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")

# Set toolchain file - must be before project() call
if(DEFINED ENV{VCPKG_ROOT})
    file(TO_CMAKE_PATH "$ENV{VCPKG_ROOT}" VCPKG_ROOT_PATH)
    set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT_PATH}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "" FORCE)
else()
    set(CMAKE_TOOLCHAIN_FILE "C:/Users/jr/Desktop/Projects/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "" FORCE)
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
project(fairyfly VERSION 0.1.0 LANGUAGES CXX)

# Require C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)

# Platform-specific settings
if(WIN32)
    # Windows-specific settings for COM support
    add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN)
endif()

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/src)

# Source files
set(FAIRYFLY_SOURCES
    src/main.cpp
    src/automation_engine.cpp
    src/com_wrapper.cpp
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
)

# Create executable
add_executable(fairyfly ${FAIRYFLY_SOURCES})

# Link libraries
target_link_libraries(fairyfly PRIVATE
    CLI11::CLI11
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    fmt::fmt
)

# Windows-specific COM libraries
if(WIN32)
    target_link_libraries(fairyfly PRIVATE
        ole32
        oleaut32
    )
endif()

# Compiler warnings
if(MSVC)
    target_compile_options(fairyfly PRIVATE /W4)
else()
    target_compile_options(fairyfly PRIVATE -Wall -Wextra -Wpedantic)
endif()

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
