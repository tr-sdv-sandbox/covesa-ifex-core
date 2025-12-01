# Defrost service
cmake_minimum_required(VERSION 3.14)

# Find required packages (already found by parent)
# Just create the executable

add_executable(defrost-service
    src/defrost_server.cpp
    src/main.cpp
)

target_include_directories(defrost-service
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(defrost-service
    PRIVATE
        ifex-core
        ifex-proto-generated
        gRPC::grpc++
        protobuf::libprotobuf
        glog::glog
        nlohmann_json::nlohmann_json
)

# Copy IFEX file to build directory
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/defrost-service.ifex.yml
    ${CMAKE_CURRENT_BINARY_DIR}/defrost-service.ifex.yml
    COPYONLY
)