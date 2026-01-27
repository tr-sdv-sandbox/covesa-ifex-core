# Backend Transport Client Library
#
# Provides gRPC client for communicating with Backend Transport service.
# Used by sync bridges and other components that need to publish/subscribe.

cmake_minimum_required(VERSION 3.16)

# Guard against multiple inclusion
if(TARGET ifex-backend-transport-client)
    return()
endif()

# Library sources
add_library(ifex-backend-transport-client STATIC
    src/backend_transport_client.cpp
)

target_include_directories(ifex-backend-transport-client
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/proto
)

# C++17 required
target_compile_features(ifex-backend-transport-client PUBLIC cxx_std_17)

# Dependencies
target_link_libraries(ifex-backend-transport-client
    PUBLIC
        ifex-proto-generated
        gRPC::grpc++
    PRIVATE
        glog::glog
        Threads::Threads
)

# Alias for consistent naming
add_library(ifex::backend-transport-client ALIAS ifex-backend-transport-client)

# Install targets
install(TARGETS ifex-backend-transport-client
    EXPORT ifex-targets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/
    DESTINATION include
    FILES_MATCHING PATTERN "*.hpp"
)
