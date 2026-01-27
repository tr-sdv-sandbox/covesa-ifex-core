# Dispatcher Service - Service-Centric Organization
#
# Structure:
#   common/       - Shared dispatcher utilities (if any)
#   vehicle/      - Vehicle-side components
#     service/    - Dispatcher service (gRPC server, JSON<->Protobuf routing)
#     rpc-bridge/ - RPC bridge for cloud-initiated calls
#   cloud/        - Cloud-side components (test-only)
#     service/    - Cloud dispatcher service

cmake_minimum_required(VERSION 3.16)

# Vehicle-side components (always built)
add_subdirectory(vehicle)

# Cloud-side components (only when tests enabled and not cross-compiling)
if(BUILD_TESTS OR BUILD_INTEGRATION_TESTS)
    if(NOT CMAKE_CROSSCOMPILING)
        add_subdirectory(cloud)
    endif()
endif()
