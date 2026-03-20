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
        ifex-parser
        ifex-common
        ifex-discovery-client
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

# Integration tests use ports 50094-50099 and optionally MQTT on 11883
# Serialize with other tests using same resources
set_tests_properties(integration_tests PROPERTIES
    LABELS "integration"
    RESOURCE_LOCK "mqtt_broker;ifex_test_services"
    TIMEOUT 180
)

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

# =============================================================================
# Scheduler Sync Bridge E2E Tests (vehicle + cloud)
# Requires: Docker for MQTT broker
# =============================================================================
if(NOT CMAKE_CROSSCOMPILING)
    add_executable(ifex-scheduler-sync-bridge-e2e-test
        integration/test_scheduler_sync_bridge_e2e.cpp
        integration/test_fixture.cpp
    )

    target_link_libraries(ifex-scheduler-sync-bridge-e2e-test
        PRIVATE
            # Vehicle side
            ifex-scheduler-service-lib
            ifex-backend-transport-server
            ifex-scheduler-sync-bridge-lib
            # Cloud side
            ifex-cloud-scheduler-service
            ifex-cloud-backend-transport-server
            ifex-cloud-scheduler-sync-bridge-lib
            # Common
            ifex-parser
        ifex-common
        ifex-discovery-client
            ifex-scheduler  # For job.hpp, job_hash.hpp (state checksum comparison)
            ifex-proto-generated
            GTest::gtest
            glog::glog
            gRPC::grpc++
            mosquitto
    )

    target_include_directories(ifex-scheduler-sync-bridge-e2e-test
        PRIVATE
            ${CMAKE_BINARY_DIR}/proto
            ${CMAKE_CURRENT_SOURCE_DIR}/integration
            ${CMAKE_SOURCE_DIR}/libs/scheduler/include  # For job.hpp, job_hash.hpp
            ${CMAKE_SOURCE_DIR}/reference-services/scheduler/include
            ${CMAKE_SOURCE_DIR}/reference-services/backend-transport/include
            ${CMAKE_SOURCE_DIR}/reference-services/scheduler-sync-bridge/include
            ${CMAKE_SOURCE_DIR}/cloud/cloud-scheduler-service/include
            ${CMAKE_SOURCE_DIR}/cloud/cloud-backend-transport/include
            ${CMAKE_SOURCE_DIR}/cloud/cloud-scheduler-sync-bridge/include
    )

    add_test(NAME scheduler_sync_bridge_e2e_test
             COMMAND ifex-scheduler-sync-bridge-e2e-test)

    set_tests_properties(scheduler_sync_bridge_e2e_test PROPERTIES
        LABELS "integration;mqtt;e2e"
        RESOURCE_LOCK "mqtt_broker;ifex_test_services"
        TIMEOUT 300
    )

    add_executable(ifex-cloud-vehicle-sync-bridge-integration-test
        integration/test_cloud_vehicle_sync_bridge_integration.cpp
        integration/test_fixture.cpp
    )

    target_link_libraries(ifex-cloud-vehicle-sync-bridge-integration-test
        PRIVATE
            ifex-sync-core
            ifex-sync-adapters-database
            ifex-sync-backend-transport-bridge
            ifex-backend-transport-server
            ifex-cloud-backend-transport-server
            ifex-cloud-backend-transport-client
            ifex-proto-generated
            GTest::gtest
            glog::glog
            gRPC::grpc++
            mosquitto
    )

    target_include_directories(ifex-cloud-vehicle-sync-bridge-integration-test
        PRIVATE
            ${CMAKE_BINARY_DIR}/proto
            ${CMAKE_CURRENT_SOURCE_DIR}/integration
            ${CMAKE_SOURCE_DIR}/reference-services/backend-transport/vehicle/service/include
            ${CMAKE_SOURCE_DIR}/reference-services/backend-transport/cloud/service/include
            ${CMAKE_SOURCE_DIR}/reference-services/sync/backend-transport/include
            ${CMAKE_SOURCE_DIR}/reference-services/sync/adapters/database/include
            ${CMAKE_SOURCE_DIR}/reference-services/sync/common/include
    )

    add_test(NAME sync_bridge_reconnect_offline
             COMMAND ifex-cloud-vehicle-sync-bridge-integration-test
             --gtest_filter=CloudTruckSyncBridgeIntegrationTest.BridgesStartIndependentlyAndRecoverAfterDisconnect:CloudTruckSyncBridgeIntegrationTest.ReconnectOfflineChangesConvergeBackToQuiescence)

    add_test(NAME sync_bridge_duplicate_delivery_replay_integration
             COMMAND ifex-cloud-vehicle-sync-bridge-integration-test
             --gtest_filter=CloudTruckSyncBridgeIntegrationTest.DuplicateDeliveryIsIdempotent:CloudTruckSyncBridgeIntegrationTest.GapRecoveryRequestsAndFetchesMissingRecords)

    set_tests_properties(sync_bridge_reconnect_offline PROPERTIES
        LABELS "integration;mqtt;sync"
        RESOURCE_LOCK "mqtt_broker;ifex_test_services"
        TIMEOUT 300
    )

    set_tests_properties(sync_bridge_duplicate_delivery_replay_integration PROPERTIES
        LABELS "integration;mqtt;sync"
        RESOURCE_LOCK "mqtt_broker;ifex_test_services"
        TIMEOUT 300
    )
endif()
