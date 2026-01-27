# =============================================================================
# Library (used by service and tests)
# =============================================================================

add_library(ifex-scheduler-sync-bridge-lib STATIC
    src/scheduler_sync_bridge.cpp
)

target_include_directories(ifex-scheduler-sync-bridge-lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../common/include
        ${CMAKE_SOURCE_DIR}/reference-services/common/include
)

target_link_libraries(ifex-scheduler-sync-bridge-lib
    PUBLIC
        ifex-proto-generated
        ifex-backend-transport-client
        ifex-scheduler
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
        ${CMAKE_CURRENT_SOURCE_DIR}/../../common/include
)

target_link_libraries(ifex-scheduler-sync-bridge
    PRIVATE
        ifex-scheduler-sync-bridge-lib
        ifex-parser
        ifex-common
        ifex-discovery-client
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
            ${CMAKE_CURRENT_SOURCE_DIR}/../../common/include
    )

    target_link_libraries(ifex-scheduler-sync-bridge-test
        PRIVATE
            ifex-scheduler-sync-bridge-lib
            ifex-parser
        ifex-common
        ifex-discovery-client
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

    # Integration tests removed - covered by scheduler_sync_bridge_e2e_test
endif()
