add_executable(ifex-dispatcher-service
    src/dispatcher_server.cpp
    src/dynamic_caller_impl.cpp
    src/main.cpp
)

target_include_directories(ifex-dispatcher-service
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ifex-dispatcher-service
    PRIVATE
        ifex-core
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