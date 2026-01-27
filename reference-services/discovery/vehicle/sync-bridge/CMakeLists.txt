# =============================================================================
# Library (used by service and tests)
# =============================================================================

add_library(ifex-discovery-sync-bridge-lib STATIC
    src/discovery_sync_bridge.cpp
)

target_include_directories(ifex-discovery-sync-bridge-lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/reference-services/common/include
)

target_link_libraries(ifex-discovery-sync-bridge-lib
    PUBLIC
        ifex-proto-generated
        ifex-backend-transport-client
        gRPC::grpc++
        glog::glog
    PRIVATE
        Threads::Threads
)

# =============================================================================
# Service Executable
# =============================================================================

add_executable(ifex-discovery-sync-bridge
    src/main.cpp
)

target_include_directories(ifex-discovery-sync-bridge
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/reference-services/common/include
)

target_link_libraries(ifex-discovery-sync-bridge
    PRIVATE
        ifex-discovery-sync-bridge-lib
        ifex-parser
        ifex-common
        ifex-discovery-client
        gflags
        Threads::Threads
)

# Install
install(TARGETS ifex-discovery-sync-bridge
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# =============================================================================
# Tests
# =============================================================================

if(BUILD_INTEGRATION_TESTS)
    find_package(GTest REQUIRED)

    # Find mosquitto for integration tests
    find_library(MOSQUITTO_LIB mosquitto REQUIRED)
    find_path(MOSQUITTO_INCLUDE mosquitto.h PATH_SUFFIXES include)

    # =========================================================================
    # Unit Tests (no Docker required)
    # =========================================================================

    add_executable(ifex-discovery-sync-bridge-test
        tests/discovery_sync_bridge_test.cpp
    )

    target_include_directories(ifex-discovery-sync-bridge-test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/reference-services/common/include
    )

    target_link_libraries(ifex-discovery-sync-bridge-test
        PRIVATE
            ifex-discovery-sync-bridge-lib
            ifex-parser
        ifex-common
        ifex-discovery-client
            ifex-backend-transport-server
            GTest::gtest
            Threads::Threads
    )

    add_test(NAME discovery_sync_bridge_test
        COMMAND ifex-discovery-sync-bridge-test
    )

    set_tests_properties(discovery_sync_bridge_test PROPERTIES
        TIMEOUT 60
        LABELS "unit"
    )

    # =========================================================================
    # Integration Tests (require Docker for MQTT broker)
    # =========================================================================

    add_executable(ifex-discovery-sync-bridge-integration-test
        tests/discovery_sync_bridge_integration_test.cpp
    )

    target_include_directories(ifex-discovery-sync-bridge-integration-test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/reference-services/common/include
            ${CMAKE_SOURCE_DIR}/reference-services/backend-transport/include
            ${CMAKE_SOURCE_DIR}/reference-services/backend-transport/client/include
            ${CMAKE_SOURCE_DIR}/reference-services/backend-transport/tests
            ${MOSQUITTO_INCLUDE}
    )

    target_link_libraries(ifex-discovery-sync-bridge-integration-test
        PRIVATE
            ifex-discovery-sync-bridge-lib
            ifex-parser
        ifex-common
        ifex-discovery-client
            ifex-backend-transport-server
            ifex-backend-transport-client
            GTest::gtest
            glog::glog
            gRPC::grpc++
            ${MOSQUITTO_LIB}
            Threads::Threads
    )

    add_test(NAME discovery_sync_bridge_integration_test
        COMMAND ifex-discovery-sync-bridge-integration-test
    )

    set_tests_properties(discovery_sync_bridge_integration_test PROPERTIES
        TIMEOUT 180
        LABELS "integration;docker;mqtt"
        RESOURCE_LOCK mqtt_broker
    )
endif()
