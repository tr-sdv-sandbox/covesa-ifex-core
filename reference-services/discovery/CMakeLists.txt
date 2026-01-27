# Discovery Service - Service-Centric Organization
#
# Structure:
#   common/client/ - Discovery client library (gRPC client)
#   vehicle/       - Vehicle-side components
#     service/     - Discovery service (gRPC server, service registry)
#     sync-bridge/ - Hash-based schema synchronization
#   cloud/         - Cloud-side components (test-only)
#     service/     - Cloud discovery service (fleet aggregator)

cmake_minimum_required(VERSION 3.16)

# Note: Discovery client library (common/client) is built early from root CMakeLists.txt
# to satisfy ifex-core dependency

# Vehicle-side components (always built)
add_subdirectory(vehicle)

# Cloud-side components (only when tests enabled and not cross-compiling)
if(BUILD_TESTS OR BUILD_INTEGRATION_TESTS)
    if(NOT CMAKE_CROSSCOMPILING)
        add_subdirectory(cloud)
    endif()
endif()
