cmake_minimum_required(VERSION 3.16)
project(ifex-cloud-backend-transport VERSION 1.0.0 LANGUAGES CXX)

# Find mosquitto library
find_library(MOSQUITTO_LIB mosquitto REQUIRED)
find_path(MOSQUITTO_INCLUDE mosquitto.h PATH_SUFFIXES include)

# =============================================================================
# Server Library
# =============================================================================

add_library(ifex-cloud-backend-transport-server STATIC
    src/cloud_backend_transport_server.cpp
)

target_include_directories(ifex-cloud-backend-transport-server
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${MOSQUITTO_INCLUDE}
)

target_link_libraries(ifex-cloud-backend-transport-server
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

add_library(ifex-cloud-backend-transport-client STATIC
    src/cloud_backend_transport_client.cpp
)

target_include_directories(ifex-cloud-backend-transport-client
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ifex-cloud-backend-transport-client
    PUBLIC
        ifex-proto-generated
        gRPC::grpc++
        glog::glog
    PRIVATE
        Threads::Threads
)

# =============================================================================
# Service Executable
# =============================================================================

add_executable(ifex-cloud-backend-transport-service
    src/main.cpp
)

target_include_directories(ifex-cloud-backend-transport-service
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ifex-cloud-backend-transport-service
    PRIVATE
        ifex-cloud-backend-transport-server
        gRPC::grpc++_reflection
        gflags
        Threads::Threads
)

# Install
install(TARGETS ifex-cloud-backend-transport-service
    RUNTIME DESTINATION bin
)

install(TARGETS ifex-cloud-backend-transport-client ifex-cloud-backend-transport-server
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/
    DESTINATION include
)

# =============================================================================
# Integration Tests (require Docker for MQTT broker)
# =============================================================================

if(BUILD_INTEGRATION_TESTS)
    find_package(GTest REQUIRED)

    add_executable(ifex-cloud-backend-transport-integration-test
        tests/cloud_backend_transport_integration_test.cpp
    )

    target_include_directories(ifex-cloud-backend-transport-integration-test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/reference-services/backend-transport/include
            ${CMAKE_SOURCE_DIR}/reference-services/backend-transport/client/include
            ${MOSQUITTO_INCLUDE}
    )

    target_link_libraries(ifex-cloud-backend-transport-integration-test
        PRIVATE
            ifex-cloud-backend-transport-server
            ifex-cloud-backend-transport-client
            ifex-backend-transport-server
            ifex-backend-transport-client
            GTest::gtest
            glog::glog
            gRPC::grpc++
            ${MOSQUITTO_LIB}
            Threads::Threads
    )

    add_test(NAME cloud_backend_transport_integration_test
        COMMAND ifex-cloud-backend-transport-integration-test
    )

    set_tests_properties(cloud_backend_transport_integration_test PROPERTIES
        TIMEOUT 120
        LABELS "integration;docker;mqtt;cloud"
        RESOURCE_LOCK mqtt_broker
    )
endif()
