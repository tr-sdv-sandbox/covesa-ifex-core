# IFEX Common Library
#
# Provides shared type definitions and utility functions.
# This is a header-mostly library with minimal dependencies.

cmake_minimum_required(VERSION 3.16)

# Guard against multiple inclusion
if(TARGET ifex-common)
    return()
endif()

# Library sources (network_utils is the only cpp file)
add_library(ifex-common STATIC
    src/network_utils.cpp
)

target_include_directories(ifex-common
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# C++17 required
target_compile_features(ifex-common PUBLIC cxx_std_17)

# Dependencies
find_package(glog REQUIRED)

target_link_libraries(ifex-common
    PRIVATE
        glog::glog
)

# Alias for consistent naming
add_library(ifex::common ALIAS ifex-common)

# Install targets (use ifex-targets export set for compatibility)
install(TARGETS ifex-common
    EXPORT ifex-targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/
    DESTINATION include
    FILES_MATCHING PATTERN "*.hpp"
)
