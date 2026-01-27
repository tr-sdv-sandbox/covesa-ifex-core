add_executable(ifex-dispatcher-service
    src/dispatcher_server.cpp
    src/dynamic_caller_impl.cpp
    src/main.cpp
)

target_include_directories(ifex-dispatcher-service
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${PROTO_IFEX_VEHICLE_DIR}
)

target_link_libraries(ifex-dispatcher-service
    PRIVATE
        ifex-parser
        ifex-common
        ifex-discovery-client
        ifex-proto-generated
        gRPC::grpc++
        protobuf::libprotobuf
        glog::glog
        nlohmann_json::nlohmann_json
)

# Install
install(TARGETS ifex-dispatcher-service
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)