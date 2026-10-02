cmake_minimum_required(VERSION 3.20)

project(cpp_api_gateway
    VERSION 1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost REQUIRED)

add_executable(api_gateway
    src/main.cpp
)

target_link_libraries(api_gateway
    PRIVATE
    Boost::headers
    ws2_32
    mswsock
)


# -------------------------
# Tests
# -------------------------

enable_testing()

add_executable(load_balancer_test
    tests/load_balancer_test.cpp
)

target_include_directories(load_balancer_test
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_test(
    NAME LoadBalancerTest
    COMMAND load_balancer_test
)