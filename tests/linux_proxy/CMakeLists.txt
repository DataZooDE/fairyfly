# fairyfly's portable proxy code end to end on Linux, against a real squid (tests/linux_proxy/run.sh; CI job
# "proxy-linux" in .github/workflows/build.yml). The Windows build is the main CMakeLists.txt; this builds only what is
# portable: libffl's HTTP/WebSocket/proxy code (vendored, unchanged) and fairyfly's proxy decisions.
cmake_minimum_required(VERSION 3.20)
project(fairyfly_linux_proxy CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(spdlog REQUIRED)
find_package(Catch2 3 REQUIRED)
find_package(Threads REQUIRED)

set(ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(FFL ${ROOT}/third_party/libffl)

add_library(ffl_net STATIC
    ${FFL}/src/util.cpp
    ${FFL}/src/proxy.cpp
    ${FFL}/src/http_client.cpp
    ${FFL}/src/ws_client.cpp)
target_include_directories(ffl_net PUBLIC ${FFL}/src)
target_include_directories(ffl_net SYSTEM PUBLIC ${FFL}/third_party/cpp-httplib)
target_compile_definitions(ffl_net PUBLIC CPPHTTPLIB_OPENSSL_SUPPORT CPPHTTPLIB_TCP_NODELAY=true
                                          CPPHTTPLIB_WEBSOCKET_MAX_PAYLOAD_LENGTH=8454144 CPPHTTPLIB_HEADER_MAX_LENGTH=65536)
target_link_libraries(ffl_net PUBLIC OpenSSL::SSL OpenSSL::Crypto nlohmann_json::nlohmann_json spdlog::spdlog Threads::Threads)

add_executable(linux_proxy_tests
    ${ROOT}/src/link/system_proxy.cpp
    ${ROOT}/src/link/proxy_config.cpp
    ${ROOT}/src/link/link_control.cpp
    ${ROOT}/src/system/process_probe.cpp
    ${ROOT}/tests/unit/test_proxy_config.cpp
    ${ROOT}/tests/unit/test_system_proxy.cpp
    test_proxy_squid.cpp)
target_include_directories(linux_proxy_tests PRIVATE ${ROOT}/src ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(linux_proxy_tests PRIVATE -Wall -Wextra)
target_link_libraries(linux_proxy_tests PRIVATE ffl_net Catch2::Catch2WithMain)
