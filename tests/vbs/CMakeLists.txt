# VBScript comparison test executable

add_executable(test_base_objects_cpp
    test_base_objects_cpp.cpp
    ../../src/sap_gui_base.cpp
    ../../src/com/wrapper.cpp
    ../../src/com/wrapper_helpers.cpp
    ../../src/com/element.cpp
    ../../src/com/window.cpp
    ../../src/com/session.cpp
    ../../src/com/connection.cpp
    ../../src/com/application.cpp
    ../../src/trace.cpp
)

target_include_directories(test_base_objects_cpp PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(test_base_objects_cpp PRIVATE
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    fmt::fmt
    ole32
    oleaut32
)
