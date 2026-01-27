# Backend Transport Service - Service-Centric Organization
#
# Structure:
#   common/client/ - Backend transport client library
#   vehicle/       - Vehicle-side components
#     service/     - Backend transport server (MQTT bridge)
#   cloud/         - Cloud-side components (test-only)
#     service/     - Cloud backend transport

cmake_minimum_required(VERSION 3.16)

# Common client library (shared by all services)
add_subdirectory(common/client)

# Vehicle-side components (always built)
add_subdirectory(vehicle)

# Cloud-side components (only when tests enabled and not cross-compiling)
if(BUILD_TESTS OR BUILD_INTEGRATION_TESTS)
    if(NOT CMAKE_CROSSCOMPILING)
        add_subdirectory(cloud)
    endif()
endif()
