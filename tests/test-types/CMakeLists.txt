# Test Types Service executable
add_executable(ifex-test-types-service
    main.cpp
    test_types_server.cpp
)

target_include_directories(ifex-test-types-service
    PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_BINARY_DIR}/proto
)

target_link_libraries(ifex-test-types-service
    PRIVATE
        ifex-core
        gRPC::grpc++
        protobuf::libprotobuf
        glog::glog
        nlohmann_json::nlohmann_json
)

# Copy IFEX file to build directory for runtime access
add_custom_command(
    TARGET ifex-test-types-service POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_CURRENT_SOURCE_DIR}/test-types-service.ifex.yml
            ${CMAKE_CURRENT_BINARY_DIR}/test-types-service.ifex.yml
)

# Also copy to the parent ifex directory for discovery
add_custom_command(
    TARGET ifex-test-types-service POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
            ${CMAKE_CURRENT_SOURCE_DIR}/test-types-service.ifex.yml
            ${CMAKE_BINARY_DIR}/ifex/test_types_service.ifex.yml
)