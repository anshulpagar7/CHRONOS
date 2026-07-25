cmake_minimum_required(VERSION 3.20)

project(chronos
    VERSION 0.1.0
    DESCRIPTION "CHRONOS - a production-inspired distributed task scheduling engine"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CHRONOS_BUILD_TESTS "Build the CHRONOS test suite" ON)
option(CHRONOS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

# ---------------------------------------------------------------------------
# chronos::warnings -- interface target carrying our warning policy, so every
# target opts in with one line and third-party code (gtest) stays untouched.
# ---------------------------------------------------------------------------
add_library(chronos_warnings INTERFACE)
add_library(chronos::warnings ALIAS chronos_warnings)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(chronos_warnings INTERFACE
        -Wall -Wextra -Wpedantic
        -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
        -Wunused -Woverloaded-virtual -Wconversion -Wsign-conversion
        -Wdouble-promotion)
    if(CHRONOS_WARNINGS_AS_ERRORS)
        target_compile_options(chronos_warnings INTERFACE -Werror)
    endif()
endif()

# ---------------------------------------------------------------------------
# chronos_core -- the engine. Deliberately zero external dependencies:
# it links against nothing but the C++ standard library and pthreads.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

add_library(chronos_core
    src/core/job.cpp
    src/core/event_bus.cpp
    src/core/job_store.cpp
    src/scheduling/policies.cpp
    src/scheduling/ready_set.cpp
    src/scheduling/retry_manager.cpp
    src/scheduling/scheduler.cpp
    src/execution/worker_registry.cpp
    src/execution/local_thread_backend.cpp
    src/observability/log.cpp
    src/observability/metrics.cpp
    src/observability/scheduler_metrics.cpp
    src/sim/simulation.cpp
    src/api/json.cpp
    src/api/http_server.cpp
    src/api/api_server.cpp)
add_library(chronos::core ALIAS chronos_core)

target_include_directories(chronos_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(chronos_core
    PUBLIC Threads::Threads
    PRIVATE chronos::warnings)

# ---------------------------------------------------------------------------
# Demo / example apps
# ---------------------------------------------------------------------------
add_executable(day1_demo apps/day1_demo.cpp)
target_link_libraries(day1_demo PRIVATE chronos::core chronos::warnings)

add_executable(day2_policy_demo apps/day2_policy_demo.cpp)
target_link_libraries(day2_policy_demo PRIVATE chronos::core chronos::warnings)

add_executable(day3_demo apps/day3_demo.cpp)
target_link_libraries(day3_demo PRIVATE chronos::core chronos::warnings)

add_executable(chronos_sim apps/chronos_sim.cpp)
target_link_libraries(chronos_sim PRIVATE chronos::core chronos::warnings)
set_target_properties(chronos_sim PROPERTIES OUTPUT_NAME chronos-sim)

add_executable(chronosd apps/chronosd.cpp)
target_link_libraries(chronosd PRIVATE chronos::core chronos::warnings)

add_executable(chronos_bench apps/chronos_bench.cpp)
target_link_libraries(chronos_bench PRIVATE chronos::core chronos::warnings)
set_target_properties(chronos_bench PROPERTIES OUTPUT_NAME chronos-bench)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(CHRONOS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
