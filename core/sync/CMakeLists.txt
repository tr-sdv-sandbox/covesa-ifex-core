# Sync library for Scheduler Sync Protocol v2
#
# Header-only library providing version vector operations and sync engine
# for bidirectional job synchronization between cloud and vehicle.

# Header-only library (no source files needed)
add_library(ifex-sync INTERFACE)

target_include_directories(ifex-sync
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include/ifex/sync>
)

# Install headers
install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ifex/sync
    FILES_MATCHING PATTERN "*.hpp"
)

# Unit tests (GTest found at top level when BUILD_TESTS or BUILD_INTEGRATION_TESTS)
if(BUILD_TESTS OR BUILD_INTEGRATION_TESTS)
    find_package(GTest QUIET)
    if(GTest_FOUND)
        add_executable(ifex-sync-tests
            tests/version_vector_test.cpp
        )

        target_link_libraries(ifex-sync-tests
            PRIVATE
                ifex-sync
                GTest::gtest
                GTest::gtest_main
        )

        add_test(
            NAME ifex-sync-tests
            COMMAND ifex-sync-tests
        )

        # Set test labels
        set_tests_properties(ifex-sync-tests
            PROPERTIES LABELS "unit"
        )
    endif()
endif()
