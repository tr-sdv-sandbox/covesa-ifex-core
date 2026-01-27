# Backend Transport Server
#
# Provides MQTT bridge for vehicle-to-cloud communication.
# See README.md for full API documentation.

cmake_minimum_required(VERSION 3.16)
project(ifex-backend-transport-service VERSION 1.0.0 LANGUAGES CXX)

# Find mosquitto library
find_library(MOSQUITTO_LIB mosquitto REQUIRED)
find_path(MOSQUITTO_INCLUDE mosquitto.h PATH_SUFFIXES include)

# =============================================================================
# Server Library
# =============================================================================

add_library(ifex-backend-transport-server STATIC
    src/backend_transport_server.cpp
    src/mqtt_client.cpp
    src/message_queue.cpp
)

target_include_directories(ifex-backend-transport-server
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${MOSQUITTO_INCLUDE}
        ${CMAKE_BINARY_DIR}/proto
)

target_compile_features(ifex-backend-transport-server PUBLIC cxx_std_17)

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
# Service Executable
# =============================================================================

add_executable(ifex-backend-transport-service
    src/main.cpp
)

target_include_directories(ifex-backend-transport-service
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${PROTO_IFEX_VEHICLE_DIR}
)

target_link_libraries(ifex-backend-transport-service
    PRIVATE
        ifex-backend-transport-server
        ifex-parser
        ifex-common
        ifex-discovery-client
        gflags
        gRPC::grpc++_reflection
        Threads::Threads
)

# Install
install(TARGETS ifex-backend-transport-service ifex-backend-transport-server
    RUNTIME DESTINATION bin
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/
    DESTINATION include
)

# =============================================================================
# Tests
# =============================================================================

if(BUILD_INTEGRATION_TESTS)
    find_package(GTest REQUIRED)

    # =========================================================================
    # Client Tests (no Docker required)
    # =========================================================================

    add_executable(ifex-backend-transport-client-test
        tests/backend_transport_client_test.cpp
    )

    target_include_directories(ifex-backend-transport-client-test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${IFEX_BACKEND_TRANSPORT_CLIENT_INCLUDE}
    )

    target_link_libraries(ifex-backend-transport-client-test
        PRIVATE
            ifex-backend-transport-server
            ifex-backend-transport-client
            GTest::gtest
            Threads::Threads
    )

    add_test(NAME backend_transport_client_test
        COMMAND ifex-backend-transport-client-test
    )

    set_tests_properties(backend_transport_client_test PROPERTIES
        TIMEOUT 30
        LABELS "client"
    )

    # =========================================================================
    # Conformance Tests
    # =========================================================================

    add_executable(ifex-backend-transport-conformance-test
        tests/backend_transport_conformance_test.cpp
    )

    target_include_directories(ifex-backend-transport-conformance-test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${IFEX_BACKEND_TRANSPORT_CLIENT_INCLUDE}
    )

    target_link_libraries(ifex-backend-transport-conformance-test
        PRIVATE
            ifex-backend-transport-server
            ifex-backend-transport-client
            GTest::gtest
            glog::glog
            gRPC::grpc++
            Threads::Threads
    )

    add_test(NAME backend_transport_conformance_test
        COMMAND ifex-backend-transport-conformance-test
    )

    set_tests_properties(backend_transport_conformance_test PROPERTIES
        TIMEOUT 60
        LABELS "conformance"
    )

    # =========================================================================
    # Integration Tests (require Docker for MQTT broker)
    # =========================================================================

    add_executable(ifex-backend-transport-integration-test
        tests/backend_transport_integration_test.cpp
    )

    target_include_directories(ifex-backend-transport-integration-test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${IFEX_BACKEND_TRANSPORT_CLIENT_INCLUDE}
            ${MOSQUITTO_INCLUDE}
    )

    target_link_libraries(ifex-backend-transport-integration-test
        PRIVATE
            ifex-backend-transport-server
            ifex-backend-transport-client
            GTest::gtest
            glog::glog
            gRPC::grpc++
            ${MOSQUITTO_LIB}
            Threads::Threads
    )

    add_test(NAME backend_transport_integration_test
        COMMAND ifex-backend-transport-integration-test
    )

    set_tests_properties(backend_transport_integration_test PROPERTIES
        TIMEOUT 180
        LABELS "integration;docker;mqtt"
        RESOURCE_LOCK mqtt_broker
    )

    # =========================================================================
    # Resilience Tests
    # =========================================================================

    add_executable(ifex-backend-transport-resilience-test
        tests/backend_transport_resilience_test.cpp
    )

    target_include_directories(ifex-backend-transport-resilience-test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${IFEX_BACKEND_TRANSPORT_CLIENT_INCLUDE}
            ${MOSQUITTO_INCLUDE}
    )

    target_link_libraries(ifex-backend-transport-resilience-test
        PRIVATE
            ifex-backend-transport-server
            ifex-backend-transport-client
            GTest::gtest
            glog::glog
            gRPC::grpc++
            ${MOSQUITTO_LIB}
            Threads::Threads
    )

    add_test(NAME backend_transport_resilience_test
        COMMAND ifex-backend-transport-resilience-test
    )

    set_tests_properties(backend_transport_resilience_test PROPERTIES
        TIMEOUT 180
        LABELS "resilience;docker;mqtt"
        RESOURCE_LOCK mqtt_broker
    )
endif()
