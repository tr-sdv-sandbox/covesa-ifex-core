# Scheduler State Library
#
# Provides canonical job structure, hash computation, and storage abstractions
# for the Scheduler Sync Protocol v2.
#
# This library is shared between:
# - Vehicle: scheduler sync bridge
# - Cloud: scheduler mirror, scheduler API

cmake_minimum_required(VERSION 3.16)

# Guard against multiple inclusion (when used as submodule from multiple repos)
if(TARGET ifex-scheduler)
    return()
endif()

# Library sources
set(SCHEDULER_LIB_SOURCES
    src/job.cpp
    src/job_hash.cpp
    src/memory_store.cpp
)

# Library headers (for IDE support)
set(SCHEDULER_LIB_HEADERS
    include/job.hpp
    include/job_hash.hpp
    include/job_store.hpp
    include/memory_store.hpp
    include/version_vector.hpp
    include/sync_engine.hpp
    adapters/proto_adapter.hpp
)

# Create the library
add_library(ifex-scheduler STATIC ${SCHEDULER_LIB_SOURCES} ${SCHEDULER_LIB_HEADERS})

target_include_directories(ifex-scheduler
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
)

# C++17 required
target_compile_features(ifex-scheduler PUBLIC cxx_std_17)

# JSON normalization for consistent hashing across platforms
find_package(nlohmann_json REQUIRED)
target_link_libraries(ifex-scheduler PUBLIC nlohmann_json::nlohmann_json)

# Alias for consistent naming
add_library(ifex::scheduler ALIAS ifex-scheduler)

# Install targets
install(TARGETS ifex-scheduler
    EXPORT ifex-scheduler-targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/
    DESTINATION include/ifex/scheduler
    FILES_MATCHING PATTERN "*.hpp"
)

install(DIRECTORY adapters/
    DESTINATION include/ifex/scheduler/adapters
    FILES_MATCHING PATTERN "*.hpp"
)

# Tests (if enabled)
if(BUILD_TESTS OR BUILD_INTEGRATION_TESTS OR VEP_BUILD_TESTS)
    add_subdirectory(tests)
endif()
