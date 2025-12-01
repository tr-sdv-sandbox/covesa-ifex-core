cmake_minimum_required(VERSION 3.16)
project(ifex-scheduler-service VERSION 2.0.0 LANGUAGES CXX)

# Create the scheduler service executable
add_executable(ifex-scheduler-service
    src/main.cpp
    src/scheduler_server.cpp
)

# Include directories
target_include_directories(ifex-scheduler-service
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Link libraries
target_link_libraries(ifex-scheduler-service
    PRIVATE
        ifex-proto-generated
        ifex-core
        gRPC::grpc++
        gRPC::grpc++_reflection
        ${PROTOBUF_LIBRARIES}
        yaml-cpp
        nlohmann_json::nlohmann_json
        glog::glog
        Threads::Threads
)

# Install
install(TARGETS ifex-scheduler-service
    RUNTIME DESTINATION bin
)
