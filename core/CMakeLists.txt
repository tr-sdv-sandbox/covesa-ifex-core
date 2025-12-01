# IFEX Core Library
# Note: This is included as a subdirectory, so packages are already found

# Create library
add_library(ifex-core
    src/parser_impl.cpp
    src/discovery_client_impl.cpp
    src/network_utils.cpp
)

# Set include directories
target_include_directories(ifex-core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}/proto  # For generated protobuf headers
)

# Link dependencies
target_link_libraries(ifex-core
    PUBLIC
        Threads::Threads
        protobuf::libprotobuf
        gRPC::grpc++
    PRIVATE
        yaml-cpp
        nlohmann_json::nlohmann_json
        glog::glog
        ifex-proto-generated  # Link to generated protobuf code
)

# Set properties
set_target_properties(ifex-core PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# Installation
include(GNUInstallDirs)

install(TARGETS ifex-core
    EXPORT ifex-core-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Export targets
install(EXPORT ifex-core-targets
    FILE ifex-core-targets.cmake
    NAMESPACE ifex::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ifex-core
)

# Create package configuration
include(CMakePackageConfigHelpers)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ifex-core-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ifex-core-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ifex-core
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ifex-core-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/ifex-core-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/ifex-core-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ifex-core
)