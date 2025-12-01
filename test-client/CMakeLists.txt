# Test client for IFEX Core

add_executable(ifex-test-client
    test_client.cpp
)

target_link_libraries(ifex-test-client
    PRIVATE
        ifex-proto-generated
        gRPC::grpc++
        protobuf::libprotobuf
        glog::glog
        nlohmann_json::nlohmann_json
)

# Generic dispatcher client
add_executable(ifex-dispatcher-client
    dispatcher_client.cpp
)

target_link_libraries(ifex-dispatcher-client
    PRIVATE
        ifex-proto-generated
        gRPC::grpc++
        protobuf::libprotobuf
        glog::glog
        nlohmann_json::nlohmann_json
)