cmake_minimum_required(VERSION 3.16)

if(TARGET ifex-sync-adapters-database)
    return()
endif()

find_package(SQLite3 REQUIRED)

if(TARGET SQLite3::SQLite3)
    set(IFEX_SYNC_SQLITE_TARGET SQLite3::SQLite3)
else()
    set(IFEX_SYNC_SQLITE_TARGET SQLite::SQLite3)
endif()

add_library(ifex-sync-adapters-database STATIC
    src/sqlite_cloud_vehicle_db_adapter.cpp
)

target_include_directories(ifex-sync-adapters-database
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ifex-sync-adapters-database
    PUBLIC
        ifex-sync-core
        ${IFEX_SYNC_SQLITE_TARGET}
)

target_compile_features(ifex-sync-adapters-database PUBLIC cxx_std_17)

add_library(ifex::sync-adapters-database ALIAS ifex-sync-adapters-database)

if(BUILD_TESTS OR BUILD_INTEGRATION_TESTS OR VEP_BUILD_TESTS)
    enable_testing()
    find_package(GTest QUIET)

    add_executable(database-adapter-test
        tests/sqlite_cloud_vehicle_db_adapter_test.cpp
    )

    if(NOT GTest_FOUND)
        target_sources(database-adapter-test PRIVATE
            ../testing/tests/gtest_fallback_main.cpp
        )
    endif()

    target_link_libraries(database-adapter-test
        PRIVATE
            ifex-sync-adapters-database
            ifex-sync-core
    )

    if(GTest_FOUND)
        target_link_libraries(database-adapter-test
            PRIVATE
            GTest::gtest
            GTest::gtest_main
        )
    endif()

    add_test(NAME database_adapter_contract COMMAND database-adapter-test --gtest_filter=DatabaseAdapterContract.*)
    add_test(NAME database_adapter_conflict COMMAND database-adapter-test --gtest_filter=DatabaseAdapterConflict.*)
endif()
