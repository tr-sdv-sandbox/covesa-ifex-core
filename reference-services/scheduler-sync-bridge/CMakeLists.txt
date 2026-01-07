# =============================================================================
# Library (used by service and tests)
# =============================================================================

add_library(ifex-scheduler-sync-bridge-lib STATIC
    src/scheduler_sync_bridge.cpp
)

target_include_directories(ifex-scheduler-sync-bridge-lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../common/include
)

target_link_libraries(ifex-scheduler-sync-bridge-lib
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

add_executable(ifex-scheduler-sync-bridge
    src/main.cpp
)

target_include_directories(ifex-scheduler-sync-bridge
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../common/include
)

target_link_libraries(ifex-scheduler-sync-bridge
    PRIVATE
        ifex-scheduler-sync-bridge-lib
        ifex-core
        gflags
        Threads::Threads
)

# Install
install(TARGETS ifex-scheduler-sync-bridge
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

    add_executable(ifex-scheduler-sync-bridge-test
        tests/scheduler_sync_bridge_test.cpp
    )

    target_include_directories(ifex-scheduler-sync-bridge-test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../common/include
    )

    target_link_libraries(ifex-scheduler-sync-bridge-test
        PRIVATE
            ifex-scheduler-sync-bridge-lib
            ifex-core
            ifex-backend-transport-server
            GTest::gtest
            Threads::Threads
    )

    add_test(NAME scheduler_sync_bridge_test
        COMMAND ifex-scheduler-sync-bridge-test
    )

    set_tests_properties(scheduler_sync_bridge_test PROPERTIES
        TIMEOUT 60
        LABELS "unit"
    )

    # =========================================================================
    # Integration Tests (require Docker for MQTT broker)
    # =========================================================================

    add_executable(ifex-scheduler-sync-bridge-integration-test
        tests/scheduler_sync_bridge_integration_test.cpp
    )

    target_include_directories(ifex-scheduler-sync-bridge-integration-test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../common/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../backend-transport/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../backend-transport/client/include
            ${CMAKE_CURRENT_SOURCE_DIR}/../backend-transport/tests
            ${MOSQUITTO_INCLUDE}
    )

    target_link_libraries(ifex-scheduler-sync-bridge-integration-test
        PRIVATE
            ifex-scheduler-sync-bridge-lib
            ifex-core
            ifex-backend-transport-server
            ifex-backend-transport-client
            GTest::gtest
            glog::glog
            gRPC::grpc++
            ${MOSQUITTO_LIB}
            Threads::Threads
    )

    add_test(NAME scheduler_sync_bridge_integration_test
        COMMAND ifex-scheduler-sync-bridge-integration-test
    )

    set_tests_properties(scheduler_sync_bridge_integration_test PROPERTIES
        TIMEOUT 180
        LABELS "integration;docker;mqtt"
        RESOURCE_LOCK mqtt_broker
    )
endif()
