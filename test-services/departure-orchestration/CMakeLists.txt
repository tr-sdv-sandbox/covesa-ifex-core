add_executable(departure-orchestration-service
    src/departure_orchestration_server.cpp
    src/main.cpp
)

target_include_directories(departure-orchestration-service
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(departure-orchestration-service
    PRIVATE
        ifex-core
        ifex-proto-generated
        gRPC::grpc++
        protobuf::libprotobuf
        glog::glog
        nlohmann_json::nlohmann_json
        yaml-cpp
        Threads::Threads
)

# Copy IFEX schema to build directory
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/departure-orchestration-service.ifex.yml
    ${CMAKE_CURRENT_BINARY_DIR}/departure-orchestration-service.ifex.yml
    COPYONLY
)

# Install
install(TARGETS departure-orchestration-service
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES departure-orchestration-service.ifex.yml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/ifex/services
)