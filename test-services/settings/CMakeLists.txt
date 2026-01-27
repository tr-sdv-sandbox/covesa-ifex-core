add_executable(ifex-settings-service
    main.cpp
    settings_server.cpp
    settings_server.hpp
)

target_include_directories(ifex-settings-service
    PRIVATE
        ${CMAKE_SOURCE_DIR}/proto/ifex-generated/test-services
)

target_link_libraries(ifex-settings-service
    PRIVATE
        ifex-parser
        ifex-common
        ifex-discovery-client
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
    ${CMAKE_CURRENT_SOURCE_DIR}/settings-service.ifex.yml
    ${CMAKE_CURRENT_BINARY_DIR}/settings-service.ifex.yml
    COPYONLY
)


# Install
install(TARGETS ifex-settings-service
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES settings-service.ifex.yml
    DESTINATION ${CMAKE_INSTALL_DATADIR}/ifex/services
)