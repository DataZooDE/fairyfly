# VBScript comparison test executable

add_executable(test_base_objects_cpp
    test_base_objects_cpp.cpp
)

target_link_libraries(test_base_objects_cpp PRIVATE fairyfly_core)
