# Cloud Discovery Service
# In-memory service for aggregating vehicle service registries

# Library
add_library(ifex-cloud-discovery-service STATIC
    src/cloud_discovery_service.cpp
)

target_include_directories(ifex-cloud-discovery-service PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ifex-cloud-discovery-service PUBLIC
    ifex-proto-generated
    gRPC::grpc++
    glog::glog
    yaml-cpp
)

# Executable
add_executable(ifex-cloud-discovery-service-bin
    src/main.cpp
)

set_target_properties(ifex-cloud-discovery-service-bin PROPERTIES
    OUTPUT_NAME ifex-cloud-discovery-service
)

target_link_libraries(ifex-cloud-discovery-service-bin PRIVATE
    ifex-cloud-discovery-service
    gflags
)

# Install
install(TARGETS ifex-cloud-discovery-service-bin
    RUNTIME DESTINATION bin
)
