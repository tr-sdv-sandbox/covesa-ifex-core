cmake_minimum_required(VERSION 3.16)
project(beverage-service)

# Find required packages
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)
find_package(glog REQUIRED)
find_package(gflags REQUIRED)
find_package(yaml-cpp REQUIRED)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_BINARY_DIR}/proto
)

# Source files
set(SOURCES
    src/beverage_server.cpp
    src/main.cpp
)

# Create executable
add_executable(beverage-service ${SOURCES})

# Link libraries
target_link_libraries(beverage-service
    PRIVATE
        ifex-core
        ifex-proto-generated
        gRPC::grpc++
        protobuf::libprotobuf
        glog::glog
        yaml-cpp
        Threads::Threads
)

# Set C++ standard
target_compile_features(beverage-service PRIVATE cxx_std_17)

# Copy IFEX schema to build directory
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/beverage-service.ifex.yml
    ${CMAKE_CURRENT_BINARY_DIR}/beverage-service.ifex.yml
    COPYONLY
)