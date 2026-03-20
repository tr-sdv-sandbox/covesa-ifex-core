cmake_minimum_required(VERSION 3.16)

if(BUILD_TESTS OR BUILD_INTEGRATION_TESTS OR VEP_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(common)
add_subdirectory(adapters/database)
add_subdirectory(adapters/testing)
add_subdirectory(backend-transport)
