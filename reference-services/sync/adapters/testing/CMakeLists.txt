cmake_minimum_required(VERSION 3.16)

if(NOT TARGET ifex-sync-adapters-testing)

    add_library(ifex-sync-adapters-testing INTERFACE)

endif()

if(BUILD_TESTS OR BUILD_INTEGRATION_TESTS OR VEP_BUILD_TESTS)
    enable_testing()
    find_package(GTest QUIET)

    add_library(ifex-sync-adapters-testing-fake STATIC
        src/fake_cloud_vehicle_db_adapter.cpp
    )

    target_include_directories(ifex-sync-adapters-testing-fake PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_features(ifex-sync-adapters-testing-fake PUBLIC cxx_std_17)

    add_executable(fake-adapter-contract-test
        tests/fake_adapter_contract_test.cpp
    )

    if(NOT GTest_FOUND)
        target_sources(fake-adapter-contract-test PRIVATE
            tests/gtest_fallback_main.cpp
        )
    endif()

    target_link_libraries(fake-adapter-contract-test
        PRIVATE
            ifex-sync-core
            ifex-sync-adapters-testing-fake
    )

    if(GTest_FOUND)
        target_link_libraries(fake-adapter-contract-test
            PRIVATE
            GTest::gtest
            GTest::gtest_main
        )
    endif()

    add_test(NAME fake_adapter_contract_all COMMAND fake-adapter-contract-test --gtest_filter=FakeAdapterContract.*)
    add_test(NAME adapter_contract_positive COMMAND fake-adapter-contract-test --gtest_filter=FakeAdapterContract.*)
    add_test(NAME adapter_contract_negative COMMAND fake-adapter-contract-test --gtest_filter=AdapterContractNegative.*)
endif()
