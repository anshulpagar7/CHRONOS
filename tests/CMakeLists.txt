# GoogleTest, pinned. FetchContent keeps the repo clean of vendored code
# while CI stays reproducible.
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    URL_HASH SHA256=1f357c27ca988c3f7c6b4bf68a9395005ac6761f034046e9dde0896e3aba00e4
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)

# We don't use gmock (yet), and gtest must not inherit our -Werror policy.
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(chronos_unit_tests
    unit/ids_test.cpp
    unit/clock_test.cpp
    unit/job_test.cpp
    unit/event_bus_test.cpp
    unit/job_store_test.cpp
    unit/policies_test.cpp
    unit/ready_set_test.cpp
    unit/retry_manager_test.cpp
    unit/worker_registry_test.cpp
    unit/scheduler_test.cpp
    unit/metrics_test.cpp
    unit/scheduler_metrics_test.cpp
    unit/timeline_recorder_test.cpp
    unit/simulation_test.cpp
    unit/json_test.cpp
    unit/http_router_test.cpp)

target_link_libraries(chronos_unit_tests
    PRIVATE chronos::core chronos::warnings GTest::gtest_main)

add_executable(chronos_integration_tests
    integration/scheduler_integration_test.cpp
    integration/api_test.cpp
    integration/stress_test.cpp)

target_link_libraries(chronos_integration_tests
    PRIVATE chronos::core chronos::warnings GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(chronos_unit_tests DISCOVERY_TIMEOUT 30)
gtest_discover_tests(chronos_integration_tests DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 60)
