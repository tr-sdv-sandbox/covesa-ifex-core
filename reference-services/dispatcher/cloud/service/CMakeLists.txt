# Cloud Dispatcher Service
# In-memory service for cloud-initiated RPC calls to vehicles

# Library
add_library(ifex-cloud-dispatcher-service STATIC
    src/cloud_dispatcher_service.cpp
)

target_include_directories(ifex-cloud-dispatcher-service PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ifex-cloud-dispatcher-service PUBLIC
    ifex-proto-generated
    gRPC::grpc++
    glog::glog
)

# Executable
add_executable(ifex-cloud-dispatcher-service-bin
    src/main.cpp
)

set_target_properties(ifex-cloud-dispatcher-service-bin PROPERTIES
    OUTPUT_NAME ifex-cloud-dispatcher-service
)

target_link_libraries(ifex-cloud-dispatcher-service-bin PRIVATE
    ifex-cloud-dispatcher-service
    gflags
)

# Install
install(TARGETS ifex-cloud-dispatcher-service-bin
    RUNTIME DESTINATION bin
)
