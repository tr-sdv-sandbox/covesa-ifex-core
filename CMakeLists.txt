cmake_minimum_required(VERSION 3.16)
project(ifex-ecosystem VERSION 0.1.0 LANGUAGES CXX)

# C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find common dependencies at top level
find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
find_package(gRPC REQUIRED)
find_package(glog REQUIRED)
find_package(gflags REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(yaml-cpp REQUIRED)

# Options
option(BUILD_CORE "Build IFEX core library" ON)
option(BUILD_REFERENCE_SERVICES "Build reference services" ON)
option(BUILD_TEST_SERVICES "Build test services" ON)
option(BUILD_INTEGRATION_TESTS "Build integration tests" ON)
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)

# Enable testing early so tests in subdirectories can register
if(BUILD_TESTS OR BUILD_INTEGRATION_TESTS)
    enable_testing()
endif()

# Export compile commands for IDEs
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find gRPC plugin for proto generation
find_program(GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin)
if(NOT GRPC_CPP_PLUGIN_EXECUTABLE)
    message(FATAL_ERROR "grpc_cpp_plugin not found")
endif()

# Build protobuf generation first if we're building core or services
if(BUILD_CORE OR BUILD_REFERENCE_SERVICES OR BUILD_TEST_SERVICES)
    # Proto directories:
    #   ifex-generated/common/        - Common type protos (from reference-specs/*/common/)
    #   ifex-generated/vehicle/       - Vehicle service protos (from reference-specs/*/vehicle/)
    #   ifex-generated/cloud/         - Cloud service protos (from reference-specs/*/cloud/)
    #   ifex-generated/test-services/ - Test service protos
    #   internal/                     - Hand-written internal protocols (sync, etc.)
    #   api/                          - Hand-written external API definitions
    set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/proto)
    set(PROTO_IFEX_GENERATED_DIR ${PROTO_DIR}/ifex-generated)
    set(PROTO_IFEX_COMMON_DIR ${PROTO_IFEX_GENERATED_DIR}/common)
    set(PROTO_IFEX_VEHICLE_DIR ${PROTO_IFEX_GENERATED_DIR}/vehicle)
    set(PROTO_IFEX_CLOUD_DIR ${PROTO_IFEX_GENERATED_DIR}/cloud)
    set(PROTO_IFEX_TEST_DIR ${PROTO_IFEX_GENERATED_DIR}/test-services)
    set(PROTO_INTERNAL_DIR ${PROTO_DIR}/internal)
    set(PROTO_API_DIR ${PROTO_DIR}/api)

    # Reference services common include directory (for ifex_content_ids.hpp, etc.)
    set(IFEX_REFERENCE_SERVICES_COMMON_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/reference-services/common/include)

    # Backend transport paths (for integration tests)
    set(IFEX_BACKEND_TRANSPORT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/reference-services/backend-transport)
    set(IFEX_BACKEND_TRANSPORT_CLIENT_INCLUDE ${IFEX_BACKEND_TRANSPORT_DIR}/common/client/include)

    # Check if proto files exist (sample check for IFEX-generated files)
    if(NOT EXISTS ${PROTO_IFEX_VEHICLE_DIR}/discovery-service.proto)
        message(FATAL_ERROR "Proto files not found in ${PROTO_IFEX_VEHICLE_DIR}. Please run ./generate_proto.sh first.")
    endif()

    # Find all proto files from all directories
    file(GLOB PROTO_FILES_COMMON ${PROTO_IFEX_COMMON_DIR}/*.proto)
    file(GLOB PROTO_FILES_VEHICLE ${PROTO_IFEX_VEHICLE_DIR}/*.proto)
    file(GLOB PROTO_FILES_CLOUD ${PROTO_IFEX_CLOUD_DIR}/*.proto)
    file(GLOB PROTO_FILES_TEST ${PROTO_IFEX_TEST_DIR}/*.proto)
    file(GLOB PROTO_FILES_INTERNAL ${PROTO_INTERNAL_DIR}/*.proto)
    file(GLOB PROTO_FILES_API ${PROTO_API_DIR}/*.proto)
    set(PROTO_FILES ${PROTO_FILES_COMMON} ${PROTO_FILES_VEHICLE} ${PROTO_FILES_CLOUD} ${PROTO_FILES_TEST} ${PROTO_FILES_INTERNAL} ${PROTO_FILES_API})

    # Generate C++ sources from proto files
    # Proto files are organized in subdirectories, and protoc preserves the directory structure
    # when using -I on the parent directory to support imports.
    set(PROTO_SRCS)
    set(PROTO_HDRS)
    set(GRPC_SRCS)
    set(GRPC_HDRS)

    foreach(proto_file ${PROTO_FILES})
        get_filename_component(proto_name ${proto_file} NAME_WE)
        get_filename_component(proto_dir ${proto_file} DIRECTORY)

        # Determine the output subdirectory based on the proto file location
        # IFEX-generated protos: preserve subdirectory structure (common/, vehicle/, cloud/, test-services/)
        # Internal/API protos: flat structure (no subdirectory)
        if(proto_file MATCHES "${PROTO_IFEX_GENERATED_DIR}")
            # Get relative path from PROTO_IFEX_GENERATED_DIR
            file(RELATIVE_PATH proto_subdir ${PROTO_IFEX_GENERATED_DIR} ${proto_dir})
            set(proto_out_dir "${CMAKE_BINARY_DIR}/proto/${proto_subdir}")
        else()
            set(proto_out_dir "${CMAKE_BINARY_DIR}/proto")
            set(proto_subdir "")
        endif()

        if(proto_subdir)
            set(proto_src "${CMAKE_BINARY_DIR}/proto/${proto_subdir}/${proto_name}.pb.cc")
            set(proto_hdr "${CMAKE_BINARY_DIR}/proto/${proto_subdir}/${proto_name}.pb.h")
            set(grpc_src "${CMAKE_BINARY_DIR}/proto/${proto_subdir}/${proto_name}.grpc.pb.cc")
            set(grpc_hdr "${CMAKE_BINARY_DIR}/proto/${proto_subdir}/${proto_name}.grpc.pb.h")
        else()
            set(proto_src "${CMAKE_BINARY_DIR}/proto/${proto_name}.pb.cc")
            set(proto_hdr "${CMAKE_BINARY_DIR}/proto/${proto_name}.pb.h")
            set(grpc_src "${CMAKE_BINARY_DIR}/proto/${proto_name}.grpc.pb.cc")
            set(grpc_hdr "${CMAKE_BINARY_DIR}/proto/${proto_name}.grpc.pb.h")
        endif()

        add_custom_command(
            OUTPUT ${proto_src} ${proto_hdr} ${grpc_src} ${grpc_hdr}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${proto_out_dir}
            COMMAND ${Protobuf_PROTOC_EXECUTABLE}
            ARGS --grpc_out=${CMAKE_BINARY_DIR}/proto
                 --cpp_out=${CMAKE_BINARY_DIR}/proto
                 --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}
                 -I ${PROTO_IFEX_GENERATED_DIR}
                 -I ${PROTO_INTERNAL_DIR}
                 -I ${PROTO_API_DIR}
                 ${proto_file}
            DEPENDS ${proto_file}
            COMMENT "Generating C++ code from ${proto_file}"
        )

        list(APPEND PROTO_SRCS ${proto_src})
        list(APPEND PROTO_HDRS ${proto_hdr})
        list(APPEND GRPC_SRCS ${grpc_src})
        list(APPEND GRPC_HDRS ${grpc_hdr})
    endforeach()

    # Create library for generated proto code
    add_library(ifex-proto-generated STATIC
        ${PROTO_SRCS}
        ${GRPC_SRCS}
    )

    target_include_directories(ifex-proto-generated
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/proto>
            $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/proto/common>
            $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/proto/vehicle>
            $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/proto/cloud>
            $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/proto/test-services>
            $<INSTALL_INTERFACE:include/ifex/proto>
    )

    target_link_libraries(ifex-proto-generated
        PUBLIC
            protobuf::libprotobuf
            gRPC::grpc++
    )

    # Install the proto library (needed for export set)
    include(GNUInstallDirs)
    install(TARGETS ifex-proto-generated
        EXPORT ifex-targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )

    # Install generated proto headers
    install(DIRECTORY ${CMAKE_BINARY_DIR}/proto/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ifex/proto
        FILES_MATCHING PATTERN "*.h"
    )
endif()

# Common types library (standalone, no proto dependency)
add_subdirectory(libs/common)

# Parser library (depends on common)
add_subdirectory(libs/parser)

# Note: Scheduler library (ifex-scheduler) is now under reference-services/scheduler/common/

# Discovery client library
add_subdirectory(reference-services/discovery/common/client)

# Reference services
if(BUILD_REFERENCE_SERVICES)
    add_subdirectory(reference-services)
endif()

# Test services
if(BUILD_TEST_SERVICES)
    add_subdirectory(test-services)
endif()

# Test client
if(BUILD_TEST_SERVICES OR BUILD_REFERENCE_SERVICES)
    add_subdirectory(test-client)
endif()

# Tests - includes both unit and integration tests
if(BUILD_TESTS OR BUILD_INTEGRATION_TESTS)
    enable_testing()
    find_package(GTest REQUIRED)
    add_subdirectory(tests)
endif()
