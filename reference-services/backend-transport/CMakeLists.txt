cmake_minimum_required(VERSION 3.16)
project(ifex-backend-transport VERSION 1.0.0 LANGUAGES CXX)

# Find mosquitto library
find_library(MOSQUITTO_LIB mosquitto REQUIRED)
find_path(MOSQUITTO_INCLUDE mosquitto.h PATH_SUFFIXES include)

# =============================================================================
# Server Library (used by service and tests)
# =============================================================================

add_library(ifex-backend-transport-server STATIC
    src/backend_transport_server.cpp
    src/mqtt_client.cpp
    src/message_queue.cpp
)

target_include_directories(ifex-backend-transport-server
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${MOSQUITTO_INCLUDE}
)

target_link_libraries(ifex-backend-transport-server
    PUBLIC
        ifex-proto-generated
        gRPC::grpc++
        glog::glog
    PRIVATE
        ${MOSQUITTO_LIB}
        Threads::Threads
)

# =============================================================================
# Client Library
# =============================================================================

add_library(ifex-backend-transport-client STATIC
    client/src/backend_transport_client.cpp
)

target_include_directories(ifex-backend-transport-client
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/client/include
)

target_link_libraries(ifex-backend-transport-client
    PUBLIC
        ifex-proto-generated
        gRPC::grpc++
        glog::glog
)

# =============================================================================
# Service Executable
# =============================================================================

add_executable(ifex-backend-transport-service
    src/main.cpp
)

target_include_directories(ifex-backend-transport-service
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ifex-backend-transport-service
    PRIVATE
        ifex-backend-transport-server
        ifex-core
        gRPC::grpc++_reflection
        ${PROTOBUF_LIBRARIES}
        yaml-cpp
        nlohmann_json::nlohmann_json
        Threads::Threads
)

# Install
install(TARGETS ifex-backend-transport-service
    RUNTIME DESTINATION bin
)

install(TARGETS ifex-backend-transport-client ifex-backend-transport-server
    ARCHIVE DESTINATION lib
)

install(DIRECTORY client/include/
    DESTINATION include
)

# =============================================================================
# Integration Tests (require Docker for MQTT broker)
# =============================================================================

if(BUILD_INTEGRATION_TESTS)
    find_package(GTest REQUIRED)

    add_executable(ifex-backend-transport-integration-test
        tests/backend_transport_integration_test.cpp
    )

    target_include_directories(ifex-backend-transport-integration-test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/client/include
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )

    target_link_libraries(ifex-backend-transport-integration-test
        PRIVATE
            ifex-backend-transport-server
            ifex-backend-transport-client
            GTest::gtest
            GTest::gtest_main
            glog::glog
            gRPC::grpc++
            Threads::Threads
    )

    # Register test with Docker label
    add_test(NAME backend_transport_integration_test
        COMMAND ifex-backend-transport-integration-test
    )

    set_tests_properties(backend_transport_integration_test PROPERTIES
        TIMEOUT 120
        LABELS "integration;docker"
    )

    # Resilience tests (broker up/down, persistence)
    add_executable(ifex-backend-transport-resilience-test
        tests/backend_transport_resilience_test.cpp
    )

    target_include_directories(ifex-backend-transport-resilience-test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/client/include
            ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )

    target_link_libraries(ifex-backend-transport-resilience-test
        PRIVATE
            ifex-backend-transport-server
            ifex-backend-transport-client
            GTest::gtest
            GTest::gtest_main
            glog::glog
            gRPC::grpc++
            Threads::Threads
    )

    add_test(NAME backend_transport_resilience_test
        COMMAND ifex-backend-transport-resilience-test
    )

    set_tests_properties(backend_transport_resilience_test PROPERTIES
        TIMEOUT 300
        LABELS "integration;docker;resilience"
    )
endif()
