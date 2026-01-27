cmake_minimum_required(VERSION 3.16)
project(ifex-scheduler-service VERSION 2.0.0 LANGUAGES CXX)

# =============================================================================
# Library (used by service and tests)
# =============================================================================

add_library(ifex-scheduler-service-lib STATIC
    src/scheduler_server.cpp
)

target_include_directories(ifex-scheduler-service-lib
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ifex-scheduler-service-lib
    PUBLIC
        ifex-proto-generated
        ifex-core
        ifex-sync
        ifex-scheduler
        gRPC::grpc++
        ${PROTOBUF_LIBRARIES}
        yaml-cpp
        nlohmann_json::nlohmann_json
        glog::glog
    PRIVATE
        Threads::Threads
)

# =============================================================================
# Service Executable
# =============================================================================

add_executable(ifex-scheduler-service
    src/main.cpp
)

target_include_directories(ifex-scheduler-service
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ifex-scheduler-service
    PRIVATE
        ifex-scheduler-service-lib
        gRPC::grpc++_reflection
        Threads::Threads
)

# Install
install(TARGETS ifex-scheduler-service
    RUNTIME DESTINATION bin
)

install(TARGETS ifex-scheduler-service-lib
    ARCHIVE DESTINATION lib
)
