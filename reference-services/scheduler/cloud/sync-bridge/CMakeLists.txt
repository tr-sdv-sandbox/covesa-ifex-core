# Cloud Scheduler Sync Bridge
# Handles bidirectional sync between cloud scheduler and vehicles

# Library
add_library(ifex-cloud-scheduler-sync-bridge-lib STATIC
    src/cloud_scheduler_sync_bridge.cpp
)

target_include_directories(ifex-cloud-scheduler-sync-bridge-lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ifex-cloud-scheduler-sync-bridge-lib
    PUBLIC
        ifex-proto-generated
        ifex-scheduler
        gRPC::grpc++
        glog::glog
)

# Executable
add_executable(ifex-cloud-scheduler-sync-bridge
    src/main.cpp
)

target_link_libraries(ifex-cloud-scheduler-sync-bridge
    PRIVATE
        ifex-cloud-scheduler-sync-bridge-lib
        gflags
)

# Unit tests
if(BUILD_TESTING)
    add_executable(ifex-cloud-scheduler-sync-bridge-test
        tests/cloud_scheduler_sync_bridge_test.cpp
    )

    target_link_libraries(ifex-cloud-scheduler-sync-bridge-test
        PRIVATE
            ifex-cloud-scheduler-sync-bridge-lib
            GTest::gtest
            GTest::gtest_main
    )

    add_test(NAME ifex-cloud-scheduler-sync-bridge-test
             COMMAND ifex-cloud-scheduler-sync-bridge-test)

    # Integration tests removed - covered by scheduler_sync_bridge_e2e_test and cloud_scheduler_integration_test
endif()
