cmake_minimum_required(VERSION 3.16)
project(ifex-cloud-scheduler-service VERSION 1.0.0 LANGUAGES CXX)

# =============================================================================
# Service Library
# =============================================================================

add_library(ifex-cloud-scheduler-service STATIC
    src/cloud_scheduler_service.cpp
)

target_include_directories(ifex-cloud-scheduler-service
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ifex-cloud-scheduler-service
    PUBLIC
        ifex-proto-generated
        ifex-cloud-backend-transport-client
        ifex-scheduler
        gRPC::grpc++
        glog::glog
    PRIVATE
        Threads::Threads
)

# =============================================================================
# Service Executable
# =============================================================================

add_executable(ifex-cloud-scheduler-service-bin
    src/main.cpp
)

set_target_properties(ifex-cloud-scheduler-service-bin PROPERTIES
    OUTPUT_NAME ifex-cloud-scheduler-service
)

target_include_directories(ifex-cloud-scheduler-service-bin
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ifex-cloud-scheduler-service-bin
    PRIVATE
        ifex-cloud-scheduler-service
        gflags
        Threads::Threads
)

# Install
install(TARGETS ifex-cloud-scheduler-service ifex-cloud-scheduler-service-bin
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/
    DESTINATION include
)

# =============================================================================
# Integration Tests
# =============================================================================

if(BUILD_INTEGRATION_TESTS)
    find_package(GTest REQUIRED)
    find_library(MOSQUITTO_LIB mosquitto REQUIRED)
    find_path(MOSQUITTO_INCLUDE mosquitto.h PATH_SUFFIXES include)

    add_executable(ifex-cloud-scheduler-integration-test
        tests/cloud_scheduler_integration_test.cpp
    )

    target_include_directories(ifex-cloud-scheduler-integration-test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/reference-services/backend-transport/include
            ${CMAKE_SOURCE_DIR}/reference-services/backend-transport/client/include
            ${CMAKE_SOURCE_DIR}/reference-services/scheduler-sync-bridge/include
            ${CMAKE_SOURCE_DIR}/cloud/cloud-backend-transport/include
            ${MOSQUITTO_INCLUDE}
    )

    target_link_libraries(ifex-cloud-scheduler-integration-test
        PRIVATE
            ifex-cloud-scheduler-service
            ifex-cloud-backend-transport-server
            ifex-cloud-backend-transport-client
            ifex-backend-transport-server
            ifex-backend-transport-client
            ifex-scheduler-sync-bridge-lib
            GTest::gtest
            glog::glog
            gRPC::grpc++
            ${MOSQUITTO_LIB}
            Threads::Threads
    )

    add_test(NAME cloud_scheduler_integration_test
        COMMAND ifex-cloud-scheduler-integration-test
    )

    set_tests_properties(cloud_scheduler_integration_test PROPERTIES
        TIMEOUT 180
        LABELS "integration;docker;mqtt;cloud;scheduler"
        RESOURCE_LOCK mqtt_broker
    )
endif()
