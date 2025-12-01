add_executable(ifex-discovery-service
    src/discovery_server.cpp
    src/main.cpp
)

target_include_directories(ifex-discovery-service
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ifex-discovery-service
    PRIVATE
        ifex-core
        ifex-proto-generated
        gRPC::grpc++
        protobuf::libprotobuf
        glog::glog
)

# Install
install(TARGETS ifex-discovery-service
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)