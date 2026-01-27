# Scheduler library tests

find_package(GTest REQUIRED)

# Job hash tests
add_executable(scheduler-job-hash-test
    job_hash_test.cpp
)

target_link_libraries(scheduler-job-hash-test
    PRIVATE
        ifex-scheduler
        GTest::gtest
        GTest::gtest_main
)

add_test(NAME scheduler_job_hash_test COMMAND scheduler-job-hash-test)
set_tests_properties(scheduler_job_hash_test PROPERTIES LABELS "unit")

# Memory store tests
add_executable(scheduler-memory-store-test
    memory_store_test.cpp
)

target_link_libraries(scheduler-memory-store-test
    PRIVATE
        ifex-scheduler
        GTest::gtest
        GTest::gtest_main
)

add_test(NAME scheduler_memory_store_test COMMAND scheduler-memory-store-test)
set_tests_properties(scheduler_memory_store_test PROPERTIES LABELS "unit")

# Version vector tests
add_executable(scheduler-version-vector-test
    version_vector_test.cpp
)

target_link_libraries(scheduler-version-vector-test
    PRIVATE
        ifex-scheduler
        GTest::gtest
        GTest::gtest_main
)

add_test(NAME scheduler_version_vector_test COMMAND scheduler-version-vector-test)
set_tests_properties(scheduler_version_vector_test PROPERTIES LABELS "unit")
