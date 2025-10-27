# VBScript comparison test executable

add_executable(test_base_objects_cpp
    test_base_objects_cpp.cpp
    ../../src/sap_gui_base.cpp
    ../../src/com_wrapper.cpp
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
