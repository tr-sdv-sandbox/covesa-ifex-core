# IFEX Parser Library
#
# Provides IFEX YAML parsing functionality.
# This is a generic utility library with no proto dependencies.

cmake_minimum_required(VERSION 3.16)

# Guard against multiple inclusion
if(TARGET ifex-parser)
    return()
endif()

# Library sources
add_library(ifex-parser STATIC
    src/parser_impl.cpp
)

target_include_directories(ifex-parser
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/libs/common/include>
        $<INSTALL_INTERFACE:include>
)

# C++17 required
target_compile_features(ifex-parser PUBLIC cxx_std_17)

# Dependencies
find_package(yaml-cpp REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(glog REQUIRED)

target_link_libraries(ifex-parser
    PUBLIC
        ifex-common
    PRIVATE
        yaml-cpp
        nlohmann_json::nlohmann_json
        glog::glog
)

# Alias for consistent naming
add_library(ifex::parser ALIAS ifex-parser)

# Install targets (use ifex-targets export set for compatibility)
install(TARGETS ifex-parser
    EXPORT ifex-targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/
    DESTINATION include
    FILES_MATCHING PATTERN "*.hpp"
)
