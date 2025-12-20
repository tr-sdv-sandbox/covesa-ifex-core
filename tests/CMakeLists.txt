cmake_minimum_required(VERSION 3.16)

# =============================================================================
# Test service binaries (required for integration tests)
# =============================================================================
add_subdirectory(test-types)

# Unit tests
add_executable(ifex-unit-tests
    unit/test_main.cpp
    unit/test_parser.cpp
    unit/test_namespace_parsing.cpp
    unit/test_settings_serialization.cpp
)

target_link_libraries(ifex-unit-tests
    PRIVATE
        ifex-core
        ifex-proto-generated
        GTest::gtest
        GTest::gtest_main
        glog::glog
        yaml-cpp
        nlohmann_json::nlohmann_json
        gRPC::grpc++
        protobuf::libprotobuf
)

target_include_directories(ifex-unit-tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_BINARY_DIR}/proto
)

# Integration tests
add_executable(ifex-tests-integration
    integration/test_main.cpp
    integration/test_fixture.cpp
    integration/test_discovery.cpp
    integration/test_dispatcher.cpp
    integration/test_comprehensive.cpp
    integration/test_types_comprehensive.cpp
    integration/test_scheduler.cpp
    integration/test_full_chain.cpp
)

target_link_libraries(ifex-tests-integration
    PRIVATE
        ifex-proto-generated
        GTest::gtest
        GTest::gtest_main
        glog::glog
        gRPC::grpc++
        protobuf::libprotobuf
        nlohmann_json::nlohmann_json
)

target_include_directories(ifex-tests-integration
    PRIVATE
        ${CMAKE_BINARY_DIR}/proto
        ${CMAKE_CURRENT_SOURCE_DIR}/integration
)

# Add tests to CTest
add_test(NAME unit_tests COMMAND ifex-unit-tests)
add_test(NAME integration_tests COMMAND ifex-tests-integration)

# Enable test discovery for CLion (unit tests only)
# Note: Integration tests must run as a suite (integration_tests) because they
# share a global test fixture that manages service lifecycle. Individual test
# discovery would cause each test to run in isolation without services.
include(GoogleTest)
gtest_discover_tests(ifex-unit-tests)

# Custom target to run just unit tests
add_custom_target(run-unit-tests
    COMMAND ${CMAKE_CTEST_COMMAND} -R unit_tests --output-on-failure
    DEPENDS ifex-unit-tests
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Custom target to run just integration tests
add_custom_target(run-integration-tests
    COMMAND ${CMAKE_CTEST_COMMAND} -R integration_tests --output-on-failure
    DEPENDS ifex-tests-integration
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Custom target to run all tests
add_custom_target(run-all-tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS ifex-unit-tests ifex-tests-integration
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)