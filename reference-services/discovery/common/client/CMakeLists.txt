# IFEX Discovery Client Library
#
# Provides gRPC client for Discovery service.
# Used by all services that need to register with or query the discovery service.

cmake_minimum_required(VERSION 3.16)

# Guard against multiple inclusion
if(TARGET ifex-discovery-client)
    return()
endif()

# Library sources
add_library(ifex-discovery-client STATIC
    src/discovery_client_impl.cpp
)

target_include_directories(ifex-discovery-client
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}/proto
)

# C++17 required
target_compile_features(ifex-discovery-client PUBLIC cxx_std_17)

# Dependencies
target_link_libraries(ifex-discovery-client
    PUBLIC
        Threads::Threads
        protobuf::libprotobuf
        gRPC::grpc++
        ifex-parser
        ifex-common
    PRIVATE
        nlohmann_json::nlohmann_json
        glog::glog
        ifex-proto-generated
)

# Alias for consistent naming
add_library(ifex::discovery-client ALIAS ifex-discovery-client)

# Install targets (use ifex-targets export set for compatibility)
install(TARGETS ifex-discovery-client
    EXPORT ifex-targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/
    DESTINATION include
    FILES_MATCHING PATTERN "*.hpp"
)
