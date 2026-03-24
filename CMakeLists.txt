cmake_minimum_required(VERSION 3.20)
project(asgard_matching_engine CXX C)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type")

# ---------------------------------------------------------------------------
# FetchContent dependencies
# ---------------------------------------------------------------------------
include(FetchContent)

# nlohmann/json (header-only)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
    DOWNLOAD_NO_EXTRACT TRUE
    DOWNLOAD_DIR ${CMAKE_CURRENT_BINARY_DIR}/include/nlohmann)
FetchContent_MakeAvailable(nlohmann_json)

# moodycamel ConcurrentQueue (header-only)
FetchContent_Declare(concurrentqueue
    GIT_REPOSITORY https://github.com/cameron314/concurrentqueue.git
    GIT_TAG        v1.0.4
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(concurrentqueue)

# Catch2 v3 (testing)
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.4
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(Catch2)

# uSockets (C runtime for uWebSockets)
FetchContent_Declare(uSockets
    GIT_REPOSITORY https://github.com/uNetworking/uSockets.git
    GIT_TAG        v0.8.8
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(uSockets)

file(GLOB USOCKETS_SRCS
    ${usockets_SOURCE_DIR}/src/*.c
    ${usockets_SOURCE_DIR}/src/eventing/*.c)
add_library(uSockets_lib STATIC ${USOCKETS_SRCS})
target_include_directories(uSockets_lib PUBLIC ${usockets_SOURCE_DIR}/src)
target_compile_definitions(uSockets_lib PUBLIC LIBUS_NO_SSL)

# uWebSockets v20 (header-only C++ wrapper over uSockets)
FetchContent_Declare(uWebSockets
    GIT_REPOSITORY https://github.com/uNetworking/uWebSockets.git
    GIT_TAG        v20.62.0
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(uWebSockets)

# ---------------------------------------------------------------------------
# Include paths
# ---------------------------------------------------------------------------
set(NLOHMANN_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)
set(CONCURRENTQUEUE_INCLUDE ${concurrentqueue_SOURCE_DIR})
set(UWEBSOCKETS_INCLUDE ${uwebsockets_SOURCE_DIR}/src)
set(USOCKETS_INCLUDE ${usockets_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# Engine library (core matching engine — no API or simulation deps)
# ---------------------------------------------------------------------------
add_library(engine_lib STATIC
    engine/orderbook.cpp
    engine/sequencer.cpp
    engine/margin.cpp
    engine/insurance_vault.cpp
    engine/risk_checks.cpp
    engine/matching.cpp
    engine/positions.cpp
    engine/stop_orders.cpp
    engine/risk_monitor.cpp
    engine/liquidation.cpp
    engine/circuit_breakers.cpp)

target_include_directories(engine_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${NLOHMANN_INCLUDE}
    ${CONCURRENTQUEUE_INCLUDE})

# ---------------------------------------------------------------------------
# Main simulation executable
# ---------------------------------------------------------------------------
add_executable(sim
    main.cpp
    simulation/market_price.cpp
    simulation/order_generator.cpp
    simulation/user_simulator.cpp
    api/subscriptions.cpp
    api/broadcaster.cpp
    api/websocket_server.cpp
    stats/stats_collector.cpp)

target_include_directories(sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${NLOHMANN_INCLUDE}
    ${CONCURRENTQUEUE_INCLUDE}
    ${UWEBSOCKETS_INCLUDE}
    ${USOCKETS_INCLUDE})

target_compile_definitions(sim PRIVATE LIBUS_NO_SSL)
target_link_libraries(sim PRIVATE engine_lib uSockets_lib pthread z)
if(APPLE)
    target_link_libraries(sim PRIVATE "-framework CoreFoundation" "-framework Security")
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
add_executable(tests
    tests/test_orderbook.cpp
    tests/test_matching.cpp
    tests/test_risk.cpp
    tests/test_margin.cpp)

target_include_directories(tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${NLOHMANN_INCLUDE}
    ${CONCURRENTQUEUE_INCLUDE})

target_link_libraries(tests PRIVATE engine_lib Catch2::Catch2WithMain pthread)

include(CTest)
include(Catch)
catch_discover_tests(tests)

# ---------------------------------------------------------------------------
# Latency benchmark
# ---------------------------------------------------------------------------
add_executable(bench_latency bench/bench_latency.cpp)

target_include_directories(bench_latency PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${NLOHMANN_INCLUDE}
    ${CONCURRENTQUEUE_INCLUDE})

target_link_libraries(bench_latency PRIVATE engine_lib pthread)

# ---------------------------------------------------------------------------
# Compiler flags
# ---------------------------------------------------------------------------
foreach(tgt engine_lib sim tests bench_latency)
    target_compile_options(${tgt} PRIVATE
        $<$<CONFIG:Release>:-O3 -march=native>
        $<$<CONFIG:Debug>:-g -fsanitize=address>
        -Wall -Wextra -Wpedantic)
endforeach()
