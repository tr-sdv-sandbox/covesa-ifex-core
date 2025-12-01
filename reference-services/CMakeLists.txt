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

# Copy IFEX files to build directory as resources
file(GLOB IFEX_FILES "${CMAKE_CURRENT_SOURCE_DIR}/ifex/*.yml")
foreach(IFEX_FILE ${IFEX_FILES})
    get_filename_component(IFEX_NAME ${IFEX_FILE} NAME)
    configure_file(${IFEX_FILE} ${CMAKE_CURRENT_BINARY_DIR}/ifex/${IFEX_NAME} COPYONLY)
    # Also copy to the main binary directory for easier access
    configure_file(${IFEX_FILE} ${CMAKE_BINARY_DIR}/ifex/${IFEX_NAME} COPYONLY)
endforeach()