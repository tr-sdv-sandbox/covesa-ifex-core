# Scheduler Service - Service-Centric Organization
#
# Structure:
#   common/       - Shared scheduler library (Job, Store, Hash, VersionVector, SyncEngine)
#   vehicle/      - Vehicle-side components
#     service/    - Scheduler service (gRPC server)
#     sync-bridge/ - Vehicle-side sync bridge
#   cloud/        - Cloud-side components (test-only)
#     service/    - Cloud scheduler service
#     sync-bridge/ - Cloud-side sync bridge

cmake_minimum_required(VERSION 3.16)

# Common scheduler library (shared between vehicle and cloud)
add_subdirectory(common)

# Vehicle-side components (always built)
add_subdirectory(vehicle)

# Cloud-side components (only when tests enabled and not cross-compiling)
if(BUILD_TESTS OR BUILD_INTEGRATION_TESTS)
    if(NOT CMAKE_CROSSCOMPILING)
        add_subdirectory(cloud)
    endif()
endif()
