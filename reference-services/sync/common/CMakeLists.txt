cmake_minimum_required(VERSION 3.16)

if(TARGET ifex-sync-core)
    return()
endif()

set(SYNC_CORE_SOURCES
    src/cloud_vehicle_sync_core.cpp
)

set(SYNC_CORE_HEADERS
    include/cloud_vehicle_sync_types.hpp
    include/cloud_vehicle_db_adapter.hpp
    include/cloud_vehicle_sync_core.hpp
)

add_library(ifex-sync-core STATIC ${SYNC_CORE_SOURCES} ${SYNC_CORE_HEADERS})

target_include_directories(ifex-sync-core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(ifex-sync-core PUBLIC cxx_std_17)

add_library(ifex::sync-core ALIAS ifex-sync-core)

if(BUILD_TESTS OR BUILD_INTEGRATION_TESTS OR VEP_BUILD_TESTS)
    add_subdirectory(tests)
endif()
