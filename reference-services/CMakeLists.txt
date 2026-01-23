cmake_minimum_required(VERSION 3.16)
project(ifex-reference-services)

# Find required packages
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)
find_package(glog REQUIRED)
find_package(nlohmann_json REQUIRED)

# The proto library is now built in the proto directory
# Just include the generated headers
include_directories(${CMAKE_BINARY_DIR}/proto)

# Build discovery service
add_subdirectory(discovery)

# Build dispatcher service
add_subdirectory(dispatcher)

# Build scheduler service
add_subdirectory(scheduler)

# Build backend transport service
add_subdirectory(backend-transport)

# Build dispatcher bridge service
add_subdirectory(dispatcher-bridge)

# Build discovery sync bridge service
add_subdirectory(discovery-sync-bridge)

# Build scheduler sync bridge service
add_subdirectory(scheduler-sync-bridge)

# Copy IFEX specs to build directory as resources
file(GLOB VEHICLE_IFEX_FILES "${CMAKE_SOURCE_DIR}/specs/vehicle/*.ifex.yml")
foreach(IFEX_FILE ${VEHICLE_IFEX_FILES})
    get_filename_component(IFEX_NAME ${IFEX_FILE} NAME)
    configure_file(${IFEX_FILE} ${CMAKE_CURRENT_BINARY_DIR}/ifex/${IFEX_NAME} COPYONLY)
    configure_file(${IFEX_FILE} ${CMAKE_BINARY_DIR}/ifex/${IFEX_NAME} COPYONLY)
endforeach()