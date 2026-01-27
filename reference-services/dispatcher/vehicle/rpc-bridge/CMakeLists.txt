# =============================================================================
# Library (used by service and tests)
# =============================================================================

add_library(ifex-dispatcher-bridge-lib STATIC
    src/dispatcher_bridge.cpp
)

target_include_directories(ifex-dispatcher-bridge-lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/reference-services/common/include
)

target_link_libraries(ifex-dispatcher-bridge-lib
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

add_executable(ifex-dispatcher-bridge
    src/main.cpp
)

target_include_directories(ifex-dispatcher-bridge
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/reference-services/common/include
)

target_link_libraries(ifex-dispatcher-bridge
    PRIVATE
        ifex-dispatcher-bridge-lib
        ifex-parser
        ifex-common
        ifex-discovery-client
        gflags
        Threads::Threads
)

# Install
install(TARGETS ifex-dispatcher-bridge
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

    add_executable(ifex-dispatcher-bridge-test
        tests/dispatcher_bridge_test.cpp
    )

    target_include_directories(ifex-dispatcher-bridge-test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/reference-services/common/include
    )

    target_link_libraries(ifex-dispatcher-bridge-test
        PRIVATE
            ifex-dispatcher-bridge-lib
            ifex-parser
        ifex-common
        ifex-discovery-client
            ifex-backend-transport-server
            GTest::gtest
            Threads::Threads
    )

    add_test(NAME dispatcher_bridge_test
        COMMAND ifex-dispatcher-bridge-test
    )

    set_tests_properties(dispatcher_bridge_test PROPERTIES
        TIMEOUT 60
        LABELS "unit"
    )

    # =========================================================================
    # Integration Tests (require Docker for MQTT broker)
    # =========================================================================

    add_executable(ifex-dispatcher-bridge-integration-test
        tests/dispatcher_bridge_integration_test.cpp
    )

    target_include_directories(ifex-dispatcher-bridge-integration-test
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_SOURCE_DIR}/reference-services/common/include
            ${CMAKE_SOURCE_DIR}/reference-services/backend-transport/include
            ${CMAKE_SOURCE_DIR}/reference-services/backend-transport/client/include
            ${CMAKE_SOURCE_DIR}/reference-services/backend-transport/tests
            ${MOSQUITTO_INCLUDE}
    )

    target_link_libraries(ifex-dispatcher-bridge-integration-test
        PRIVATE
            ifex-dispatcher-bridge-lib
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

    add_test(NAME dispatcher_bridge_integration_test
        COMMAND ifex-dispatcher-bridge-integration-test
    )

    set_tests_properties(dispatcher_bridge_integration_test PROPERTIES
        TIMEOUT 180
        LABELS "integration;docker;mqtt"
        RESOURCE_LOCK mqtt_broker
    )
endif()
